/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. */

#ifndef FI_XPU_DEVICE_EFA_H
#define FI_XPU_DEVICE_EFA_H

/*
 * EFA Provider - XPU Device-Side Implementation
 *
 * The device-side operations fi_xpu_device.h dispatches to for EFA: the direct
 * data path, the completion queue and the counters. This is also the provider's
 * single entry point, so it pulls in the work request calls as well - the one
 * include core needs, whatever EFA splits its implementation into.
 */

#include <rdma/fi_trigger.h>
#include <rdma/fi_xpu_device_efa_internal.h>
#include <rdma/fi_xpu_device_efa_wr.h>

/*
 * This header is reached through fi_xpu_device.h, which defines FI_XPU_FUNC.
 * Provide a fallback so it also compiles on its own.
 */
#ifndef FI_XPU_FUNC
#define FI_XPU_FUNC static inline
#endif

/*
 * Device-side data transfer operations
 */
FI_XPU_FUNC int
efa_xpu_write(void *ep, const void *buf, size_t len, void *desc,
		 uint64_t data, void *dest_addr, uint64_t addr,
		 uint64_t key, void *context, uint64_t flags, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	struct efa_io_tx_wqe_128 wqe;
	struct iovec iov;
	struct fi_rma_iov rma_iov;
	struct fi_op_rma op;
	void *descs[1];
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	/*
	 * Every thread of the group formats and posts its own descriptor; only
	 * the slot it lands in is agreed between them.
	 */
	efa_xpu_rma_op_init(&op, &iov, descs, &rma_iov, (void *) buf, len, desc,
			    dest_addr, addr, key, data, flags);

	ret = efa_xpu_format_rma(e, &op, &wqe, EFA_IO_RDMA_WRITE);
	if (!ret) {
		/*
		 * The context identifies this call rather than the descriptor,
		 * so it goes in after the shared formatting.
		 */
		efa_xpu_set_wrid(e, &wqe.meta, context);
		ret = efa_xpu_post_wqe(e, &g, &wqe, flags);
	}

	return efa_xpu_group_leave(&g, ret);
}

FI_XPU_FUNC int
efa_xpu_read(void *ep, void *buf, size_t len, void *desc,
		void *src_addr, uint64_t addr, uint64_t key,
		void *context, uint64_t flags, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	struct efa_io_tx_wqe_128 wqe;
	struct iovec iov;
	struct fi_rma_iov rma_iov;
	struct fi_op_rma op;
	void *descs[1];
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	efa_xpu_rma_op_init(&op, &iov, descs, &rma_iov, buf, len, desc,
			    src_addr, addr, key, 0, flags);

	ret = efa_xpu_format_rma(e, &op, &wqe, EFA_IO_RDMA_READ);
	if (!ret) {
		efa_xpu_set_wrid(e, &wqe.meta, context);
		ret = efa_xpu_post_wqe(e, &g, &wqe, flags);
	}

	return efa_xpu_group_leave(&g, ret);
}

FI_XPU_FUNC int
efa_xpu_send(void *ep, const void *buf, size_t len, void *desc,
		uint64_t data, void *dest_addr, void *context,
		uint64_t flags, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	struct efa_io_tx_wqe_128 wqe;
	struct iovec iov;
	struct fi_op_msg op;
	void *descs[1];
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	efa_xpu_msg_op_init(&op, &iov, descs, (void *) buf, len, desc,
			    dest_addr, data, flags);

	ret = efa_xpu_format_send(e, &op, &wqe);
	if (!ret) {
		efa_xpu_set_wrid(e, &wqe.meta, context);
		ret = efa_xpu_post_wqe(e, &g, &wqe, flags);
	}

	return efa_xpu_group_leave(&g, ret);
}

FI_XPU_FUNC int
efa_xpu_recv(void *ep, void *buf, size_t len, void *desc,
		void *src_addr, void *context, uint64_t flags, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	struct efa_io_rx_desc rqe;
	struct iovec iov;
	struct fi_op_msg op;
	void *descs[1];
	int ret;

	(void)src_addr;
	(void)context;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	/* One receive descriptor per thread, as on the send queue. */
	efa_xpu_msg_op_init(&op, &iov, descs, buf, len, desc, NULL, 0, flags);

	ret = efa_xpu_format_recv(e, &op, &rqe);
	if (!ret)
		ret = efa_xpu_post_rqe(e, &g, &rqe, flags);

	return efa_xpu_group_leave(&g, ret);
}

