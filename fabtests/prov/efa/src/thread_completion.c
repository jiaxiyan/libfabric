/*
 * Copyright (c) 2026, Amazon.com, Inc.  All rights reserved.
 *
 * This software is available to you under the BSD license
 * below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * FI_THREAD_COMPLETION multi-threaded validation test.
 *
 * Validates that the provider's lock-free data path works correctly
 * under FI_THREAD_COMPLETION threading model across 4 configurations:
 *
 *   (default):    Separate TX CQ and RX CQ, concurrent access
 *   --shared-cq:  Shared CQ, serialized access via mutex
 *   -t counter:   Separate TX counter and RX counter, concurrent access
 *   --shared-cntr: Shared counter, serialized access via mutex
 */

#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>

#include "shared.h"

static bool shared_cntr;

struct thread_args {
	struct fid_ep *ep;
	struct fid_cq *cq;
	struct fid_cntr *cntr;
	struct fid_mr *mr;
	void *buf;
	size_t buf_size;
	pthread_mutex_t *shared_lock;
	uint64_t cntr_target;
	int ret;
};

/* Post fi_send and wait for CQ completions. Acquires shared_lock if set. */
static void *tx_thread_cq(void *arg)
{
	struct thread_args *args = (struct thread_args *)arg;
	struct fi_context2 ctx;
	uint64_t cq_cntr = 0;
	int ret, rc;

	if (args->shared_lock)
		pthread_mutex_lock(args->shared_lock);

	for (int i = 0; i < opts.iterations; i++) {
		do {
			ret = fi_send(args->ep, args->buf,
				      args->buf_size,
				      fi_mr_desc(args->mr),
				      remote_fi_addr, &ctx);
			if (ret == -FI_EAGAIN) {
				rc = ft_progress(args->cq, i, &cq_cntr);
				if (rc && rc != -FI_EAGAIN) {
					ret = rc;
					goto out;
				}
			}
		} while (ret == -FI_EAGAIN);

		if (ret) {
			FT_PRINTERR("fi_send", ret);
			goto out;
		}

		ret = ft_get_cq_comp(args->cq, &cq_cntr, i + 1, timeout);
		if (ret)
			goto out;
	}

out:
	if (args->shared_lock)
		pthread_mutex_unlock(args->shared_lock);

	args->ret = ret;
	return NULL;
}

/* Post fi_recv and wait for CQ completions. Acquires shared_lock if set. */
static void *rx_thread_cq(void *arg)
{
	struct thread_args *args = (struct thread_args *)arg;
	struct fi_context2 ctx;
	uint64_t cq_cntr = 0;
	int ret, rc;

	if (args->shared_lock)
		pthread_mutex_lock(args->shared_lock);

	for (int i = 0; i < opts.iterations; i++) {
		do {
			ret = fi_recv(args->ep, args->buf,
				      args->buf_size,
				      fi_mr_desc(args->mr),
				      FI_ADDR_UNSPEC, &ctx);
			if (ret == -FI_EAGAIN) {
				rc = ft_progress(args->cq, i, &cq_cntr);
				if (rc && rc != -FI_EAGAIN) {
					ret = rc;
					goto out;
				}
			}
		} while (ret == -FI_EAGAIN);

		if (ret) {
			FT_PRINTERR("fi_recv", ret);
			goto out;
		}

		ret = ft_get_cq_comp(args->cq, &cq_cntr, i + 1, timeout);
		if (ret)
			goto out;
	}

out:
	if (args->shared_lock)
		pthread_mutex_unlock(args->shared_lock);

	args->ret = ret;
	return NULL;
}

/*
 * Post fi_send then wait for counter target.
 * No CQ is bound in counter mode, so use sched_yield() on EAGAIN.
 */
static void *tx_thread_cntr(void *arg)
{
	struct thread_args *args = (struct thread_args *)arg;
	struct fi_context2 ctx;
	int ret;

	if (args->shared_lock)
		pthread_mutex_lock(args->shared_lock);

	for (int i = 0; i < opts.iterations; i++) {
		do {
			ret = fi_send(args->ep, args->buf,
				      args->buf_size,
				      fi_mr_desc(args->mr),
				      remote_fi_addr, &ctx);
			if (ret == -FI_EAGAIN)
				sched_yield();
		} while (ret == -FI_EAGAIN);

		if (ret) {
			FT_PRINTERR("fi_send", ret);
			goto out;
		}
	}

	ret = ft_get_cntr_comp(args->cntr, args->cntr_target, timeout);

out:
	if (args->shared_lock)
		pthread_mutex_unlock(args->shared_lock);

	args->ret = ret;
	return NULL;
}

