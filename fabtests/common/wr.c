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
 */

#include <stdbool.h>
#include <stdlib.h>

#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_wr.h>

#include "shared.h"

/*
 * Work request API posting.
 *
 * With --use-wr, ft_post_tx_buf() and ft_post_rx_buf() format the operation with
 * fi_wr_prepare and then submit it with fi_wr_queue_tx / fi_wr_queue_recv /
 * fi_wr_queue_trecv plus the matching flush, instead of calling fi_send /
 * fi_recv / fi_tsend / fi_trecv.  Every test built on those two helpers
 * therefore also covers the work request interface, including the ping-pong and
 * bandwidth benchmarks, tagged or not.
 *
 * A work request is opaque and provider sized, so its buffer comes from
 * fi_ep_attr rather than from sizeof(), and a provider without the interface
 * reports zero.
 *
 * Formatting is cached on the operation's arguments, so a loop that posts the
 * same buffer, length and peer repeatedly prepares once and only queues after
 * that.  That is what the split exists for, and it also means the same work
 * request is queued many times, which a provider that wrote queue slot state
 * back into it would get wrong.
 *
 * A cache miss currently reformats the whole operation.  Once the fi_wr_modify_*
 * calls are implemented, a miss in a single field should become the matching
 * modify call instead, which is both what an application would do and the only
 * way those calls get covered.  A tagged post misses on every iteration today,
 * because ft_tag defaults to 0 and the tag then follows tx_seq / rx_seq.
 */
static fi_wr ft_tx_wr, ft_rx_wr;
static size_t ft_tx_wr_size, ft_rx_wr_size;

/*
 * A tagged send is queued and flushed as any other transmit, but a tagged
 * receive has its own queue call and its own flush.
 */
enum ft_wr_queue_type {
	FT_WR_QUEUE_TX,
	FT_WR_QUEUE_RECV,
	FT_WR_QUEUE_TRECV,
};

struct ft_wr_key {
	bool valid;
	void *buf;
	void *desc;
	size_t len;
	fi_addr_t addr;
	uint64_t data;
	uint64_t tag;
	uint64_t flags;
	enum fi_op_type op_type;
	/* RMA (FI_OP_WRITE / FI_OP_READ) remote target. */
	uint64_t rma_addr;
	uint64_t rma_key;
};

static struct ft_wr_key ft_tx_wr_key, ft_rx_wr_key;

void ft_free_wrs(void)
{
	free(ft_tx_wr);
	free(ft_rx_wr);
	ft_tx_wr = NULL;
	ft_rx_wr = NULL;
	ft_tx_wr_size = 0;
	ft_rx_wr_size = 0;
	ft_tx_wr_key.valid = false;
	ft_rx_wr_key.valid = false;
}

static int ft_alloc_wrs(void)
{
	if (ft_tx_wr && ft_rx_wr)
		return 0;

	ft_tx_wr_size = fi->ep_attr->max_tx_wr_size;
	ft_rx_wr_size = fi->ep_attr->max_rx_wr_size;

	if (!ft_tx_wr_size || !ft_rx_wr_size) {
		FT_ERR("--use-wr given but %s reports no work request support "
		       "(max_tx_wr_size %zu, max_rx_wr_size %zu)",
		       fi->fabric_attr->prov_name, ft_tx_wr_size,
		       ft_rx_wr_size);
		return -FI_ENOSYS;
	}

	ft_tx_wr = calloc(1, ft_tx_wr_size);
	ft_rx_wr = calloc(1, ft_rx_wr_size);
	if (!ft_tx_wr || !ft_rx_wr) {
		ft_free_wrs();
		return -FI_ENOMEM;
	}

	return 0;
}

