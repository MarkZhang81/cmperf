/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause) */
/*
 * Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved
 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include <rdma/rdma_cma.h>

#include "cmperf.h"

struct perf_client {
	struct rdma_cm_id *id;
	struct ibv_pd *pd;
	struct ibv_mr *mr;
	struct ibv_cq *cq;
	int state;

	struct timeval tv_start;
	struct timeval tv_addr_resolved;
	struct timeval tv_resolve_route_start;
	struct timeval tv_route_resolved;
	struct timeval tv_connect_start;
	struct timeval tv_established;

	struct timeval tv_dis_start;
	struct timeval tv_disconnected;

	uint64_t pd_used, mr_used, cq_used, qp_used;
};

struct perf_client_task {
	struct sockaddr server;
	pthread_t main_thread;

	struct perf_client *clients;
	struct rdma_event_channel *ech;
	pthread_t event_thread;
	int event_thread_err;
	sem_t sem;

	unsigned int num_addr_resolved;
	unsigned int num_route_resolved;
	unsigned int num_conn_established;
	unsigned int num_conn_disconnected;

	struct timeval t_start;
	struct timeval t_addr_resolved;
	struct timeval t_route_resolved;
	struct timeval t_established;

	struct timeval t_dis_start;
	struct timeval t_disconnected;
};

static unsigned client_task_num;
static unsigned int client_conn_num_per_task;
static volatile unsigned int client_running_task_num;
static unsigned int client_failed_task_num;
static struct perf_client_task *ctasks;


unsigned int num_tasks_conn_established;
struct timeval prog_start;
struct timeval prog_conn_established;

/* Data buffer is not important as we don't send/recv any useful data */
static unsigned char buf[1024];

static struct ibv_qp_init_attr init_attr = {
	.cap = {
		.max_send_wr = 4,
		.max_recv_wr = 4,
		.max_send_sge = 1,
		.max_recv_sge = 1,
	},
	.qp_type = IBV_QPT_RC,
};

static uint64_t get_time_used(const struct timeval *t1, const struct timeval *t2)
{
	return (t2->tv_sec - t1->tv_sec) * 1000000 + (t2->tv_usec - t1->tv_usec);
}

#define dump(args...) fprintf(stdout, ##args)

static int create_qp(struct perf_client *c)
{
	int err;

	struct timeval t1 = {}, t2 = {}, t3 = {}, t4 = {}, t5 = {};

	gettimeofday(&t1, NULL);
	c->pd = ibv_alloc_pd(c->id->verbs);
	if (!c->pd) {
		perror("ibv_alloc_pd");
		return -1;
	}

	gettimeofday(&t2, NULL);
	c->mr = ibv_reg_mr(c->pd, buf, sizeof(buf), IBV_ACCESS_LOCAL_WRITE);
	if (!c->mr) {
		perror("ibv_reg_mr");
		goto fail_reg_mr;
	}

	gettimeofday(&t3, NULL);
	c->cq = ibv_create_cq(c->id->verbs, 32, NULL, NULL, 0);
	if (!c->cq) {
		perror("ibv_create_cq");
		goto fail_create_cq;
	}

	gettimeofday(&t4, NULL);
	init_attr.send_cq = c->cq;
	init_attr.recv_cq = c->cq;
	err = rdma_create_qp(c->id, c->pd, &init_attr);
	if (err) {
		perror("rdma_create_qp");
		goto fail_create_qp;
	}

	gettimeofday(&t5, NULL);
	c->pd_used = get_time_used(&t1, &t2);
	c->mr_used = get_time_used(&t2, &t3);
	c->cq_used = get_time_used(&t3, &t4);
	c->qp_used = get_time_used(&t4, &t5);
	return 0;

fail_reg_mr:
	ibv_dealloc_pd(c->pd);
	c->pd = NULL;

fail_create_cq:
	ibv_dereg_mr(c->mr);
	c->cq = NULL;

fail_create_qp:
	ibv_destroy_cq(c->cq);

	return -1;
}

