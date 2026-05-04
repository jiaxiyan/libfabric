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
 * EFA hardware counter test.
 *
 * Runs MSG pingpong or RMA write.
 * Use --external-mem to pass user-allocated memory for hw counters
 * in cntr_open_ext.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <inttypes.h>
#include <time.h>

#include <rdma/fi_errno.h>
#include <rdma/fi_ext_efa.h>
#include <shared.h>
#include <hmem.h>
#include "benchmarks/benchmark_shared.h"

static bool use_ext_mem;

enum {
	LONG_OPT_EXTERNAL_MEM,
};

/* Track ext_mem allocations for cleanup. Two counters (tx, rx) x two
 * memory locations (comp, err) = up to 4 GPU allocations and 4 dmabuf fds.
 */
#define MAX_EXT_MEM_ALLOCS 4
static void *ext_mem_gpu_bufs[MAX_EXT_MEM_ALLOCS];
static int ext_mem_dmabuf_fds[MAX_EXT_MEM_ALLOCS];
static int ext_mem_alloc_cnt;

/* Process-accessible pointers to dmabuf counter memory.
 * When counters are on GPU memory (dmabuf), fi_cntr_read does not work.
 * Instead, read the counter value directly via these pointers.
 */
static volatile uint64_t *txcntr_comp_ptr;
static volatile uint64_t *rxcntr_comp_ptr;

static int alloc_ext_mem_dmabuf(struct fi_efa_memory_location *mem)
{
	void *gpu_buf;
	int fd;
	uint64_t offset;
	int ret;

	ret = ft_hmem_alloc(opts.iface, opts.device, &gpu_buf,
			    sizeof(uint64_t));
	if (ret)
		return ret;

	ret = ft_hmem_memset(opts.iface, opts.device, gpu_buf, 0,
			     sizeof(uint64_t));
	if (ret)
		goto free_buf;

	ret = ft_hmem_get_dmabuf_fd(opts.iface, gpu_buf, sizeof(uint64_t),
				    &fd, &offset);
	if (ret)
		goto free_buf;

	mem->type = FI_EFA_MEMORY_LOCATION_DMABUF;
	mem->ptr = (uint8_t *)gpu_buf;
	mem->dmabuf.fd = fd;
	mem->dmabuf.offset = offset;

	ext_mem_gpu_bufs[ext_mem_alloc_cnt] = gpu_buf;
	ext_mem_dmabuf_fds[ext_mem_alloc_cnt] = fd;
	ext_mem_alloc_cnt++;
	return FI_SUCCESS;

free_buf:
	ft_hmem_free(opts.iface, gpu_buf);
	return ret;
}

static void free_ext_mem(void)
{
	int i;

	for (i = 0; i < ext_mem_alloc_cnt; i++) {
		ft_hmem_put_dmabuf_fd(opts.iface, ext_mem_dmabuf_fds[i]);
		ft_hmem_free(opts.iface, ext_mem_gpu_bufs[i]);
	}
	ext_mem_alloc_cnt = 0;
}

static int open_cntr(struct fid_cntr **cntr, volatile uint64_t **comp_ptr)
{
	struct fi_efa_ops_gda *gda_ops;
	struct fi_cntr_attr attr = {0};
	struct fi_efa_comp_cntr_init_attr efa_attr = {0};
	int ret;

	*comp_ptr = NULL;

	ret = fi_open_ops(&domain->fid, FI_EFA_GDA_OPS, 0,
			  (void **)&gda_ops, NULL);
	if (!ret) {
		attr.events = FI_CNTR_EVENTS_COMP;
		attr.wait_obj = FI_WAIT_UNSPEC;

		if (use_ext_mem) {
			if (opts.iface != FI_HMEM_SYSTEM) {
				ret = alloc_ext_mem_dmabuf(
					&efa_attr.comp_cntr_ext_mem);
				if (ret)
					return ret;

				ret = alloc_ext_mem_dmabuf(
					&efa_attr.err_cntr_ext_mem);
				if (ret)
					return ret;
			} else {
				efa_attr.comp_cntr_ext_mem.type =
					FI_EFA_MEMORY_LOCATION_VA;
				efa_attr.comp_cntr_ext_mem.ptr =
					calloc(1, sizeof(uint64_t));
				if (!efa_attr.comp_cntr_ext_mem.ptr)
					return -FI_ENOMEM;

				efa_attr.err_cntr_ext_mem.type =
					FI_EFA_MEMORY_LOCATION_VA;
				efa_attr.err_cntr_ext_mem.ptr =
					calloc(1, sizeof(uint64_t));
				if (!efa_attr.err_cntr_ext_mem.ptr) {
					free(efa_attr.comp_cntr_ext_mem.ptr);
					return -FI_ENOMEM;
				}
			}

			efa_attr.flags =
				FI_EFA_COMP_CNTR_INIT_WITH_COMP_EXTERNAL_MEM |
				FI_EFA_COMP_CNTR_INIT_WITH_ERR_EXTERNAL_MEM;
		}

		ret = gda_ops->cntr_open_ext(domain, &attr, cntr, NULL,
					     &efa_attr);
	}

	if (ret) {
		FT_WARN("hw cntr open failed (%s)\n", fi_strerror(-ret));
		if (use_ext_mem && opts.iface == FI_HMEM_SYSTEM) {
			free(efa_attr.comp_cntr_ext_mem.ptr);
			free(efa_attr.err_cntr_ext_mem.ptr);
		}
		return ret;
	}

	if (use_ext_mem && opts.iface != FI_HMEM_SYSTEM)
		*comp_ptr = (volatile uint64_t *)efa_attr.comp_cntr_ext_mem.ptr;

	return FI_SUCCESS;
}