static bool ft_wr_key_matches(const struct ft_wr_key *key,
			      enum fi_op_type op_type, void *op_buf,
			      void *op_mr_desc, size_t len, fi_addr_t addr,
			      uint64_t data, uint64_t tag, uint64_t flags,
			      uint64_t rma_addr, uint64_t rma_key)
{
	return key->valid && key->op_type == op_type && key->buf == op_buf &&
	       key->desc == op_mr_desc && key->len == len &&
	       key->addr == addr && key->data == data && key->tag == tag &&
	       key->flags == flags && key->rma_addr == rma_addr &&
	       key->rma_key == rma_key;
}

/*
 * Format one single iov operation.  The work request attributes reuse the
 * deferred work queue descriptors, so an untagged operation is described by
 * struct fi_op_msg, a tagged one by struct fi_op_tagged, and an RMA write or
 * read (FI_OP_WRITE / FI_OP_READ) by struct fi_op_rma.  For an RMA op, rma_addr
 * and rma_key describe the remote target; they are ignored otherwise.
 */
static int ft_wr_prepare_op(struct fid_ep *ep_ptr, enum fi_op_type op_type,
			    struct ft_wr_key *key, fi_wr wr, size_t wr_size,
			    void *op_buf, void *op_mr_desc, size_t len,
			    fi_addr_t addr, uint64_t data, uint64_t tag,
			    uint64_t flags, uint64_t rma_addr, uint64_t rma_key)
{
	struct iovec iov = {
		.iov_base = op_buf,
		.iov_len = len,
	};
	struct fi_rma_iov rma_iov = {
		.addr = rma_addr,
		.len = len,
		.key = rma_key,
	};
	struct fi_msg msg = {
		.msg_iov = &iov,
		.desc = &op_mr_desc,
		.iov_count = 1,
		.addr = addr,
		.context = NULL,
		.data = (data == NO_CQ_DATA) ? 0 : data,
	};
	struct fi_msg_tagged tagged_msg = {
		.msg_iov = &iov,
		.desc = &op_mr_desc,
		.iov_count = 1,
		.addr = addr,
		.tag = tag,
		.ignore = 0,
		.context = NULL,
		.data = (data == NO_CQ_DATA) ? 0 : data,
	};
	struct fi_msg_rma rma_msg = {
		.msg_iov = &iov,
		.desc = &op_mr_desc,
		.iov_count = 1,
		.addr = addr,
		.rma_iov = &rma_iov,
		.rma_iov_count = 1,
		.context = NULL,
		.data = (data == NO_CQ_DATA) ? 0 : data,
	};
	struct fi_op_msg op_msg = {
		.ep = ep_ptr,
		.msg = msg,
		.flags = flags,
	};
	struct fi_op_tagged op_tagged = {
		.ep = ep_ptr,
		.msg = tagged_msg,
		.flags = flags,
	};
	struct fi_op_rma op_rma = {
		.ep = ep_ptr,
		.msg = rma_msg,
		.flags = flags,
	};
	struct fi_wr_attr attr = {
		.op_type = op_type,
	};
	size_t wr_len = wr_size;
	int ret;

	switch (op_type) {
	case FI_OP_TSEND:
	case FI_OP_TRECV:
		attr.op.tagged = &op_tagged;
		break;
	case FI_OP_WRITE:
	case FI_OP_READ:
		attr.op.rma = &op_rma;
		break;
	default:
		attr.op.msg = &op_msg;
		break;
	}

	ret = fi_wr_prepare(ep_ptr, &attr, wr, &wr_len);
	if (ret) {
		FT_PRINTERR("fi_wr_prepare", ret);
		return ret;
	}

	if (wr_len > wr_size) {
		FT_ERR("fi_wr_prepare reported length %zu over the %zu byte "
		       "work request buffer", wr_len, wr_size);
		return -FI_EOTHER;
	}

	*key = (struct ft_wr_key) {
		.valid = true,
		.buf = op_buf,
		.desc = op_mr_desc,
		.len = len,
		.addr = addr,
		.data = data,
		.tag = tag,
		.flags = flags,
		.op_type = op_type,
		.rma_addr = rma_addr,
		.rma_key = rma_key,
	};

	return 0;
}