static char *rdmacm_event_str[] = {
	"ADDR_RESOLVED",
	"ADDR_ERROR",
	"ROUTE_RESOLVED",
	"ROUTE_ERROR",
	"CONNECT_REQUEST",
	"CONNECT_RESPONSE",
	"CONNECT_ERROR",
	"UNREACHABLE",
	"REJECTED",
	"ESTABLISHED",
	"DISCONNECTED",
	"DEVICE_REMOVAL",
	"MULTICAST_JOIN",
	"MULTICAST_ERROR",
	"ADDR_CHANGE",
	"TIMEWAIT_EXIT",
};

static char *event2str(enum rdma_cm_event_type event)
{
	return rdmacm_event_str[event];
}

static int client_handle_event(struct perf_client_task *ctask,
			       struct rdma_cm_event *event)
{
	unsigned long index = (unsigned long)event->id->context;
	struct rdma_conn_param param = {};
	//struct perf_client *c;
	int err;

	assert(event->id == ctask->clients[index].id);

	ctask->clients[index].state = event->event;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		gettimeofday(&ctask->clients[index].tv_addr_resolved, NULL);

		ctask->num_addr_resolved++;
		if (ctask->num_addr_resolved == client_conn_num_per_task)
			gettimeofday(&ctask->t_addr_resolved, NULL);

		err = rdma_resolve_route(event->id, 16000);
		if (err) {
			perror("rdma_resolve_route");
			return err;
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		gettimeofday(&ctask->clients[index].tv_route_resolved, NULL);

		ctask->num_route_resolved++;
		if (ctask->num_route_resolved == client_conn_num_per_task)
			gettimeofday(&ctask->t_route_resolved, NULL);

		err = create_qp(ctask->clients + index);
		if (err) {
			perror("rdma_create_qp");
			return err;
		}

		gettimeofday(&ctask->clients[index].tv_connect_start, NULL);
		err = rdma_connect(event->id, &param);
		if (err) {
			perror("rdma_connect");
			return err;
		}

		/*
		c = ctask->clients + index;
		dump("From route_resolved to connect_start %ld: pd, mr, cq, qp = [%ld, %ld, %ld, %ld]\n",
		     get_time_used(&c->tv_route_resolved, &c->tv_connect_start),
		     c->pd_used, c->mr_used, c->cq_used, c->qp_used);
		*/
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		gettimeofday(&ctask->clients[index].tv_established, NULL);

		ctask->num_conn_established++;
		if (ctask->num_conn_established == client_conn_num_per_task) {
			gettimeofday(&ctask->t_established, NULL);
			sem_post(&ctask->sem);
		}
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		gettimeofday(&ctask->clients[index].tv_disconnected, NULL);

		ctask->num_conn_disconnected++;
		if (ctask->num_conn_disconnected == client_conn_num_per_task) {
			gettimeofday(&ctask->t_disconnected, NULL);
			sem_post(&ctask->sem);
			break;
		}
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		err("Error event received: %d(%s), status %d\n",
		    event->event, event2str(event->event), event->status);
		return -1;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		err("Device removal detected.\n");
		return -1;

	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		break;

	default:
		err("Unknown event %d(%s), status %d, ignored\n",
		    event->event, event2str(event->event), event->status);
		break;
	}

	return 0;
}

static void *client_task_event_thread_run(void *arg)
{
	struct perf_client_task *ctask = arg;
	struct rdma_cm_event *event;
	int err;

	while (1) {
		err = rdma_get_cm_event(ctask->ech, &event);
		if (err) {
			perror("rdma_get_cm_event");
			ctask->event_thread_err = errno;
			sem_post(&ctask->sem);
			break;
		}

		err = client_handle_event(ctask, event);
		rdma_ack_cm_event(event);
		if (err) {
			err("Failed to handle event %d\n", event->event);
			ctask->event_thread_err = err;
			sem_post(&ctask->sem);
			break;
		}
	}

	return NULL;
}

