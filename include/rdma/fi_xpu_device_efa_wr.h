/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */

#ifndef FI_XPU_DEVICE_EFA_WR_H
#define FI_XPU_DEVICE_EFA_WR_H

/*
 * struct fi_wr_attr and the fi_op_* descriptors it names. fi_xpu_device.h
 * includes this too, for its own dispatch, but this header is also reached
 * through fi_xpu_device_efa.h alone - the provider's own sources include that
 * one directly - so it brings in what it reads rather than relying on whoever
 * included it.
 */
#include <rdma/fi_wr.h>
#include <rdma/fi_trigger.h>

/*
 * EFA work request calls for an XPU kernel.
 *
 * Included by rdma/fi_xpu_device_efa.h, whose data path machinery this builds
 * on; it is not meant to be included on its own.
 *
 * fi_wr(3) splits an operation's lifecycle into prepare, modify, queue and
 * flush, so that formatting a request is separated in time and place from
 * submitting it. On a device that division is what lets a kernel pay for the
 * formatting once, outside its inner loop - or have the host pay for it before
 * the kernel is launched at all - and then submit the same request as often as
 * it likes.
 *
 * A prepared request holds everything about the operation and nothing about
 * the queue: the slot's phase bit and its request ID belong to the queueing,
 * not to the request, and the post writes them into the queue rather than back
 * into the request (see fi_wr.h). That is what makes one prepared request
 * queueable by any number of threads at once, which is the only form that is
 * useful to a kernel.
 *
 * The request is formatted into the widest send queue entry the device defines,
 * so one prepared request serves a queue of either width and matches byte for
 * byte what the host's fi_wr_prepare() produces. A narrow queue takes the
 * leading bytes of it, which are the same fields in the same places.
 */

/*
 * The sizes fi_xpu_ctx_attr reports as max_tx_wr_desc_size and
 * max_rx_wr_desc_size, and the same ones efa_wr_tx_size() and
 * efa_wr_rx_size() report on the host.
 */
FI_XPU_FUNC size_t
efa_xpu_wr_tx_size(void)
{
	return sizeof(struct efa_io_tx_wqe_128);
}

FI_XPU_FUNC size_t
efa_xpu_wr_rx_size(size_t num_sge)
{
	return num_sge * sizeof(struct efa_io_rx_desc);
}

/*
 * A work request carries no tag of its own saying which queue it belongs to,
 * and queueing a receive descriptor on the send queue would hand the device
 * sixteen bytes of nonsense. The meta descriptor bit every send queue entry
 * sets, and no receive descriptor does, is what tells them apart: a receive
 * descriptor's first four bytes are the low half of a buffer address, so the
 * bit is clear there unless the buffer happens to be so aligned - which it
 * cannot be, since the bit lies in the low byte and a registered buffer is at
 * least two-byte aligned.
 */
FI_XPU_FUNC int
efa_xpu_wr_is_tx(const void *wr)
{
	const struct efa_io_tx_wqe_128 *wqe = (const struct efa_io_tx_wqe_128 *) wr;

	return !!(wqe->meta.ctrl1 & EFA_IO_TX_META_DESC_META_DESC_MASK);
}

/*
 * Format a send or an RMA request into @wqe.
 *
 * The iovec, the descriptor and the peer address come out of the fi_wr_attr
 * the same way they do for a direct post, except that on a device the address
 * and the memory descriptor are the raw handles fi_av_lookup2(FI_XPU) and
 * fi_mr_get_xpu_desc() gave the application, carried in the fi_msg fields that
 * hold host handles on the host.
 */
FI_XPU_FUNC int
efa_xpu_wr_format_msg(struct efa_io_tx_wqe_128 *wqe,
		      const struct fi_op_msg *op, uint32_t op_type)
{
	const struct fi_msg *msg = &op->msg;
	struct efa_xpu_peer *peer;
	struct efa_xpu_desc *d;

	if (!msg->msg_iov || msg->iov_count != 1 || !msg->desc || !msg->addr)
		return -FI_EINVAL;

	peer = (struct efa_xpu_peer *) (uintptr_t) msg->addr;
	d = (struct efa_xpu_desc *) msg->desc[0];
	if (!d)
		return -FI_EINVAL;

	efa_xpu_init_meta(&wqe->meta, op_type, peer);

	if (op->flags & FI_REMOTE_CQ_DATA) {
		wqe->meta.ctrl1 |= EFA_IO_TX_META_DESC_HAS_IMM_MASK;
		wqe->meta.immediate_data = (uint32_t) msg->data;
	}

	efa_xpu_set_buf(&wqe->data.sgl[0], d->lkey,
			(uint64_t) (uintptr_t) msg->msg_iov[0].iov_base,
			(uint32_t) msg->msg_iov[0].iov_len);
	wqe->meta.length = 1;
	return 0;
}