/*
 * Post fi_recv then wait for counter target.
 * No CQ is bound in counter mode, so use sched_yield() on EAGAIN.
 */
static void *rx_thread_cntr(void *arg)
{
	struct thread_args *args = (struct thread_args *)arg;
	struct fi_context2 ctx;
	int ret;

	if (args->shared_lock)
		pthread_mutex_lock(args->shared_lock);

	for (int i = 0; i < opts.iterations; i++) {
		do {
			ret = fi_recv(args->ep, args->buf,
				      args->buf_size,
				      fi_mr_desc(args->mr),
				      FI_ADDR_UNSPEC, &ctx);
			if (ret == -FI_EAGAIN)
				sched_yield();
		} while (ret == -FI_EAGAIN);

		if (ret) {
			FT_PRINTERR("fi_recv", ret);
			goto out;
		}
	}

	ret = ft_get_cntr_comp(args->cntr, args->cntr_target, timeout);

out:
	if (args->shared_lock)
		pthread_mutex_unlock(args->shared_lock);

	args->ret = ret;
	return NULL;
}

/* Spawn TX and RX threads using CQ-based completion. Serializes CQ access with a mutex when FT_OPT_CQ_SHARED. */
static int run_cq_test(void)
{
	pthread_t tx_tid, rx_tid;
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	struct thread_args tx_args = {0}, rx_args = {0};
	int ret;

	tx_args.ep = ep;
	tx_args.cq = txcq;
	tx_args.mr = mr;
	tx_args.buf = tx_buf;
	tx_args.buf_size = opts.transfer_size;

	rx_args.ep = ep;
	rx_args.cq = rxcq;
	rx_args.mr = mr;
	rx_args.buf = rx_buf;
	rx_args.buf_size = opts.transfer_size;

	if (ft_check_opts(FT_OPT_CQ_SHARED)) {
		tx_args.shared_lock = &lock;
		rx_args.shared_lock = &lock;
	}

	ret = pthread_create(&rx_tid, NULL, rx_thread_cq, &rx_args);
	if (ret) {
		FT_PRINTERR("pthread_create(rx)", ret);
		return -ret;
	}

	ret = pthread_create(&tx_tid, NULL, tx_thread_cq, &tx_args);
	if (ret) {
		FT_PRINTERR("pthread_create(tx)", ret);
		pthread_join(rx_tid, NULL);
		return -ret;
	}

	pthread_join(tx_tid, NULL);
	pthread_join(rx_tid, NULL);

	if (ft_check_opts(FT_OPT_CQ_SHARED))
		pthread_mutex_destroy(&lock);

	if (tx_args.ret) {
		fprintf(stderr, "TX thread failed: %s\n",
			fi_strerror(-tx_args.ret));
		return tx_args.ret;
	}
	if (rx_args.ret) {
		fprintf(stderr, "RX thread failed: %s\n",
			fi_strerror(-rx_args.ret));
		return rx_args.ret;
	}

	return 0;
}

/* Spawn TX and RX threads using counter-based completion. Serializes counter access with a mutex when shared_cntr. */
static int run_cntr_test(void)
{
	pthread_t tx_tid, rx_tid;
	pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	struct thread_args tx_args = {0}, rx_args = {0};
	int ret;

	tx_args.ep = ep;
	tx_args.cntr = txcntr;
	tx_args.mr = mr;
	tx_args.buf = tx_buf;
	tx_args.buf_size = opts.transfer_size;
	tx_args.cntr_target = opts.iterations;

	rx_args.ep = ep;
	rx_args.cntr = rxcntr;
	rx_args.mr = mr;
	rx_args.buf = rx_buf;
	rx_args.buf_size = opts.transfer_size;
	rx_args.cntr_target = opts.iterations;

	if (shared_cntr) {
		tx_args.shared_lock = &lock;
		rx_args.shared_lock = &lock;
		tx_args.cntr_target = opts.iterations * 2;
		rx_args.cntr_target = opts.iterations * 2;
	}

	ret = pthread_create(&rx_tid, NULL, rx_thread_cntr, &rx_args);
	if (ret) {
		FT_PRINTERR("pthread_create(rx)", ret);
		return -ret;
	}

	ret = pthread_create(&tx_tid, NULL, tx_thread_cntr, &tx_args);
	if (ret) {
		FT_PRINTERR("pthread_create(tx)", ret);
		pthread_join(rx_tid, NULL);
		return -ret;
	}

	pthread_join(tx_tid, NULL);
	pthread_join(rx_tid, NULL);

	if (shared_cntr)
		pthread_mutex_destroy(&lock);

	if (tx_args.ret) {
		fprintf(stderr, "TX thread failed: %s\n",
			fi_strerror(-tx_args.ret));
		return tx_args.ret;
	}
	if (rx_args.ret) {
		fprintf(stderr, "RX thread failed: %s\n",
			fi_strerror(-rx_args.ret));
		return rx_args.ret;
	}

	return 0;
}