static bool use_dmabuf_ext_mem(void)
{
	return use_ext_mem && opts.iface != FI_HMEM_SYSTEM;
}

/* Read counter value directly from the process-accessible ptr.
 * Used when counters are on GPU memory (dmabuf) and fi_cntr_read
 * does not work.
 */
static uint64_t read_cntr_ptr(volatile uint64_t *ptr)
{
	return *ptr;
}

/* Spin until the counter value (read via ptr) reaches total or timeout. */
static int spin_for_cntr_ptr(volatile uint64_t *ptr, uint64_t total,
			     int timeout)
{
	struct timespec a, b;

	if (timeout >= 0)
		clock_gettime(CLOCK_MONOTONIC, &a);

	for (;;) {
		if (read_cntr_ptr(ptr) >= total)
			return 0;

		if (timeout >= 0) {
			clock_gettime(CLOCK_MONOTONIC, &b);
			if ((b.tv_sec - a.tv_sec) > timeout)
				break;
		}
	}

	fprintf(stderr, "%ds timeout expired\n", timeout);
	return -FI_ENODATA;
}

/*
 * Custom init that mirrors ft_init_fabric() but opens hw counters
 * and assigns them to the global txcntr/rxcntr before fi_enable.
 *
 * When using dmabuf external memory, CQ completions are kept enabled
 * so that shared helpers (ft_sync, ft_finalize, pingpong) work normally.
 * The counter values are then verified via direct ptr reads.
 *
 * When not using dmabuf, CQ is disabled and the completion path routes
 * through ft_get_cntr_comp which calls fi_cntr_read.
 */
static int init_fabric_with_hw_cntr(void)
{
	char *node, *service;
	uint64_t flags = 0;
	int ret;

	ret = ft_init();
	if (ret)
		return ret;

	ret = ft_init_oob();
	if (ret)
		return ret;

	if (oob_sock >= 0 && opts.dst_addr) {
		ret = ft_sock_sync(oob_sock, 0);
		if (ret)
			return ret;
	}

	ret = ft_read_addr_opts(&node, &service, hints, &flags, &opts);
	if (ret)
		return ret;

	/* hw cntr require API version >= 2.5 */
	ret = fi_getinfo(FI_VERSION(2, 5), node, service, flags,
				hints, &fi);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}

	if (fi->domain_attr->max_cntr_value == UINT64_MAX) {
		FT_INFO("Device does not support hw counters, skipping test");
		return -FI_ENODATA;
	}

	ret = ft_open_fabric_res();
	if (ret)
		return ret;

	ret = ft_alloc_active_res(fi);
	if (ret)
		return ret;

	opts.options |= FT_OPT_TX_CNTR | FT_OPT_RX_CNTR;

	/*
	 * For dmabuf counters, fi_cntr_read does not work, so keep CQ
	 * enabled for the shared completion helpers. Counter values
	 * are verified via direct ptr reads after the test.
	 */
	if (!use_dmabuf_ext_mem())
		opts.options &= ~(FT_OPT_TX_CQ | FT_OPT_RX_CQ);

	ret = open_cntr(&txcntr, &txcntr_comp_ptr);
	if (ret) {
		FT_PRINTERR("open_cntr(tx)", ret);
		return ret;
	}

	ret = open_cntr(&rxcntr, &rxcntr_comp_ptr);
	if (ret) {
		FT_PRINTERR("open_cntr(rx)", ret);
		return ret;
	}

	ret = ft_enable_ep_recv();
	if (ret)
		return ret;

	if (oob_sock >= 0 && !opts.dst_addr) {
		ret = ft_sock_sync(oob_sock, 0);
		if (ret)
			return ret;
	}

	ret = ft_init_av();
	if (ret)
		return ret;

	return 0;
}

/*
 * Verify that dmabuf counter values are being updated by reading
 * directly from the process-accessible ptr. The expected value is
 * a lower bound; the counter may exceed it due to control messages.
 */
static int verify_dmabuf_cntr(volatile uint64_t *comp_ptr,
			      uint64_t min_expected, const char *name)
{
	int ret;

	ret = spin_for_cntr_ptr(comp_ptr, min_expected, timeout);
	if (ret) {
		FT_ERR("%s dmabuf counter timed out: got %" PRIu64 ", expected >= %" PRIu64 "\n",
		       name, read_cntr_ptr(comp_ptr), min_expected);
		return ret;
	}

	printf("%s dmabuf counter: %" PRIu64 " (expected >= %" PRIu64 ")\n",
	       name, read_cntr_ptr(comp_ptr), min_expected);
	return FI_SUCCESS;
}