FI_XPU_FUNC int
efa_xpu_wr_format_rma(struct efa_io_tx_wqe_128 *wqe,
		      const struct fi_op_rma *op, uint32_t op_type)
{
	const struct fi_msg_rma *msg = &op->msg;
	struct efa_xpu_peer *peer;
	struct efa_xpu_desc *d;

	if (!msg->msg_iov || msg->iov_count != 1 || !msg->desc ||
	    !msg->rma_iov || msg->rma_iov_count != 1 || !msg->addr)
		return -FI_EINVAL;

	peer = (struct efa_xpu_peer *) (uintptr_t) msg->addr;
	d = (struct efa_xpu_desc *) msg->desc[0];
	if (!d)
		return -FI_EINVAL;

	efa_xpu_init_meta(&wqe->meta, op_type, peer);

	/* A read reports no immediate, as efa_xpu_read() does not either. */
	if (op_type == EFA_IO_RDMA_WRITE && (op->flags & FI_REMOTE_CQ_DATA)) {
		wqe->meta.ctrl1 |= EFA_IO_TX_META_DESC_HAS_IMM_MASK;
		wqe->meta.immediate_data = (uint32_t) msg->data;
	}

	efa_xpu_set_remote(&wqe->data.rdma_req.remote_mem,
			   (uint32_t) msg->rma_iov[0].key,
			   msg->rma_iov[0].addr,
			   (uint32_t) msg->rma_iov[0].len);
	efa_xpu_set_buf(&wqe->data.rdma_req.local_mem[0], d->lkey,
			(uint64_t) (uintptr_t) msg->msg_iov[0].iov_base,
			(uint32_t) msg->msg_iov[0].iov_len);
	wqe->meta.length = 1;
	return 0;
}

/*
 * Format a receive request into @rqe.
 *
 * A receive descriptor's request ID is the slot, written when the descriptor
 * is queued, so there is nothing of the queue here either. Only a
 * single-segment request can be formatted: a longer one needs as many
 * consecutive slots as it has segments, and a thread claims one.
 */
FI_XPU_FUNC int
efa_xpu_wr_format_recv(struct efa_io_rx_desc *rqe, const struct fi_op_msg *op)
{
	const struct fi_msg *msg = &op->msg;
	struct efa_xpu_desc *d;
	uint64_t addr;

	if (!msg->msg_iov || msg->iov_count != 1 || !msg->desc)
		return -FI_EINVAL;

	d = (struct efa_xpu_desc *) msg->desc[0];
	if (!d)
		return -FI_EINVAL;

	addr = (uint64_t) (uintptr_t) msg->msg_iov[0].iov_base;
	rqe->buf_addr_lo = (uint32_t) (addr & 0xFFFFFFFF);
	rqe->buf_addr_hi = (uint32_t) (addr >> 32);
	rqe->req_id = 0;
	rqe->length = (uint16_t) msg->msg_iov[0].iov_len;
	rqe->lkey_ctrl = (d->lkey & EFA_IO_RX_DESC_LKEY_MASK) |
			 EFA_IO_RX_DESC_FIRST_MASK | EFA_IO_RX_DESC_LAST_MASK;
	return 0;
}

/* The one operation an fi_xpu_wr_prepare() call amounts to. */
FI_XPU_FUNC int
efa_xpu_wr_prepare_one(const struct fi_wr_attr *attr, void *wr, size_t *wr_len)
{
	size_t len;
	int ret;

	switch (attr->op_type) {
	case FI_OP_RECV:
		if (!attr->op.msg)
			return -FI_EINVAL;
		len = efa_xpu_wr_rx_size(attr->op.msg->msg.iov_count);
		break;
	case FI_OP_SEND:
		if (!attr->op.msg)
			return -FI_EINVAL;
		len = efa_xpu_wr_tx_size();
		break;
	case FI_OP_WRITE:
	case FI_OP_READ:
		if (!attr->op.rma)
			return -FI_EINVAL;
		len = efa_xpu_wr_tx_size();
		break;
	default:
		return -FI_ENOSYS;
	}

	if (*wr_len < len)
		return -FI_ETOOSMALL;

	memset(wr, 0, len);

	switch (attr->op_type) {
	case FI_OP_RECV:
		ret = efa_xpu_wr_format_recv((struct efa_io_rx_desc *) wr,
					     attr->op.msg);
		break;
	case FI_OP_SEND:
		ret = efa_xpu_wr_format_msg((struct efa_io_tx_wqe_128 *) wr,
					    attr->op.msg, EFA_IO_SEND);
		break;
	case FI_OP_WRITE:
		ret = efa_xpu_wr_format_rma((struct efa_io_tx_wqe_128 *) wr,
					    attr->op.rma, EFA_IO_RDMA_WRITE);
		break;
	default:
		ret = efa_xpu_wr_format_rma((struct efa_io_tx_wqe_128 *) wr,
					    attr->op.rma, EFA_IO_RDMA_READ);
		break;
	}

	if (ret)
		return ret;

	*wr_len = len;
	return 0;
}

/**
 * Format an operation into a work request a kernel can queue later.
 *
 * @wr_len is the size of @wr on the way in and what was used on the way out,
 * as on the host. A caller sizes the buffer from the sizes fi_xpu_ctx_query()
 * reports.
 *
 * Every thread of the group formats the same request from the same arguments
 * into the same buffer, which is one operation's worth of work however many
 * threads make the call, so the leader does it and the group takes its result.
 */