static int client_task_init(struct perf_client_task *ctask)
{
	struct rdma_cm_id *id;
	int i, err;

	ctask->clients = calloc(client_conn_num_per_task,
				sizeof(*ctask->clients));
	if (!ctask->clients) {
		err("calloc(%d/%ld) failed: %d\n",
		       client_conn_num_per_task, sizeof(sizeof(*ctasks->clients)), errno);
		return errno;
	}

	ctask->ech = rdma_create_event_channel();
	if (!ctask->ech) {
		perror("rdma_create_event_channel");
		err = errno;
		goto fail_create_event_channel;
	}

	sem_init(&ctask->sem, 0, 0);

	err = pthread_create(&ctask->event_thread, NULL,
			     client_task_event_thread_run, ctask);
	if (err) {
		perror("pthread_creaste");
		err = errno;
		goto fail_pthread_create;
	}

	for (i = 0; i < client_conn_num_per_task; i++) {
		err = rdma_create_id(ctask->ech, &id,
				     (void *)(unsigned long)i, RDMA_PS_TCP);
		if (err) {
			perror("rdma_create_id");
			goto fail_clients_init;
		}

		ctask->clients[i].id = id;
		ctask->clients[i].state = -1;
	}

	return 0;

fail_clients_init:
	for (i = 0; i < client_conn_num_per_task; i++) {
		if (ctask->clients[i].cq)
			ibv_destroy_cq(ctask->clients[i].cq);
		if (ctask->clients[i].mr)
			ibv_dereg_mr(ctask->clients[i].mr);
		if (ctask->clients[i].pd)
			ibv_dealloc_pd(ctask->clients[i].pd);
		if (ctask->clients[i].id)
			rdma_destroy_id(ctask->clients[i].id);
	}

	pthread_cancel(ctask->event_thread);
	pthread_join(ctask->event_thread, NULL);
fail_pthread_create:
	rdma_destroy_event_channel(ctask->ech);
fail_create_event_channel:
	free(ctask->clients);

	return err;
}

static void client_task_free_ib_res(struct perf_client_task *ctask)
{
	int i;

	for (i = 0; i < client_conn_num_per_task; i++) {
		if (ctask->clients[i].cq)
			ibv_destroy_cq(ctask->clients[i].cq);
		if (ctask->clients[i].mr)
			ibv_dereg_mr(ctask->clients[i].mr);
		if (ctask->clients[i].pd)
			ibv_dealloc_pd(ctask->clients[i].pd);
		if (ctask->clients[i].id)
			rdma_destroy_id(ctask->clients[i].id);
	}

	rdma_destroy_event_channel(ctask->ech);
}

static void *client_task_main_thread_run(void *arg)
{
	unsigned long taskid = (unsigned long)arg;
	struct perf_client_task *ctask = ctasks + taskid;
	int i, err;

	info("  Task %ld started..\n", taskid);

	err = client_task_init(ctask);
	if (err)
		return NULL;

	gettimeofday(&ctask->t_start, NULL);

	for (i = 0; i < client_conn_num_per_task; i++) {
		gettimeofday(&ctask->clients[i].tv_start, NULL);
		err = rdma_resolve_addr(ctask->clients[i].id, NULL,
					&ctask->server, 16000);
		if (err) {
			perror("rdma_reslove_addr");
			goto fail;
		}
	}

	sem_wait(&ctask->sem);
	if (ctask->event_thread_err) {
		info("Task %ld: event_thread failed %d\n", taskid, ctask->event_thread_err);
		goto fail;
	}

	info("Task %ld: all connections have been established\n", taskid);
	num_tasks_conn_established++;
	if (num_tasks_conn_established == client_task_num)
		gettimeofday(&prog_conn_established, NULL);

	sleep(1);

	gettimeofday(&ctask->t_dis_start, NULL);
	for (i = 0; i < client_conn_num_per_task; i++) {
		gettimeofday(&ctask->clients[i].tv_dis_start, NULL);
		err = rdma_disconnect(ctask->clients[i].id);
		if (err) {
			perror("rdma_disconnect");
			goto fail;
		}
	}

	sem_wait(&ctask->sem);

	client_task_free_ib_res(ctask);

	client_running_task_num--;
	info("Task %ld: all connections have been disconnected, exit (left running %d)\n",
	     taskid, client_running_task_num);

	return NULL;

fail:
	client_task_free_ib_res(ctask);

	client_running_task_num--;
	client_failed_task_num++;

	return NULL;
}

