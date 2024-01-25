/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause) */
/*
 * Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved
 */

#include <arpa/inet.h>
#include <getopt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "config.h"

#include "cmperf.h"


enum {
	CMPERF_ROLE_NONE,
	CMPERF_ROLE_SERVER,
	CMPERF_ROLE_CLIENT,
};

static unsigned int sock_port = CMPERF_DEFAULT_SOCK_PORT;
static unsigned int task_num = CMPERF_DEFAULT_TASK_NUM;
static unsigned int conn_num = CMPERF_DEFAULT_CONN_NUM;
static int my_role = CMPERF_ROLE_NONE;
static struct sockaddr server;

static void show_usage(char *program)
{
	printf("Usage: %s [OPTION]\n", program);
	printf("   [-c, --client]	- Run as the client\n");
	printf("   [-s, --server]	- Run as the server\n");
	printf("   [-t, --task-num]	- The number of tasks that run simultaneously\n");
	printf("   [-n, --conn-num]	- The number of connections that each task have\n");
	printf("   [-v, --version]	- Show version\n");
	printf("   [-h, --help]		- Show help\n");
	printf("Example:\n");
	printf("    Server: $ cmperf -s -t 4 -n 10\n");
	printf("    Client: $ cmperf -c -t 4 -n 10 192.168.2.100\n");

}

static int get_addr(char *ip, struct sockaddr *addr)
{
	struct addrinfo *res;
	int ret;

	ret = getaddrinfo(ip, NULL, NULL, &res);
	if (ret) {
		err("getaddrinfo failed (%s) - invalid hostname or IP address '%s'\n",
		    gai_strerror(ret), ip);
		return ret;
	}

	if (res->ai_family == PF_INET)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
	else if (res->ai_family == PF_INET6)
		memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
	else
		ret = -1;

	freeaddrinfo(res);
	return ret;
}

static int parse_opt(int argc, char *argv[])
{
	static const struct option long_opts[] = {
		{"version", 0, NULL, 'v'},
		{"help", 0, NULL, 'h'},
		{"client", 0, NULL, 'c'},
		{"server", 0, NULL, 's'},
		{"task-num", 0, NULL, 't'},
		{"conn-num", 0, NULL, 'n'},
                {},
        };
	int op, err;

	while ((op = getopt_long(argc, argv, "hvcst:n:", long_opts, NULL)) != -1) {
		switch (op) {
		case 'v':
			printf("%s %s\n", PROJECT_NAME, PROJECT_VERSION);
			exit(0);

		case 'h':
			show_usage(argv[0]);
			exit(0);

		case 'c':
			my_role = CMPERF_ROLE_CLIENT;
			err = get_addr(argv[argc - 1], &server);
			if (err) {
				err("Failed to get server IP address\n");
				exit(err);
			}
			break;

		case 's':
			my_role = CMPERF_ROLE_SERVER;
			break;

		case 't':
			task_num = atoi(optarg);
			break;

		case 'n':
			conn_num = atoi(optarg);
			break;

		default:
			err("Unknown option %c\n", op);
			show_usage(argv[0]);
			return -1;
		}
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int err;

	err = parse_opt(argc, argv);
	if (err)
		return err;

	if (my_role == CMPERF_ROLE_SERVER)
		err = cmperf_start_server(sock_port, task_num, conn_num);
	else
		err = cmperf_start_client(&server, sock_port,
					  task_num, conn_num);

	return err;
}