FI_XPU_FUNC int
efa_xpu_wr_prepare(void *ep, const struct fi_wr_attr *attr, void *wr,
		   size_t *wr_len, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (!attr || !wr || !wr_len)
		return -FI_EINVAL;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader)
		ret = efa_xpu_wr_prepare_one(attr, wr, wr_len);

	return efa_xpu_group_leave(&g, ret);
}

/**
 * Queue a prepared send or RMA request on the send queue.
 *
 * The request is const and may be queued by other threads at the same time,
 * so it is copied and only the copy is given what belongs to this queueing:
 * the slot's phase bit and the request ID carrying @context, both written by
 * the post.
 *
 * The doorbell is left for a flush, as the host's fi_wr_queue_tx() leaves it:
 * a kernel queueing a batch rings once at the end rather than once per
 * request, so nothing here is on the fabric until fi_xpu_tx_flush() - or a
 * later batch filling the device's staging limit - rings it.
 */
FI_XPU_FUNC int
efa_xpu_wr_queue_tx(void *ep, const void *wr, void *context, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (!wr)
		return -FI_EINVAL;

	if (!efa_xpu_wr_is_tx(wr))
		return -FI_EINVAL;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader) {
		struct efa_io_tx_wqe wqe;

		/*
		 * The leading bytes of the prepared request, which are the
		 * whole of it for every operation formatted today: the wide
		 * entry's extra room holds inline data, which nothing puts
		 * there yet.
		 */
		memcpy(&wqe, wr, sizeof(wqe));
		ret = efa_xpu_post_wqe(e, &wqe, context, FI_MORE);
	}

	return efa_xpu_group_leave(&g, ret);
}

/**
 * Queue a prepared receive request on the receive queue.
 *
 * A receive descriptor holds a buffer, a length and a key and nothing else, so
 * the request is taken apart and posted the way a direct receive is; the slot
 * it lands in supplies its request ID.
 *
 * The receive descriptor's request ID is the slot, which leaves nowhere to put
 * a context, so one is kept in the queue's context table as it is for a direct
 * receive.
 */
FI_XPU_FUNC int
efa_xpu_wr_queue_recv(void *ep, const void *wr, void *context, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	const struct efa_io_rx_desc *prepared = (const struct efa_io_rx_desc *) wr;
	struct efa_xpu_group g;
	uint64_t buf;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (!wr)
		return -FI_EINVAL;

	/* An endpoint whose queue pair has no receive queue has none to fill. */
	if (!e->rq.db)
		return -FI_EOPNOTSUPP;

	if (efa_xpu_wr_is_tx(wr))
		return -FI_EINVAL;

	/*
	 * A request describing more than one segment needs as many consecutive
	 * slots as it has segments, where a thread here claims one. Until a
	 * claim can cover a run, only a request whose first segment is also
	 * its last can be queued - which the host side can queue either way,
	 * so such a request is still worth preparing.
	 */
	if (!(prepared[0].lkey_ctrl & EFA_IO_RX_DESC_LAST_MASK))
		return -FI_EOPNOTSUPP;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	buf = ((uint64_t) prepared[0].buf_addr_hi << 32) |
	      prepared[0].buf_addr_lo;

	if (g.leader)
		ret = efa_xpu_post_rqe(e, (void *) (uintptr_t) buf,
				       prepared[0].length,
				       prepared[0].lkey_ctrl &
					       EFA_IO_RX_DESC_LKEY_MASK,
				       context, FI_MORE);

	return efa_xpu_group_leave(&g, ret);
}

/*
 * Flush initiates what a deferred post or a queued request left unrung. It is
 * one operation for the whole scope however many threads call it: there is one
 * queue and one doorbell, so the group's threads would otherwise contend to
 * ring the same span. The leader rings and the result goes back to the group,
 * as the completion reads do.
 *
 * Flags are reserved on both. A failure means the endpoint is unusable: a
 * per-request error belongs to the post that produced it, so there is nothing
 * here a caller could retry.
 */
FI_XPU_FUNC int
efa_xpu_tx_flush(void *ep, uint64_t flags, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (flags)
		return -FI_EINVAL;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader)
		ret = efa_xpu_wq_flush(&e->sq);

	return efa_xpu_group_leave(&g, ret);
}

FI_XPU_FUNC int
efa_xpu_recv_flush(void *ep, uint64_t flags, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (flags)
		return -FI_EINVAL;

	/*
	 * An endpoint whose queue pair was created without a receive queue has
	 * no doorbell to ring. The geometry is only filled in where the queue
	 * exists, so a null doorbell is what says it does not.
	 */
	if (!e->rq.db)
		return -FI_EOPNOTSUPP;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader)
		ret = efa_xpu_wq_flush(&e->rq);

	return efa_xpu_group_leave(&g, ret);
}

#endif /* FI_XPU_DEVICE_EFA_WR_H */