static int run_msg(void)
{
	int i, ret = 0;

	ret = init_fabric_with_hw_cntr();
	if (ret)
		return ret;

	if (!(opts.options & FT_OPT_SIZE)) {
		for (i = 0; i < TEST_CNT; i++) {
			if (!ft_use_size(i, opts.sizes_enabled))
				continue;
			opts.transfer_size = test_size[i].size;
			init_test(&opts, test_name, sizeof(test_name));
			ret = pingpong();
			if (ret)
				goto out;
		}
	} else {
		init_test(&opts, test_name, sizeof(test_name));
		ret = pingpong();
		if (ret)
			goto out;
	}

	if (use_dmabuf_ext_mem()) {
		ret = verify_dmabuf_cntr(txcntr_comp_ptr, tx_seq, "tx");
		if (ret)
			goto out;
		ret = verify_dmabuf_cntr(rxcntr_comp_ptr, rx_seq, "rx");
		if (ret)
			goto out;
	}

	ft_finalize();
out:
	return ret;
}

static int rma_write(void)
{
	int i, ret;

	ret = ft_sync();
	if (ret)
		return ret;

	ft_start();
	for (i = 0; i < opts.iterations; i++) {
		ret = fi_write(ep, tx_buf, opts.transfer_size, mr_desc,
			       remote_fi_addr, remote.addr, remote.key,
			       &tx_ctx);
		if (ret) {
			FT_PRINTERR("fi_write", ret);
			return ret;
		}
		tx_seq++;

		if (use_dmabuf_ext_mem()) {
			ret = spin_for_cntr_ptr(txcntr_comp_ptr, tx_seq,
						timeout);
		} else {
			ret = ft_get_tx_comp(tx_seq);
		}
		if (ret)
			return ret;
	}
	ft_stop();

	show_perf(NULL, opts.transfer_size, opts.iterations, &start, &end, 1);
	return 0;
}

static int run_rma(void)
{
	int i, ret = 0;

	ret = init_fabric_with_hw_cntr();
	if (ret)
		return ret;

	ret = ft_exchange_keys(&remote);
	if (ret)
		return ret;

	if (!(opts.options & FT_OPT_SIZE)) {
		for (i = 0; i < TEST_CNT; i++) {
			if (!ft_use_size(i, opts.sizes_enabled))
				continue;
			opts.transfer_size = test_size[i].size;
			init_test(&opts, test_name, sizeof(test_name));
			ret = rma_write();
			if (ret)
				goto out;
		}
	} else {
		init_test(&opts, test_name, sizeof(test_name));
		ret = rma_write();
		if (ret)
			goto out;
	}

	if (use_dmabuf_ext_mem()) {
		printf("tx dmabuf counter final: %" PRIu64 "\n",
		       read_cntr_ptr(txcntr_comp_ptr));
	}

	ft_finalize();
out:
	return ret;
}

int main(int argc, char **argv)
{
	int op, ret;

	opts = INIT_OPTS;
	opts.rma_op = 0;
	opts.comp_method = FT_COMP_SPIN;

	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	int lopt_idx = 0;
	struct option long_opts[] = {
		{"external-mem", no_argument, NULL, LONG_OPT_EXTERNAL_MEM},
		{0, 0, 0, 0}
	};
	while ((op = getopt_long(argc, argv, "h" CS_OPTS INFO_OPTS BENCHMARK_OPTS
				 API_OPTS, long_opts, &lopt_idx)) != -1) {
		switch (op) {
		case LONG_OPT_EXTERNAL_MEM:
			use_ext_mem = true;
			break;
		default:
			if (!ft_parse_long_opts(op, optarg))
				continue;
			ft_parse_benchmark_opts(op, optarg);
			ft_parseinfo(op, optarg, hints, &opts);
			ft_parsecsopts(op, optarg, &opts);
			ret = ft_parse_api_opts(op, optarg, hints, &opts);
			if (ret)
				return ret;
			break;
		case '?':
		case 'h':
			ft_csusage(argv[0],
				   "Pingpong using EFA hardware counters.");
			ft_benchmark_usage();
			FT_PRINT_OPTS_USAGE("-o <op>",
				"op: msg|write (default: msg)");
			FT_PRINT_OPTS_USAGE("--external-mem",
				"use external user memory for hw counters");
			ft_longopts_usage();
			return EXIT_FAILURE;
		}
	}

	if (optind < argc)
		opts.dst_addr = argv[optind];

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps |= FI_MSG;
	hints->domain_attr->mr_mode = opts.mr_mode;
	hints->domain_attr->threading = FI_THREAD_DOMAIN;
	hints->tx_attr->tclass = FI_TC_LOW_LATENCY;
	hints->addr_format = opts.address_format;
	hints->mode |= FI_CONTEXT | FI_CONTEXT2;

	if (opts.rma_op) {
		hints->caps |= FI_RMA;
		ret = run_rma();
	} else {
		ret = run_msg();
	}

	free_ext_mem();
	ft_free_res();
	return ft_exit_code(ret);
}
