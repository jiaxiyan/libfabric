/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */

#ifndef EFA_WR_H
#define EFA_WR_H

#include <stddef.h>

/*
 * Work Request (WR) API support for efa-direct.  See fi_wr(3).
 *
 * A work request is the device queue entry itself, which is what fixes its
 * size.
 *
 * A transmit work request is a struct efa_io_tx_wqe_128.  The 128 byte form is
 * reported unconditionally: its meta descriptor, scatter gather list and RDMA
 * request layouts are a superset of the 64 byte form, so one work request
 * serves either send queue entry size.  A receive work request is the array of
 * receive queue descriptors, one per receive SGE, because the device consumes
 * one receive queue entry per SGE.
 */

/**
 * @brief Size of a transmit work request, for fi_ep_attr::max_tx_wr_size
 *
 * @return 0 when work requests are not supported by this build
 */
size_t efa_wr_tx_size(void);

/**
 * @brief Size of a receive work request, for fi_ep_attr::max_rx_wr_size
 *
 * Also used to size one prepared receive, which needs a descriptor per iov
 * entry rather than the device maximum.
 *
 * @param num_sge	number of receive SGEs to make room for
 * @return 0 when work requests are not supported by this build
 */
size_t efa_wr_rx_size(size_t num_sge);

#endif /* EFA_WR_H */
