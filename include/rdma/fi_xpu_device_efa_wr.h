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
 * and there is nothing in a descriptor that could stand in for one: the meta
 * descriptor bit every send queue entry sets lands on bit 7 of a receive
 * descriptor's buffer address, which is set for about half of all buffers. So
 * each queue call reads the request as the kind of descriptor its own queue
 * takes, as the host's queue calls do, and queueing a request on the wrong
 * queue is the caller's error to avoid rather than the provider's to catch.
 */

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

/*
 * Modifying a prepared request.
 *
 * A kernel whose operation differs from one iteration to the next in one field
 * can change that field rather than format the request again - a different
 * peer, a different chunk of the same buffer, a different remote offset. What
 * each call may change follows the descriptor: a field the request was not
 * prepared to carry cannot be added, because there is nowhere in the
 * descriptor to put it.
 *
 * Unlike the queue calls these write to the request, so a request being
 * modified is not one another thread may queue. That is the caller's to
 * arrange, and it is why a kernel that wants per-iteration variety usually
 * does better with a request per issuer, modified by its own issuer alone.
 *
 * @op_type says what the request was prepared as; the request does not record
 * it, and the descriptor a send and an RMA write keep their buffers in is not
 * the same.
 */
FI_XPU_FUNC int
efa_xpu_wr_op_is_tx(enum fi_op_type op_type)
{
	return op_type == FI_OP_SEND || op_type == FI_OP_READ ||
	       op_type == FI_OP_WRITE;
}

/*
 * The buffer descriptor a transmit request keeps its local buffer in: a send
 * holds it in the top level sgl, an RMA request in the rdma_req beside the
 * remote address.
 */
FI_XPU_FUNC struct efa_io_tx_buf_desc *
efa_xpu_wr_local_buf(struct efa_io_tx_wqe_128 *wqe, enum fi_op_type op_type)
{
	if (op_type == FI_OP_SEND)
		return &wqe->data.sgl[0];

	return &wqe->data.rdma_req.local_mem[0];
}

/**
 * Point a prepared request at another peer.
 *
 * @xpu_addr is the raw peer handle fi_av_lookup2(FI_XPU) produced, which is
 * the address already resolved - there is no address vector to look it up in
 * from a kernel, which is the whole difference from the host call.
 *
 * A receive request names no peer, so there is nothing to change and the call
 * succeeds, as it does on the host.
 */
FI_XPU_FUNC int
efa_xpu_wr_modify_addr(void *ep, void *wr, enum fi_op_type op_type,
		       const void *xpu_addr, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (!wr)
		return -FI_EINVAL;

	if (!efa_xpu_wr_op_is_tx(op_type))
		return 0;

	if (!xpu_addr)
		return -FI_EINVAL;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader) {
		struct efa_io_tx_wqe_128 *wqe = (struct efa_io_tx_wqe_128 *) wr;
		const struct efa_xpu_peer *p =
			(const struct efa_xpu_peer *) xpu_addr;

		wqe->meta.ah = p->ahn;
		wqe->meta.dest_qp_num = p->remote_qpn;
		wqe->meta.qkey = p->remote_qkey;
	}

	return efa_xpu_group_leave(&g, ret);
}

/**
 * Replace the local buffer of a prepared request.
 *
 * @desc is the packed array of raw memory descriptors fi_mr_get_xpu_desc()
 * produced, one per buffer and each fi_xpu_ctx_attr::mr_desc_size bytes - not
 * the array of pointers the host call takes.
 *
 * One buffer only, as the prepare accepts: a longer request needs a run of
 * queue slots a thread cannot claim.
 */
FI_XPU_FUNC int
efa_xpu_wr_modify_iov(void *ep, void *wr, enum fi_op_type op_type,
		      const struct iovec *iov, const void *desc, size_t count,
		      int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	const struct efa_xpu_desc *d = (const struct efa_xpu_desc *) desc;
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (!wr || !iov || !desc)
		return -FI_EINVAL;

	if (count != 1)
		return -FI_EOPNOTSUPP;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader) {
		uint64_t addr = (uint64_t) (uintptr_t) iov[0].iov_base;
		uint32_t len = (uint32_t) iov[0].iov_len;

		if (efa_xpu_wr_op_is_tx(op_type)) {
			struct efa_io_tx_wqe_128 *wqe =
				(struct efa_io_tx_wqe_128 *) wr;

			/*
			 * An inline request carries its payload where the
			 * buffer descriptor would be, and nothing prepares
			 * one on the device, so there is none to replace.
			 */
			if (wqe->meta.ctrl1 &
			    EFA_IO_TX_META_DESC_INLINE_MSG_MASK) {
				ret = -FI_EOPNOTSUPP;
			} else {
				efa_xpu_set_buf(efa_xpu_wr_local_buf(wqe,
								     op_type),
						d[0].lkey, addr, len);
				/*
				 * An RMA transfer's length is the total of its
				 * local buffers, so the remote side follows.
				 */
				if (op_type != FI_OP_SEND)
					wqe->data.rdma_req.remote_mem.length =
						len;
			}
		} else {
			struct efa_io_rx_desc *rqe = (struct efa_io_rx_desc *) wr;

			rqe->buf_addr_lo = (uint32_t) (addr & 0xFFFFFFFF);
			rqe->buf_addr_hi = (uint32_t) (addr >> 32);
			rqe->length = (uint16_t) len;
			rqe->lkey_ctrl = (d[0].lkey & EFA_IO_RX_DESC_LKEY_MASK) |
					 EFA_IO_RX_DESC_FIRST_MASK |
					 EFA_IO_RX_DESC_LAST_MASK;
		}
	}

	return efa_xpu_group_leave(&g, ret);
}