/*
 * ft_wr_queue() and ft_wr_flush() are leaf helpers that dispatch on queue type.
 */
static ssize_t ft_wr_queue(struct fid_ep *ep_ptr, const fi_wr wr, void *ctx,
			   enum ft_wr_queue_type type)
{
	switch (type) {
	case FT_WR_QUEUE_TX:
		return fi_wr_queue_tx(ep_ptr, wr, ctx);
	case FT_WR_QUEUE_RECV:
		return fi_wr_queue_recv(ep_ptr, wr, ctx);
	default:
		return fi_wr_queue_trecv(ep_ptr, wr, ctx);
	}
}

static ssize_t ft_wr_flush(struct fid_ep *ep_ptr, enum ft_wr_queue_type type)
{
	switch (type) {
	case FT_WR_QUEUE_TX:
		return fi_tx_flush(ep_ptr, 0);
	case FT_WR_QUEUE_RECV:
		return fi_recv_flush(ep_ptr, 0);
	default:
		return fi_trecv_flush(ep_ptr, 0);
	}
}

/*
 * Queue a prepared work request, then flush it. The queue step retries on a
 * full queue exactly the way FT_POST() does: a -FI_EAGAIN means the send queue
 * is full, which is resolved by polling the CQ (ft_progress) to free slots, not
 * by flushing. The provider already rings the doorbell for anything it has
 * batched before returning -FI_EAGAIN. A single flush after a successful queue
 * is what makes the operation visible to the device.
 */
static ssize_t ft_wr_queue_and_flush(struct fid_ep *ep_ptr, const fi_wr wr,
				     void *ctx, enum ft_wr_queue_type type)
{
	bool tx = (type == FT_WR_QUEUE_TX);
	struct fid_cq *cq = tx ? txcq : rxcq;
	uint64_t *seq = tx ? &tx_seq : &rx_seq;
	uint64_t *cq_cntr = tx ? &tx_cq_cntr : &rx_cq_cntr;
	const char *flush_str =
		(type == FT_WR_QUEUE_TX) ? "fi_tx_flush" :
		(type == FT_WR_QUEUE_RECV) ? "fi_recv_flush" :
					     "fi_trecv_flush";
	ssize_t ret;

	/*
	 * Queue the work request, retrying on a full queue. This is the same
	 * post/progress loop as the standard path, so reuse FT_POST(): on
	 * -FI_EAGAIN it polls the CQ to free queue slots and retries, and bumps
	 * the sequence number on success. (*seq) is passed so the macro's seq++
	 * increments the counter, not the pointer. FT_POST() requires a string
	 * literal for its op label, so the three queue variants share one.
	 */
	FT_POST(ft_wr_queue, ft_progress, cq, (*seq), cq_cntr, "fi_wr_queue",
		ep_ptr, wr, ctx, type);

	/*
	 * A queued work request is not visible to the device until flushed, so
	 * flush once after the queue succeeds.
	 */
	ret = ft_wr_flush(ep_ptr, type);
	if (ret) {
		FT_ERR("%s(): ret=%d (%s)", flush_str, (int) ret,
		       fi_strerror((int) -ret));
		return ret;
	}

	return 0;
}

