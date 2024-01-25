/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause) */
/*
 * Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <rdma/rdma_cma.h>

#include "cmperf.h"

struct server_task_client {
	struct rdma_cm_id *cid;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_mr *mr;
	int state;
};

struct server_task {
	pthread_t tid;
	unsigned int sock_port;

	struct rdma_event_channel *ech;
	struct rdma_cm_id *sid;
	struct server_task_client *clients;

	unsigned int num_req_handled;
	unsigned int num_conn_established;
	unsigned int num_conn_disconnected;
};

static struct server_task *servers;
static unsigned int server_task_num;
static volatile unsigned int server_running_task_num;

static unsigned int server_conn_num_per_task;

static int server_task_init(struct server_task *stask)
{
	int i;

	stask->clients = calloc(server_conn_num_per_task,
				    sizeof(*stask->clients));
	if (!stask->clients) {
		err("calloc(%d/%ld) failed: %d\n",
		    server_conn_num_per_task, sizeof(*stask->clients), errno);
		return errno;
	}

	for (i = 0; i < server_conn_num_per_task; i++)
		stask->clients[i].state = -1;

	return 0;
}

static unsigned char test_buf[1024];
static int create_conn_qp(struct server_task_client *client)
{
	struct ibv_qp_init_attr attr = {
		.cap = {
			.max_send_wr = 32,
			.max_recv_wr = 32,
			.max_send_sge = 1,
			.max_recv_sge = 1,
			.max_inline_data = 64,
		},
		.qp_type = IBV_QPT_RC,
	};
	int err;

	client->pd = ibv_alloc_pd(client->cid->verbs);
	if (!client->pd) {
		perror("ibv_alloc_pd");
		return -1;
	}

	client->mr = ibv_reg_mr(client->pd, test_buf, sizeof(test_buf),
				IBV_ACCESS_LOCAL_WRITE);
	if (!client->mr) {
		perror("ibv_reg_mr");
		goto fail_reg_mr;
	}

	client->cq = ibv_create_cq(client->cid->verbs, 64, NULL, NULL, 0);
	if (!client->cq) {
		perror("client->cq");
		goto fail_create_cq;
	}

	attr.send_cq = client->cq;
	attr.recv_cq = client->cq;
	err = rdma_create_qp(client->cid, client->pd, &attr);
	if (err) {
		perror("rdma_create_qp");
		goto fail_create_qp;
	}

	return 0;

fail_create_qp:
	ibv_destroy_cq(client->cq);
	client->cq = NULL;
fail_create_cq:
	ibv_dereg_mr(client->mr);
	client->mr = NULL;
fail_reg_mr:
	ibv_dealloc_pd(client->pd);
	client->pd = NULL;

	return -1;
}

static void destroy_conn_qp(struct server_task_client *client)
{
	rdma_destroy_qp(client->cid);
	ibv_destroy_cq(client->cq);
	client->cq = NULL;
	ibv_dereg_mr(client->mr);
	client->mr = NULL;
	ibv_dealloc_pd(client->pd);
	client->pd = NULL;
	rdma_destroy_id(client->cid);
	client->state = -1;
}

static int handle_event_req(struct server_task *server, struct rdma_cm_event *e)
{
	struct rdma_conn_param param = {};
	unsigned long slot;
	int i, err;

	for (i = 0; i < server->num_req_handled; i++)
		if (server->clients[i].cid == e->id) {
			err("Duplicate request received, last %d\n", i);
			return -1;
		}

	slot = server->num_req_handled;
	server->clients[slot].state = e->event;
	server->clients[slot].cid = e->id;
	server->clients[slot].cid->context = (void *)slot;

	err = create_conn_qp(server->clients + slot);
	if (err) {
		err("create_qp failed at slot %ld\n", slot);
		return err;
	}

	err = rdma_accept(e->id, &param);
	if (err) {
		perror("rdma_accept");
		return err;
	}

	server->num_req_handled++;
	return 0;
}

static void handle_event_established(struct server_task *server,
				    struct rdma_cm_event *e)
{
	unsigned long client_slot = (unsigned long)e->id->context;

	assert(client_slot < server_conn_num_per_task);

	server->clients[client_slot].state = e->event;
	server->num_conn_established++;
}

static int server_conn_establish(unsigned int taskid)
{
	struct server_task *server = servers + taskid;
	struct sockaddr_in sin = {};
	struct rdma_cm_event *e;
	int err, i;

	server->ech = rdma_create_event_channel();
	if (!server->ech) {
		perror("rdma_create_event_channel");
		return errno;
	}

	err = rdma_create_id(server->ech, &server->sid, server, RDMA_PS_TCP);
	if (err) {
		perror("rdma_create_id");
		goto fail_create_id;
	}

	sin.sin_family = AF_INET;
	sin.sin_port = htons(server->sock_port);
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	err = rdma_bind_addr(server->sid, (struct sockaddr *)&sin);
	if (err) {
		perror("rdma_bind_addr");
		goto fail_bind_addr;
	}

	err = rdma_listen(server->sid, 1024);
	if (err) {
		perror("rdma_listen");
		goto fail_bind_addr;
	}

	while (server->num_conn_established < server_conn_num_per_task) {
		err = rdma_get_cm_event(server->ech, &e);
		if (err) {
			perror("rdma_get_cm_event");
			goto fail_event;
		}

		switch (e->event) {
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			err = handle_event_req(server, e);
			if (err) {
				err("Handle REQUEST event failed.\n");
				goto fail_event;
			}
			break;

		case RDMA_CM_EVENT_ESTABLISHED:
			handle_event_established(server, e);
			break;

		default:
			err("Unexpected event %d\n", e->event);
			err = -1;
			goto fail_event;
		}

		rdma_ack_cm_event(e);
	}

	return 0;


fail_event:
	rdma_ack_cm_event(e);
	for (i = 0; i < server->num_req_handled; i++)
		destroy_conn_qp(server->clients + i);

fail_bind_addr:
	rdma_destroy_id(server->sid);
	server->sid = NULL;
fail_create_id:
	rdma_destroy_event_channel(server->ech);
	server->ech = NULL;

	return err;
}

static int server_conn_disconnect(unsigned int taskid)
{
	struct server_task *server = servers + taskid;
	unsigned long client_slot;
	struct rdma_cm_event *e;
	int err;

	while (server->num_conn_disconnected < server->num_conn_established) {
		err = rdma_get_cm_event(server->ech, &e);
		if (err) {
			perror("rdma_get_cm_event");
			break;
		}

		if (e->event != RDMA_CM_EVENT_DISCONNECTED) {
			err("Unexpected event %d, skip\n", e->event);
			goto next_event;
		}


		client_slot = (unsigned long)e->id->context;
		assert(client_slot < server_conn_num_per_task);
		if (server->clients[client_slot].state != RDMA_CM_EVENT_ESTABLISHED) {
			err("Unexpected DISCONNECTED event for slot %ld (current state %d), ignore\n",
				       client_slot, server->clients[client_slot].state);
			goto next_event;
		}

		server->clients[client_slot].state = e->event;
		server->num_conn_disconnected++;

next_event:
		rdma_ack_cm_event(e);
	}

	return err;
}

static void server_destroy_clients(struct server_task *server)
{
	int i;

	for (i = 0; i < server->num_conn_disconnected; i++)
		destroy_conn_qp(server->clients + i);

	rdma_destroy_id(server->sid);
	rdma_destroy_event_channel(server->ech);
}

static int do_run_server_task(unsigned int taskid)
{
	int err;

	err = server_conn_establish(taskid);
	if (err)
		return err;

	info("Task %d: all connections have been established\n", taskid);

	err = server_conn_disconnect(taskid);
	if (err)
		return err;

	return 0;
}

static void *server_task_run(void *arg)
{
	unsigned long taskid = (unsigned long)arg;
	struct server_task *stask = servers + taskid;
	int err;

	info("  Task %ld started..\n", taskid);
	err = server_task_init(stask);
	if (err) {
		err("Failed to initialize server task %ld.\n", taskid);
		goto out;
	}

	err = do_run_server_task(taskid);
	if (err)
		goto out;

	server_destroy_clients(stask);

out:
	free(stask->clients);
	server_running_task_num--;

	info("Task %ld: all connections have been disconnected, exit (left running %d)\n",
	     taskid, server_running_task_num);
	return NULL;
}

int cmperf_start_server(unsigned int first_port,
			unsigned int task_num,
			unsigned int conn_num_per_task)
{
	int i, j, err;

	info("Server: %d tasks each with %d connections, first ip port %d\n",
	     task_num, conn_num_per_task, first_port);

	if (!task_num || !conn_num_per_task) {
		err("Invalid parameter: Task num %d, connection num per task %d\n",
		       task_num, conn_num_per_task);
		return EINVAL;
	}

	server_task_num = task_num;
	server_conn_num_per_task = conn_num_per_task;
	servers = calloc(task_num, sizeof(*servers));
	if (!servers) {
		err("calloc(%d/%ld) failed: %d\n",
		       task_num, sizeof(*servers), errno);
		return errno;
	}

	server_running_task_num = task_num;
	for (i = 0; i < task_num; i++) {
		servers[i].sock_port = first_port + i;
		err = pthread_create(&servers[i].tid, NULL,
				     server_task_run, (void *)(unsigned long)i);
		if (err) {
			perror("pthread_create");
			goto fail;
		}
	}

	while (server_running_task_num)
		sleep(1);

	info("Server done.\n");
	free(servers);
	return 0;

fail:
	for (j = 0; j < i; j++)
		pthread_cancel(servers[j].tid);
	free(servers);
	return err;
}