/*
 * Counter operations
 *
 * A counter is only read here, never advanced, so every thread that reads one
 * gets the same answer whatever scope it is called at and the reads need no
 * agreement between threads. efa_xpu_cntr_read and efa_xpu_cntr_readerr return
 * the counter value itself, so they also have no way to report an incompatible
 * handle or an unsupported scope; a kernel reaches them only after the endpoint
 * or completion queue it drives has already been accepted by
 * efa_xpu_ep_compat / efa_xpu_cq_compat.
 */
FI_XPU_FUNC uint64_t
efa_xpu_cntr_read(void *cntr, int scope)
{
	struct efa_xpu_cntr *c = efa_xpu_cntr_resolve(cntr);
	(void)scope;
	return *c->value;
}

FI_XPU_FUNC uint64_t
efa_xpu_cntr_readerr(void *cntr, int scope)
{
	struct efa_xpu_cntr *c = efa_xpu_cntr_resolve(cntr);
	(void)scope;
	if (!c->err_value)
		return 0;
	return *c->err_value;
}

FI_XPU_FUNC void
efa_xpu_cntr_wait(void *cntr, uint64_t threshold,
		     int timeout, int scope)
{
	struct efa_xpu_cntr *c = efa_xpu_cntr_resolve(cntr);
	struct efa_xpu_group g;

	(void)timeout;

	/* Never spin on a handle whose layout this kernel cannot interpret. */
	if (!efa_xpu_cntr_compat(c))
		return;

	if (efa_xpu_group_enter(&g, scope))
		return;

	/*
	 * Every thread sees the same counter, so the wait needs no leader. What
	 * the group scope adds is the barrier below: no thread of the group
	 * leaves the wait until all of them have seen the threshold reached.
	 */
	while (*c->value < threshold)
		;

	efa_xpu_group_sync(&g);
}

/*
 * CQ operations
 */

/*
 * A successful completion. The EFA status codes live in the provider's private
 * efa_errno.h, so the one value the device needs is named here; it is the base
 * of that enumeration and cannot move without breaking the hardware interface.
 */
#define EFA_XPU_COMP_STATUS_OK	0

/**
 * Read a field out of a completion's flags byte.
 *
 * The shift comes from the mask rather than being written out, so the field
 * offsets stay wherever efa_io_defs.h puts them. Every caller passes a constant
 * mask, which folds the loop away.
 */
FI_XPU_FUNC uint32_t
efa_xpu_field_get(uint32_t mask, uint32_t reg)
{
	uint32_t shift = 0;

	while (mask && !(mask & 1u)) {
		mask >>= 1;
		shift++;
	}

	return (reg >> shift) & mask;
}

/**
 * The libfabric completion flags a hardware completion stands for.
 *
 * A send queue completion describes the operation this side posted; a receive
 * queue completion describes what arrived, which for RDMA is the remote side's
 * write landing here.
 */
FI_XPU_FUNC uint64_t
efa_xpu_cqe_flags(uint8_t cqe_flags)
{
	uint32_t q_type = efa_xpu_field_get(EFA_IO_CDESC_COMMON_Q_TYPE_MASK,
					    cqe_flags);
	uint32_t op_type = efa_xpu_field_get(EFA_IO_CDESC_COMMON_OP_TYPE_MASK,
					     cqe_flags);
	uint64_t flags = 0;

	if (q_type == EFA_IO_SEND_QUEUE) {
		switch (op_type) {
		case EFA_IO_RDMA_WRITE:
			flags = FI_RMA | FI_WRITE;
			break;
		case EFA_IO_RDMA_READ:
			flags = FI_RMA | FI_READ;
			break;
		default:
			flags = FI_MSG | FI_SEND;
			break;
		}
	} else {
		switch (op_type) {
		case EFA_IO_RDMA_WRITE:
			flags = FI_RMA | FI_REMOTE_WRITE;
			break;
		default:
			flags = FI_MSG | FI_RECV;
			break;
		}
		if (cqe_flags & EFA_IO_CDESC_COMMON_HAS_IMM_MASK)
			flags |= FI_REMOTE_CQ_DATA;
	}

	return flags;
}

/**
 * The request ID a send queue completion carries.
 *
 * A device-posted work request puts the application's context in the request ID
 * itself, because the device has no wr_id pool to translate an index through.
 * That needs the send queue's 64-bit request IDs, which fi_ep_export_xpu reports
 * in the endpoint handle: without them the upper words are not part of the
 * request ID and what comes back here is the send queue slot the descriptor was
 * posted to.
 */
FI_XPU_FUNC uint64_t
efa_xpu_cqe_req_id(const struct efa_io_tx_cdesc *tx)
{
	return (uint64_t) tx->common.req_id |
	       ((uint64_t) tx->req_id_ex.w[0] << 16) |
	       ((uint64_t) tx->req_id_ex.w[1] << 32) |
	       ((uint64_t) tx->req_id_ex.w[2] << 48);
}