/**
 * Move a prepared RMA request to another remote address.
 *
 * The length is not changed here, as it is not on the host: an RMA transfer's
 * length comes from its local buffers, so a request that should move a
 * different number of bytes goes through fi_xpu_wr_modify_iov().
 */
FI_XPU_FUNC int
efa_xpu_wr_modify_rma_iov(void *ep, void *wr, enum fi_op_type op_type,
			  const struct fi_rma_iov *rma_iov, size_t count,
			  int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (!wr || !rma_iov)
		return -FI_EINVAL;

	/* Only an rma request has a remote buffer. */
	if (op_type != FI_OP_READ && op_type != FI_OP_WRITE)
		return -FI_EINVAL;

	if (count != EFA_IO_TX_DESC_NUM_RDMA_BUFS)
		return -FI_EINVAL;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader) {
		struct efa_io_tx_wqe_128 *wqe = (struct efa_io_tx_wqe_128 *) wr;
		struct efa_io_remote_mem_addr *rm =
			&wqe->data.rdma_req.remote_mem;

		if (rma_iov[0].len != rm->length) {
			ret = -FI_EINVAL;
		} else {
			rm->rkey = (uint32_t) rma_iov[0].key;
			rm->buf_addr_lo =
				(uint32_t) (rma_iov[0].addr & 0xFFFFFFFF);
			rm->buf_addr_hi = (uint32_t) (rma_iov[0].addr >> 32);
		}
	}

	return efa_xpu_group_leave(&g, ret);
}

/* EFA has no tagged messaging, on the device or on the host. */
FI_XPU_FUNC int
efa_xpu_wr_modify_tag(void *ep, void *wr, enum fi_op_type op_type,
		      uint64_t tag, uint64_t ignore, int scope)
{
	return -FI_ENOSYS;
}

/**
 * Change the remote CQ data a prepared request carries.
 *
 * It cannot be added: the descriptor has the immediate only if it was prepared
 * with FI_REMOTE_CQ_DATA, and a request without it has nowhere to put one.
 */
FI_XPU_FUNC int
efa_xpu_wr_modify_data(void *ep, void *wr, enum fi_op_type op_type,
		       uint64_t data, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (!wr)
		return -FI_EINVAL;

	/* Only a transmit request carries remote CQ data. */
	if (!efa_xpu_wr_op_is_tx(op_type))
		return -FI_EINVAL;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader) {
		struct efa_io_tx_wqe_128 *wqe = (struct efa_io_tx_wqe_128 *) wr;

		if (!(wqe->meta.ctrl1 & EFA_IO_TX_META_DESC_HAS_IMM_MASK))
			ret = -FI_EINVAL;
		else
			wqe->meta.immediate_data = (uint32_t) data;
	}

	return efa_xpu_group_leave(&g, ret);
}

/*
 * The flags that can be set on a request this provider's device path formats.
 *
 * FI_INJECT is not among them, nor are EFA's own FI_EFA_EXTENDED_MSG and
 * FI_EFA_WR_HIGH_PPS. The first needs the request to carry its payload inline
 * and the second needs completion action fields, and fi_xpu_wr_prepare()
 * formats neither, so neither flag could be made true by setting a bit here.
 * The third would be settable, but it lives in rdma/fi_ext_efa.h, which this
 * header does not include: that header is installed rather than in the source
 * tree, so including it would build only against an installed libfabric. A
 * kernel that wants the hint is a reason to move the flag somewhere both can
 * reach.
 */
#define EFA_XPU_WR_SUPPORTED_FLAGS					\
	(FI_REMOTE_CQ_DATA | FI_COMPLETION | FI_TRANSMIT_COMPLETE |	\
	 FI_DELIVERY_COMPLETE)

/**
 * Change the operation flags of a prepared request.
 *
 * A flag that chose how the request was formatted cannot be turned on or off
 * afterwards, only stated again to match, so what this amounts to on a device
 * request is a check that the caller and the request still agree.
 */
FI_XPU_FUNC int
efa_xpu_wr_modify_flags(void *ep, void *wr, enum fi_op_type op_type,
			uint64_t flags, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (!wr)
		return -FI_EINVAL;

	if (flags & ~(uint64_t) EFA_XPU_WR_SUPPORTED_FLAGS)
		return -FI_EBADFLAGS;

	/* A receive descriptor carries none of these. */
	if (!efa_xpu_wr_op_is_tx(op_type))
		return 0;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader) {
		struct efa_io_tx_wqe_128 *wqe = (struct efa_io_tx_wqe_128 *) wr;
		int has_imm = !!(wqe->meta.ctrl1 &
				 EFA_IO_TX_META_DESC_HAS_IMM_MASK);

		if (!!(flags & FI_REMOTE_CQ_DATA) != has_imm)
			ret = -FI_EINVAL;
	}

	return efa_xpu_group_leave(&g, ret);
}

#endif /* FI_XPU_DEVICE_EFA_WR_H */