/*
 * Initialize fabric with a single shared counter bound with FI_SEND|FI_RECV.
 * Cannot use ft_init_fabric() because ft_alloc_ep_res always creates separate
 * TX/RX counters, and we need to bind one counter before fi_enable.
 */
static int init_shared_cntr(void)
{
	int ret;

	opts.options &= ~(FT_OPT_TX_CQ | FT_OPT_RX_CQ);
	opts.options &= ~(FT_OPT_TX_CNTR | FT_OPT_RX_CNTR);

	ret = ft_init();
	if (ret)
		return ret;

	ret = ft_init_oob();
	if (ret)
		return ret;

	ret = ft_getinfo(hints, &fi);
	if (ret)
		return ret;

	ret = ft_open_fabric_res();
	if (ret)
		return ret;

	ret = ft_alloc_active_res(fi);
	if (ret)
		return ret;

	ret = ft_cntr_open(&txcntr);
	if (ret)
		return ret;
	rxcntr = txcntr;

	ret = ft_enable_ep(ep, eq, av, txcq, rxcq, txcntr, rxcntr, NULL);
	if (ret)
		return ret;

	ret = ft_alloc_msgs();
	if (ret)
		return ret;

	ret = ft_init_av();
	if (ret)
		return ret;

	return 0;
}

static int run_test(void)
{
	int ret;

	if (shared_cntr) {
		ret = init_shared_cntr();
	} else {
		ret = ft_init_fabric();
	}
	if (ret)
		return ret;

	ret = ft_sync();
	if (ret)
		return ret;

	if (ft_check_opts(FT_OPT_TX_CNTR) || shared_cntr)
		ret = run_cntr_test();
	else
		ret = run_cq_test();

	if (shared_cntr)
		rxcntr = NULL;

	ft_sync();
	return ret;
}

enum {
	OPT_SHARED_CQ = 256,
	OPT_SHARED_CNTR,
};

static struct option test_long_opts[] = {
	{"shared-cq", no_argument, NULL, OPT_SHARED_CQ},
	{"shared-cntr", no_argument, NULL, OPT_SHARED_CNTR},
	{0, 0, 0, 0}
};

static void print_usage(char *prog)
{
	ft_csusage(prog, "FI_THREAD_COMPLETION multi-threaded test");
	FT_PRINT_OPTS_USAGE("--shared-cq",
		"use shared CQ with serialized access");
	FT_PRINT_OPTS_USAGE("--shared-cntr",
		"use shared counter with serialized access (implies -t counter)");
}

int main(int argc, char **argv)
{
	int op, ret;

	opts = INIT_OPTS;
	opts.options |= FT_OPT_SIZE;
	timeout = 30;

	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt_long(argc, argv, "h" CS_OPTS INFO_OPTS,
				 test_long_opts, NULL)) != -1) {
		switch (op) {
		case OPT_SHARED_CQ:
			opts.options |= FT_OPT_CQ_SHARED;
			break;
		case OPT_SHARED_CNTR:
			shared_cntr = true;
			break;
		case '?':
		case 'h':
			print_usage(argv[0]);
			return EXIT_FAILURE;
		default:
			ft_parseinfo(op, optarg, hints, &opts);
			ft_parsecsopts(op, optarg, &opts);
			break;
		}
	}

	if (optind < argc)
		opts.dst_addr = argv[optind];

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG;
	hints->mode = FI_CONTEXT | FI_CONTEXT2;
	hints->domain_attr->threading = FI_THREAD_COMPLETION;
	hints->domain_attr->mr_mode = FI_MR_ALLOCATED | FI_MR_LOCAL |
				      FI_MR_VIRT_ADDR | FI_MR_PROV_KEY;

	ret = run_test();

	ft_free_res();
	return ft_exit_code(ret);
}