/** The number of bytes a receive queue completion transferred. */
FI_XPU_FUNC uint64_t
efa_xpu_cqe_len(struct efa_xpu_cq *c, const struct efa_io_rx_cdesc *rx)
{
	uint32_t op_type = efa_xpu_field_get(EFA_IO_CDESC_COMMON_OP_TYPE_MASK,
					     rx->common.flags);
	uint64_t len = rx->length;

	/*
	 * An RDMA write can land more than 64KiB, so its length has 16 more
	 * bits in the extended completion - which is only there when the ring
	 * was created with entries big enough to hold it.
	 */
	if (op_type == EFA_IO_RDMA_WRITE &&
	    c->entry_size >= sizeof(struct efa_io_rx_cdesc_ex)) {
		const struct efa_io_rx_cdesc_ex *ex =
			(const struct efa_io_rx_cdesc_ex *) rx;

		len |= (uint64_t) ex->u.rdma_write.length_hi << 16;
	}

	return len;
}

/**
 * Fill one entry of the caller's buffer from a hardware completion.
 *
 * The entry is written in the format the CQ was opened with, so a kernel reads
 * the same struct out of fi_xpu_cq_read() that a host thread reads out of
 * fi_cq_read() on the same CQ.
 */
FI_XPU_FUNC void
efa_xpu_cq_fill(struct efa_xpu_cq *c, const struct efa_io_cdesc_common *cqe,
		void *entry)
{
	struct fi_cq_data_entry e;
	uint32_t q_type = efa_xpu_field_get(EFA_IO_CDESC_COMMON_Q_TYPE_MASK,
					    cqe->flags);
	size_t len;

	e.flags = efa_xpu_cqe_flags(cqe->flags);
	e.buf = NULL;
	e.data = 0;

	if (q_type == EFA_IO_SEND_QUEUE) {
		e.op_context = (void *) (uintptr_t)
			efa_xpu_cqe_req_id((const struct efa_io_tx_cdesc *) cqe);
		e.len = 0;
	} else {
		const struct efa_io_rx_cdesc *rx =
			(const struct efa_io_rx_cdesc *) cqe;

		/*
		 * A receive descriptor has only 16 bits of request ID, so what
		 * comes back is the receive queue slot rather than a context.
		 * An unsolicited completion has no descriptor behind it at all.
		 */
		e.op_context = (cqe->flags & EFA_IO_CDESC_COMMON_UNSOLICITED_MASK) ?
			NULL : (void *) (uintptr_t) cqe->req_id;
		e.len = efa_xpu_cqe_len(c, rx);
		if (e.flags & FI_REMOTE_CQ_DATA)
			e.data = rx->imm;
	}

	/*
	 * The three formats are prefixes of one another, so the entry is built
	 * once and as much of it as the format asks for is copied out.
	 */
	switch (c->format) {
	case FI_CQ_FORMAT_CONTEXT:
		len = sizeof(struct fi_cq_entry);
		break;
	case FI_CQ_FORMAT_MSG:
		len = sizeof(struct fi_cq_msg_entry);
		break;
	default:
		len = sizeof(struct fi_cq_data_entry);
		break;
	}

	memcpy(entry, &e, len);
}

/**
 * Stage a failing completion for fi_xpu_cq_readerr().
 *
 * There is room for one, which is all a kernel that stops at the first error
 * needs; a second error while one is still staged is counted and dropped rather
 * than overwriting what has not been read yet.
 */
FI_XPU_FUNC void
efa_xpu_cq_stage_err(struct efa_xpu_cq *c,
		     const struct efa_io_cdesc_common *cqe)
{
	struct fi_cq_data_entry e;

	if (EFA_XPU_ATOMIC_CAS(&c->err_pending, 0u, 1u) != 0u) {
		EFA_XPU_ATOMIC_ADD(&c->err_dropped, 1u);
		return;
	}

	efa_xpu_cq_fill(c, cqe, &e);
	c->err_op_context = (uint64_t) (uintptr_t) e.op_context;
	c->err_flags = e.flags;
	c->err_len = e.len;
	EFA_XPU_STORE(&c->err_status, cqe->status);
}

/**
 * Take the next completion, if the NIC has written one.
 *
 * The consumer cursor is claimed with a compare and swap rather than an
 * unconditional add: a thread that finds no completion has to leave the cursor
 * where it was for the next caller, and only the thread that moves the cursor
 * off a slot may report the completion in it. Which phase bit a slot should
 * carry follows from the cursor, so the cursor is the only state to move.
 *
 * The slot stays readable after the claim - the NIC does not write it again
 * until the ring has wrapped all the way round - so the caller reads the
 * completion out of the ring rather than under the cursor.
 */