static void report_dump(uint64_t t_ar, uint64_t t_rr, uint64_t t_est,
			uint64_t max_ar, uint64_t max_rr, uint64_t max_est,
			uint64_t t_dis, uint64_t max_dis,
			unsigned int data_num)
{
	uint64_t t;

	t = t_ar / data_num;
	dump("  till addr_resolved (max, average):	%ld.%03ld	%ld.%03ld\n",
	     max_ar / 1000000, (max_ar % 1000000) / 1000,
	     t / 1000000, (t % 1000000) / 1000);

	t= t_rr / data_num;
	dump("  till route resolved:			%ld.%03ld	%ld.%03ld\n",
	     max_rr / 1000000, (max_rr % 1000000) / 1000,
	     t / 1000000, (t % 1000000) / 1000);

	t = t_est / data_num;
	dump("  till established:			%ld.%03ld	%ld.%03ld\n",
	     max_est / 1000000, (max_est % 1000000) / 1000,
	     t / 1000000, (t % 1000000) / 1000);

	t = t_dis / data_num;
	dump("  (disconnect:	%ld.%03ld	%ld.%03ld)\n",
	     max_dis / 1000000, (max_dis % 1000000) / 1000,
	     t / 1000000, (t % 1000000) / 1000);
}

static void report_ib_obj(void)
{
	uint64_t max_pd = 0, max_mr = 0, max_cq = 0, max_qp = 0;
	uint64_t t_pd = 0, t_mr = 0, t_cq = 0, t_qp = 0;
	struct perf_client *c;
	int i, j;

	for (i = 0; i <client_task_num; i++) {
		for (j = 0; j < client_conn_num_per_task; j++) {
			c = &ctasks[i].clients[j];

			if (c->pd_used > max_pd)
				max_pd = c->pd_used;
			if (c->mr_used > max_mr)
				max_mr = c->mr_used;
			if (c->cq_used > max_cq)
				max_cq = c->cq_used;
			if (c->qp_used > max_qp)
				max_qp = c->qp_used;

			t_pd += c->pd_used;
			t_mr += c->mr_used;
			t_cq += c->cq_used;
			t_qp += c->qp_used;
		}
	}

	dump("\nAfter route_resolved, before connect (in microsecond, which is an exception):\n");
	dump("  alloc_pd (max, avg): %ld	%ld\n", max_pd, t_pd / (client_task_num * client_conn_num_per_task));
	dump("  reg_mr:              %ld	%ld\n", max_mr, t_mr / (client_task_num * client_conn_num_per_task));
	dump("  create_cq:           %ld	%ld\n", max_cq, t_cq / (client_task_num * client_conn_num_per_task));
	dump("  create_qp:           %ld	%ld\n", max_qp, t_qp / (client_task_num * client_conn_num_per_task));
}

static void report_connection_wise(void)
{
	/* addr_resolved, route_resolved and established */
	uint64_t max_ar = 0, max_rr = 0, max_est = 0, t_ar = 0, t_rr = 0, t_est = 0;
	/* Disconnect */
	uint64_t max_dis = 0, t_dis = 0;
	struct perf_client *c;
	uint64_t d1, d2, d3;
	int i, j;

	for (i = 0; i < client_task_num; i++) {
		for (j = 0; j < client_conn_num_per_task; j++) {
			c = &ctasks[i].clients[j];

			/* till addr_resolved */
			d1 = get_time_used(&c->tv_start, &c->tv_addr_resolved);
			if (max_ar < d1)
				max_ar = d1;
			t_ar += d1;

			/* till route_resolved */
			d2 = get_time_used(&c->tv_start, &c->tv_route_resolved);
			if (max_rr < d2)
				max_rr = d2;
			t_rr += d2;

			/* till established */
			d3 = get_time_used(&c->tv_start, &c->tv_established);
			if (max_est < d3)
				max_est = d3;
			t_est += d3;

			/* Disconnect */
			d1 = get_time_used(&c->tv_dis_start, &c->tv_disconnected);
			if (max_dis < d1)
				max_dis = d1;
			t_dis += d1;
		}
	}

	dump("Connection-wise (%d connections in total):\n",
	     client_task_num * client_conn_num_per_task);
	report_dump(t_ar, t_rr, t_est, max_ar, max_rr, max_est,
		    t_dis, max_dis, client_task_num * client_conn_num_per_task);

	report_ib_obj();
}

