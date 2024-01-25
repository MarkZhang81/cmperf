/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause) */
/*
 * Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved
 */

#include <sys/types.h>
#include <sys/socket.h>

#define err(args...) fprintf(stderr, ##args)
#define info(args...) fprintf(stdout, ##args)
#define dbg(args...) fprintf(stdout, ##args)

#define CMPERF_DEFAULT_SOCK_PORT  6006

#define CMPERF_DEFAULT_TASK_NUM 1
#define CMPERF_DEFAULT_CONN_NUM  1 /* per-task connection number */

/* Each process has a different sock port */
int cmperf_start_server(unsigned int first_port, unsigned int task_num,
			unsigned int conn_num_per_task);

int cmperf_start_client(struct sockaddr *server, unsigned short first_port,
			unsigned int task_num, unsigned int conn_num_per_task);