ssize_t ft_wr_post_tx_buf(struct fid_ep *ep_ptr, fi_addr_t fi_addr,
			  size_t size, uint64_t data, void *ctx,
			  void *op_buf, void *op_mr_desc, uint64_t op_tag)
{
	uint64_t flags = (data == NO_CQ_DATA) ? 0 : FI_REMOTE_CQ_DATA;
	enum fi_op_type op_type;
	int ret;

	ret = ft_alloc_wrs();
	if (ret)
		return ret;

	if (hints->caps & FI_TAGGED) {
		op_type = FI_OP_TSEND;
		op_tag = op_tag ? op_tag : tx_seq;
	} else {
		op_type = FI_OP_SEND;
		op_tag = 0;
	}

	if (!ft_wr_key_matches(&ft_tx_wr_key, op_type, op_buf, op_mr_desc, size,
			       fi_addr, data, op_tag, flags, 0, 0)) {
		ret = ft_wr_prepare_op(ep_ptr, op_type, &ft_tx_wr_key, ft_tx_wr,
				       ft_tx_wr_size, op_buf, op_mr_desc, size,
				       fi_addr, data, op_tag, flags, 0, 0);
		if (ret)
			return ret;
	}

	return ft_wr_queue_and_flush(ep_ptr, ft_tx_wr, ctx, FT_WR_QUEUE_TX);
}

ssize_t ft_wr_post_rx_buf(struct fid_ep *ep_ptr, fi_addr_t fi_addr,
			  size_t size, void *ctx, void *op_buf,
			  void *op_mr_desc, uint64_t op_tag)
{
	enum ft_wr_queue_type type;
	enum fi_op_type op_type;
	int ret;

	ret = ft_alloc_wrs();
	if (ret)
		return ret;

	if (hints->caps & FI_TAGGED) {
		op_type = FI_OP_TRECV;
		type = FT_WR_QUEUE_TRECV;
		op_tag = op_tag ? op_tag : rx_seq;
	} else {
		op_type = FI_OP_RECV;
		type = FT_WR_QUEUE_RECV;
		op_tag = 0;
	}

	if (!ft_wr_key_matches(&ft_rx_wr_key, op_type, op_buf, op_mr_desc, size,
			       fi_addr, NO_CQ_DATA, op_tag, 0, 0, 0)) {
		ret = ft_wr_prepare_op(ep_ptr, op_type, &ft_rx_wr_key, ft_rx_wr,
				       ft_rx_wr_size, op_buf, op_mr_desc, size,
				       fi_addr, NO_CQ_DATA, op_tag, 0, 0, 0);
		if (ret)
			return ret;
	}

	return ft_wr_queue_and_flush(ep_ptr, ft_rx_wr, ctx, type);
}

ssize_t ft_wr_post_rma(enum ft_rma_opcodes rma_op, struct fid_ep *ep_ptr,
		       void *op_buf, void *op_mr_desc, size_t size,
		       fi_addr_t fi_addr, uint64_t rma_addr, uint64_t rma_key,
		       uint64_t data, void *ctx)
{
	enum fi_op_type op_type;
	uint64_t flags;
	int ret;

	ret = ft_alloc_wrs();
	if (ret)
		return ret;

	switch (rma_op) {
	case FT_RMA_WRITE:
		op_type = FI_OP_WRITE;
		data = NO_CQ_DATA;
		flags = 0;
		break;
	case FT_RMA_WRITEDATA:
		op_type = FI_OP_WRITE;
		flags = (data == NO_CQ_DATA) ? 0 : FI_REMOTE_CQ_DATA;
		break;
	case FT_RMA_READ:
		op_type = FI_OP_READ;
		data = NO_CQ_DATA;
		flags = 0;
		break;
	default:
		FT_ERR("Unknown RMA op type\n");
		return -FI_EINVAL;
	}

	if (!ft_wr_key_matches(&ft_tx_wr_key, op_type, op_buf, op_mr_desc, size,
			       fi_addr, data, 0, flags, rma_addr, rma_key)) {
		ret = ft_wr_prepare_op(ep_ptr, op_type, &ft_tx_wr_key, ft_tx_wr,
				       ft_tx_wr_size, op_buf, op_mr_desc, size,
				       fi_addr, data, 0, flags, rma_addr,
				       rma_key);
		if (ret)
			return ret;
	}

	return ft_wr_queue_and_flush(ep_ptr, ft_tx_wr, ctx, FT_WR_QUEUE_TX);
}