static void report_task_wise(void)
{
	/* addr_resolved, route_resolved and established */
	uint64_t max_ar = 0, max_rr = 0, max_est = 0, t_ar = 0, t_rr = 0, t_est = 0;
	/* Disconnect */
	uint64_t max_dis = 0, t_dis = 0;
	struct perf_client_task *t;
	uint64_t d1, d2, d3;
	int i;

	for (i = 0; i < client_task_num; i++) {
		t = ctasks + i;

		/* Resolve addr */
		d1 = get_time_used(&t->t_start, &t->t_addr_resolved);
		if (max_ar < d1)
			max_ar = d1;
		t_ar += d1;

		/* Resolve route */
		d2 = get_time_used(&t->t_start, &t->t_route_resolved);
		if (max_rr < d2)
			max_rr = d2;
		t_rr += d2;

		/* Establish */
		d3 = get_time_used(&t->t_start, &t->t_established);
		if (max_est < d3)
			max_est = d3;
		t_est += d3;

		/* Disconnect */
		d1 = get_time_used(&t->t_dis_start, &t->t_disconnected);
		if (max_dis < d1)
			max_dis = d1;
		t_dis += d1;
	}

	dump("Task-wise (%d tasks in total):\n", client_task_num);
	report_dump(t_ar, t_rr, t_est, max_ar, max_rr, max_est,
		    t_dis, max_dis, client_task_num);
}

static void do_statistic_report(void)
{
	uint64_t d = get_time_used(&prog_start, &prog_conn_established);

	dump("\n");
	dump("******** Statistics (in seconds) ********\n");
	dump("\n");

	report_connection_wise();
	dump("\n");

	report_task_wise();
	dump("\n");

	dump("Program-wise (including time of tasks creation):\n");
	dump("  Establishment: %ld.%03ld\n", d / 1000000, (d % 1000000) / 1000);
	dump("\n");

	dump("*****************************************\n");
	dump("\n");
}

int cmperf_start_client(struct sockaddr *server, unsigned short first_port,
			unsigned int task_num, unsigned int conn_num_per_task)
{
	const char *p = NULL;
	char ipstr[64] = {};
	int i, err;

	switch(server->sa_family) {
	case AF_INET:
		p = inet_ntop(AF_INET, &(((struct sockaddr_in *)server)->sin_addr),
			      ipstr, sizeof(ipstr));
		break;

	case AF_INET6:
		p = inet_ntop(AF_INET, &(((struct sockaddr_in *)server)->sin_addr),
			      ipstr, sizeof(ipstr));
		break;
	default:
		err("Unsupported AF: %d\n", server->sa_family);
		break;
	}

	if (!p) {
		err("Failed to get ip addr: %d\n", errno);
		return -1;
	}

	info("Client: %d tasks each with %d connections, first ip port %d, server: %s\n",
	     task_num, conn_num_per_task, first_port, ipstr);

	if (!task_num || !conn_num_per_task) {
		err("Invalid parameter: Task num %d, connection num per task %d\n",
		       task_num, conn_num_per_task);
		return EINVAL;
	}

	client_task_num = task_num;
	client_conn_num_per_task = conn_num_per_task;
	ctasks = calloc(task_num, sizeof(*ctasks));
	if (!ctasks) {
		err("calloc(%d/%ld) failed: %d\n",
		       task_num, sizeof(*ctasks), errno);
		return errno;
	}

	client_running_task_num = task_num;
	gettimeofday(&prog_start, NULL);

	for (i = 0; i < task_num; i++) {
		ctasks[i].server = *server;
		if (server->sa_family == AF_INET)
			((struct sockaddr_in *) &ctasks[i].server)->sin_port = htons(first_port + i);
		else
			((struct sockaddr_in6 *) &ctasks[i].server)->sin6_port = htons(first_port + i);

		err = pthread_create(&ctasks[i].main_thread, NULL,
				     client_task_main_thread_run, (void *)(unsigned long)i);
		if (err) {
			err("Failed to start task %d: %d\n", i, errno);
			return err;
		}
	}

	while (client_running_task_num)
		sleep(1);

	if (client_failed_task_num) {
		info("All tasks have been finished, %d failed\n", client_failed_task_num);
		err = -1;
		goto out;
	}

	info("All tasks have been finished.\n");
	do_statistic_report();

out:
	for (i = 0; i < task_num; i++)
		free(ctasks[i].clients);

	info("Client done\n");
	return err;
}