FI_XPU_FUNC const struct efa_io_cdesc_common *
efa_xpu_cq_consume(struct efa_xpu_cq *c)
{
	for (;;) {
		uint32_t cc = EFA_XPU_LOAD(&c->cc);
		uint32_t cq_offset = (cc & c->queue_mask) * c->entry_size;
		uint8_t *slot = c->buf + cq_offset;
		volatile uint8_t *flags = (volatile uint8_t *) slot +
			offsetof(struct efa_io_cdesc_common, flags);
		int expected = (c->init_phase ^
				(int)(cc >> c->queue_size_shift)) & 1;
		int actual = *flags & EFA_IO_CDESC_COMMON_PHASE_MASK;

		if (actual != expected)
			return NULL;	/* no completion ready */

		if (EFA_XPU_ATOMIC_CAS(&c->cc, cc, cc + 1) != cc)
			continue;	/* another thread took this one */

		/*
		 * Nothing else in the completion may be read before its phase
		 * bit has been seen, or a wrapped ring hands back the previous
		 * pass's contents.
		 */
		EFA_XPU_FENCE_BLOCK();
		return (const struct efa_io_cdesc_common *) slot;
	}
}

/**
 * Read up to @count completions into @buf.
 *
 * Pollers do not divide the queue between them ahead of time: each claims one
 * completion at a time, so any number of threads can poll the same CQ
 * concurrently and each fills its own buffer with the completions it claimed.
 * @buf may be NULL, which consumes the completions without reporting them - a
 * kernel that only needs to know how many operations finished.
 */
FI_XPU_FUNC int64_t
efa_xpu_cq_poll(struct efa_xpu_cq *c, void *buf, size_t count)
{
	int64_t n = 0;

	/* A staged error is reported before anything after it. */
	if (EFA_XPU_LOAD(&c->err_pending))
		return -FI_EAVAIL;

	while ((size_t) n < count) {
		const struct efa_io_cdesc_common *cqe = efa_xpu_cq_consume(c);

		if (!cqe)
			break;

		if (cqe->status != EFA_XPU_COMP_STATUS_OK) {
			efa_xpu_cq_stage_err(c, cqe);
			return n ? n : -FI_EAVAIL;
		}

		if (buf)
			efa_xpu_cq_fill(c, cqe,
					(uint8_t *) buf +
					(size_t) n * c->user_entry_size);
		n++;
	}

	return n ? n : -FI_EAGAIN;
}

FI_XPU_FUNC int64_t
efa_xpu_cq_read(void *cq, void *buf, size_t count, int scope)
{
	struct efa_xpu_cq *c = efa_xpu_cq_resolve(cq);
	struct efa_xpu_group g;
	int ret;

	if (!efa_xpu_cq_compat(c))
		return -FI_EOPNOTSUPP;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	/*
	 * A group polls once and shares the result rather than polling once per
	 * thread the way it posts once per thread: there is one buffer and one
	 * count between them, so the leader reads into the buffer the group
	 * passed and what the others take back is how many entries are in it.
	 * The count read and every error code fit in an int, which is what the
	 * group can pass between its threads.
	 */
	if (g.leader)
		ret = (int) efa_xpu_cq_poll(c, buf, count);

	return efa_xpu_group_leave(&g, ret);
}

/**
 * Read the staged error completion.
 *
 * Only the fields the hardware completion carries are filled; prov_errno is the
 * EFA completion status, which is what fi_cq_strerror() on the host turns into a
 * message.
 */
FI_XPU_FUNC int64_t
efa_xpu_cq_readerr(void *cq, void *buf, uint64_t flags, int scope)
{
	struct efa_xpu_cq *c = efa_xpu_cq_resolve(cq);
	struct efa_xpu_group g;
	int ret;

	(void)flags;

	if (!efa_xpu_cq_compat(c))
		return -FI_EOPNOTSUPP;

	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	if (g.leader) {
		if (!EFA_XPU_LOAD(&c->err_pending)) {
			ret = -FI_EAGAIN;
		} else {
			struct fi_cq_err_entry *e =
				(struct fi_cq_err_entry *) buf;

			if (e) {
				memset(e, 0, sizeof(*e));
				e->op_context =
					(void *) (uintptr_t) c->err_op_context;
				e->flags = c->err_flags;
				e->len = c->err_len;
				e->prov_errno = (int) EFA_XPU_LOAD(&c->err_status);
				e->err = FI_EIO;
			}
			EFA_XPU_FENCE_BLOCK();
			EFA_XPU_STORE(&c->err_pending, 0u);
			ret = 1;
		}
	}

	return efa_xpu_group_leave(&g, ret);
}

#endif /* FI_XPU_DEVICE_EFA_H */
