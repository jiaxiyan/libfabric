/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. */

#ifndef FI_XPU_DEVICE_EFA_WR_H
#define FI_XPU_DEVICE_EFA_WR_H

/*
 * EFA Provider - XPU Device-Side Work Request Implementation
 *
 * The work request calls split a transfer into formatting it, modifying it,
 * queueing it and initiating it, so that a kernel which knows its transfers
 * ahead of time can format them once and spend its inner loop only queueing.
 * They are reported separately from the rest of the device API, through
 * FI_XPU_CAP_WR, and so live in their own header - which is also how the host
 * side is arranged.
 *
 * Everything here builds on fi_xpu_device_efa_internal.h: the handle layouts, the
 * cooperative scopes, the queues, and the descriptor formatters shared with
 * the direct data path. The dependency runs one way only. Consumers include
 * fi_xpu_device.h, which pulls in both and dispatches on prov_id.
 */

#include <rdma/fi_wr.h>
#include <rdma/fi_xpu_device_efa_internal.h>

/*
 * This header is included by fi_xpu_device.h which defines FI_XPU_FUNC.
 * Provide fallback for standalone compilation.
 */
#ifndef FI_XPU_FUNC
#define FI_XPU_FUNC static inline
#endif

/*
 * Work request preparation
 *
 * fi_xpu_wr_prepare formats a descriptor into memory the caller owns, without
 * claiming a queue slot or telling the device anything. What it writes is
 * everything the transfer needs except the two things that only become known
 * when the request is queued: the slot's phase bit, and the request ID. Those
 * are filled in by efa_xpu_post_wqe() once a slot has been claimed, which is
 * also why the same prepared request may be queued any number of times.
 *
 * The descriptor is byte-for-byte what the host's fi_wr_prepare() produces, so
 * a request prepared on either side can be queued from the other. That is why
 * the transmit form is the wide layout whatever the queue negotiated: it is the
 * larger of the two and a superset of the narrow one, and the queue path copies
 * only the bytes its slots hold.
 *
 * Addressing is in the device's own form, as it is for every other call here:
 * fi_msg::addr carries the address of the fi_xpu_ctx_attr::av_addr_size bytes
 * that fi_av_lookup2() returned, not an fi_addr_t to be resolved through an AV,
 * and each fi_msg::desc entry points at the fi_xpu_ctx_attr::mr_desc_size bytes
 * that fi_mr_get_xpu_desc() returned, not at a host memory region handle. A
 * kernel has neither the address vector nor the region to resolve the host
 * forms against. fi_op_msg::ep and fi_op_rma::ep are unused: the endpoint comes
 * from the handle this call is made on.
 */

/* The transmit work request is always the wide descriptor. */
FI_XPU_FUNC size_t
efa_xpu_wr_tx_size(void)
{
	return sizeof(struct efa_io_tx_wqe_128);
}

/* A receive work request is one descriptor per segment. */
FI_XPU_FUNC size_t
efa_xpu_wr_rx_size(size_t iov_count)
{
	return iov_count * sizeof(struct efa_io_rx_desc);
}

FI_XPU_FUNC int
efa_xpu_wr_prepare(void *ep, const struct fi_wr_attr *attr, void *wr,
		   size_t *wr_len, int scope)
{
	struct efa_xpu_ep *e = efa_xpu_ep_resolve(ep);
	struct efa_xpu_group g;
	size_t len;
	int ret;

	if (!efa_xpu_ep_compat(e))
		return -FI_EOPNOTSUPP;

	if (!attr || !wr || !wr_len)
		return -FI_EINVAL;

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
	case FI_OP_READ:
	case FI_OP_WRITE:
		if (!attr->op.rma)
			return -FI_EINVAL;
		len = efa_xpu_wr_tx_size();
		break;
	default:
		/*
		 * Tagged and atomic operations are not available on this
		 * fabric, so there is no descriptor to format for them.
		 */
		return -FI_EOPNOTSUPP;
	}

	if (!len || *wr_len < len)
		return -FI_ETOOSMALL;

	/*
	 * Entered as a group for the same reason the posting calls are: the
	 * scope is checked once, every thread leaves with the same answer, and
	 * FI_XPU_DEVICE is refused in one place. There is nothing to agree on
	 * beyond that, because each thread formats into its own buffer.
	 */
	ret = efa_xpu_group_enter(&g, scope);
	if (ret)
		return ret;

	/*
	 * Zero first: the request is handed to the device as it stands, so a
	 * field this operation does not set has to read as unset rather than as
	 * whatever the caller's buffer happened to hold.
	 */
	memset(wr, 0, len);

	switch (attr->op_type) {
	case FI_OP_RECV:
		ret = efa_xpu_format_recv(e, attr->op.msg,
					   (struct efa_io_rx_desc *) wr);
		break;
	case FI_OP_SEND:
		ret = efa_xpu_format_send(e, attr->op.msg,
					   (struct efa_io_tx_wqe_128 *) wr);
		break;
	case FI_OP_READ:
		ret = efa_xpu_format_rma(e, attr->op.rma,
					  (struct efa_io_tx_wqe_128 *) wr,
					  EFA_IO_RDMA_READ);
		break;
	default:
		ret = efa_xpu_format_rma(e, attr->op.rma,
					  (struct efa_io_tx_wqe_128 *) wr,
					  EFA_IO_RDMA_WRITE);
		break;
	}

	if (!ret)
		*wr_len = len;

	return efa_xpu_group_leave(&g, ret);
}

#endif /* FI_XPU_DEVICE_EFA_WR_H */
