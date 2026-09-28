/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */

#include "efa.h"
#include "efa_io_defs.h"
#include "efa_wr.h"

size_t efa_wr_tx_size(void)
{
#if HAVE_EFA_DATA_PATH_DIRECT
	return sizeof(struct efa_io_tx_wqe_128);
#else
	return 0;
#endif
}

size_t efa_wr_rx_size(size_t num_sge)
{
#if HAVE_EFA_DATA_PATH_DIRECT
	return num_sge * sizeof(struct efa_io_rx_desc);
#else
	return 0;
#endif
}
