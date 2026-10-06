/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All rights reserved. */

/*
 * C-linkage helper functions for gtest.
 *
 * EFA internal headers (efa.h, efa_av.h, etc.) cannot be included from C++
 * because they transitively pull in unix/osd.h which uses C _Complex types.
 * Functions in this file are implemented in efa_gtest_common_helpers.c
 * (compiled as C) and provide a C++-callable interface to EFA internals that
 * the test files need but cannot access directly.
 */

#ifndef EFA_GTEST_COMMON_HELPERS_H
#define EFA_GTEST_COMMON_HELPERS_H

#include <stdbool.h>
#include <stdint.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct efa_ep_addr;

/**
 * @brief Fabricate a unique peer address from the ep's own address, perturbed
 * so it differs from the ep's real GID (and from prior fabricated addrs).
 */
void efa_test_fabricate_addr(struct fid_ep *ep, struct efa_ep_addr *addr);

/**
 * @brief Whether the real selected EFA device advertises both RDMA read and write.
 */
int efa_test_device_supports_rma(void);

/**
 * @brief Set efa_env.track_mr and return its previous value.
 */
int efa_test_set_track_mr(int value);

/**
 * @brief Read the domain's zero-byte bounce buffer address and lkey for the
 * endpoint behind @p ep.
 */
void efa_test_get_zero_byte_bounce_buf(struct fid_ep *ep, uint64_t *addr,
				       uint32_t *lkey);

/**
 * @brief Insert a peer with a fabricated GID via fi_av_insert.
 */
int efa_test_av_insert_fake_gid(struct fid_ep *ep, struct fid_av *av,
				fi_addr_t *addr);

/**
 * @brief Insert the ep's own (self) GID as a peer via fi_av_insert.
 */
int efa_test_av_insert_self(struct fid_ep *ep, struct fid_av *av,
			    fi_addr_t *addr);

/**
 * @brief Insert a fabricated-GID peer via efa_rdm_av_insert_one_implicit.
 */
fi_addr_t efa_test_av_insert_new_ah(struct fid_ep *ep, struct fid_av *av);

struct efa_ibv_cq;
struct efa_context;

/**
 * @brief Allocate an efa_context
 */
struct efa_context *efa_test_alloc_context(uint64_t completion_flags,
					   fi_addr_t addr);

/**
 * @brief Get the efa_ibv_cq embedded in a fid_cq (opaque from C++).
 */
struct efa_ibv_cq *efa_test_get_ibv_cq(struct fid_cq *cq_fid);

/**
 * @brief Get a CQ's internal err_buf
 */
char *efa_test_get_cq_err_buf(struct fid_cq *cq_fid);

/**
 * @brief Capacity of the CQ's internal err_buf (EFA_ERROR_MSG_BUFFER_LENGTH),
 * the upper bound on err_data_size the fallback error path can report.
 */
extern const size_t efa_test_cq_err_buf_len;

/**
 * @brief Get the QP number of the endpoint's underlying EFA QP. Works for the
 * bare efa_base_ep endpoint used by the efa-direct fabric.
 */
uint32_t efa_test_get_qp_num(struct fid_ep *ep);

/**
 * @brief Set status and wr_id on the ibv_cq_ex inside efa_ibv_cq, to simulate a
 * polled completion.
 */
void efa_test_set_ibv_cq_ex(struct efa_ibv_cq *ibv_cq, int status,
			    uint64_t wr_id);

int efa_cq_poll_ibv_cq(ssize_t cqe_to_process, struct efa_ibv_cq *ibv_cq);

ssize_t efa_test_cq_read_staged_data_entry(struct fid_cq *cq_fid,
					   struct fi_cq_data_entry *entry);

struct ibv_ah;

/**
 * @brief Resolve an implicit-AV fi_addr to the underlying ibv_ah.
 */
struct ibv_ah *efa_test_implicit_addr_to_ibv_ah(struct fid_av *av,
						fi_addr_t fi_addr);

size_t efa_test_ope_list_count(struct fid_ep *ep);

/**
 * @brief Get/set an RDM domain's shm_domain.
 */
struct fid_domain *efa_test_get_shm_domain(struct fid_domain *domain);
void efa_test_set_shm_domain(struct fid_domain *domain,
			     struct fid_domain *shm_domain);

/**
 * @brief Get util_domain's refcount.
 */
int efa_test_get_util_domain_ref(struct fid_domain *domain);

/**
 * @brief Try to acquire domain->util_domain.lock, releasing it again on
 * success. Should be run on an observer thread to check whether the lock is
 * currently held.
 * @return 0 if the lock was free, else EBUSY.
 */
int efa_test_util_domain_trylock(struct fid_domain *domain);

/**
 * @brief Unlock domain->util_domain.lock from the current thread.
 */
void efa_test_util_domain_unlock(struct fid_domain *domain);

/**
 * @brief The endpoint's staged receive work request count
 * (efa_base_ep::recv_wr_index), i.e. how many FI_MORE recvs are queued but not
 * yet posted.
 */
size_t efa_test_ep_recv_wr_index(struct fid_ep *ep);

/**
 * @brief The endpoint's send queue count of WQEs staged but not yet rung to the
 * device (efa_data_path_direct_sq::num_wqe_pending). Only meaningful when the
 * direct data path is enabled.
 */
uint32_t efa_test_ep_sq_num_wqe_pending(struct fid_ep *ep);

/**
 * @brief Whether the endpoint has an open ibv_wr batch that fi_tx_flush must
 * complete (efa_base_ep::is_wr_started). Used on the non-direct path where
 * fi_tx_flush calls ibv_wr_complete and clears this flag.
 */
int efa_test_ep_is_wr_started(struct fid_ep *ep);

/**
 * @brief The transmit/receive work request sizes efa-direct reports in
 * fi_ep_attr::max_tx_wr_size / max_rx_wr_size, i.e. efa_wr_tx_size() and
 * efa_wr_rx_size(device max_rq_sge). Both are 0 on builds without the direct
 * data path.
 */
size_t efa_test_wr_tx_size(void);
size_t efa_test_wr_rx_size(void);

/**
 * @brief Whether this build was compiled with the direct data path
 * (HAVE_EFA_DATA_PATH_DIRECT), i.e. efa-direct reports non-zero work request
 * sizes and advertises FI_WR.
 */
int efa_test_have_data_path_direct(void);

/**
 * @brief Initialize the provider so the device queries below are valid, by
 * running a fi_getinfo. Returns 0 on success, a negative fi errno otherwise.
 */
int efa_test_device_probe(void);

/**
 * @brief The selected EFA device's maximum send/recv queue depths, i.e. the
 * ceiling an endpoint's effective tx/rx size is capped by.
 */
size_t efa_test_device_max_tx_size(void);
size_t efa_test_device_max_rx_size(void);

/**
 * @brief Whether the selected EFA device supports wide WQEs, i.e. an inject
 * size above its inline_buf_size.
 */
int efa_test_device_supports_wide_wqe(void);

/**
 * @brief The selected EFA device's inline_buf_size, above which an inject size
 * makes every send queue entry wide.
 */
size_t efa_test_device_inline_buf_size(void);

/**
 * @brief The selected EFA device's maximum send queue depth for a wide WQE
 * configuration with the given inline size, i.e. the depth fi_getinfo reports
 * for such an endpoint. Returns a negative errno when unavailable.
 */
ssize_t efa_test_device_max_wide_wqe_sq_depth(size_t inject_size);

/**
 * @brief The QP capabilities the endpoint's QP is created with, i.e. what
 * efa_base_ep_construct_ibv_qp_init_attr_ex() fills in. Lets a test check that
 * the depths fi_getopt reports are the ones the QP was created with. Any out
 * parameter may be NULL.
 */
void efa_test_ep_qp_cap(struct fid_ep *ep_fid, size_t *max_send_wr,
			size_t *max_recv_wr, size_t *max_inline_data);

/**
 * @brief Value of OFI_HMEM_DATA_DEV_REG_HANDLE (the gdrcopy handle bit), which
 * lives in ofi_mr.h and is not includable from C++.
 */
uint64_t efa_test_ofi_hmem_data_dev_reg_handle(void);

/**
 * @brief Wrapper around the product efa_rdm_mr_shm_flags() so the shm MR flag
 * derivation can be exercised from C++ tests.
 */
uint64_t efa_test_rdm_mr_shm_flags(uint64_t mr_flags, enum fi_hmem_iface iface);

/**
 * @brief Wrapper around the product efa_rdm_rma_verified_copy_iov() for a
 * single peer-supplied rma_iov (struct efa_rma_iov is not includable from C++).
 */
int efa_test_rdm_rma_verified_copy_iov(struct fid_ep *ep_fid, uint64_t addr,
				       size_t len, uint64_t key, uint32_t flags,
				       struct iovec *iov, void **desc);

/*
 * Accessors for a prepared transmit work request (struct efa_io_tx_wqe_128),
 * so a C++ test can inspect the bytes fi_wr_prepare / fi_wr_modify_* wrote
 * without including efa_io_defs.h. All take the opaque fi_wr (void *).
 */
uint16_t efa_test_wr_tx_dest_qp_num(const void *wr);
uint32_t efa_test_wr_tx_qkey(const void *wr);
uint16_t efa_test_wr_tx_ah(const void *wr);
uint16_t efa_test_wr_tx_length(const void *wr);
uint32_t efa_test_wr_tx_imm_data(const void *wr);
int efa_test_wr_tx_has_imm(const void *wr);
int efa_test_wr_tx_inline_msg(const void *wr);
int efa_test_wr_tx_op_type(const void *wr);
int efa_test_wr_tx_high_pps(const void *wr);

/* First send SGL entry (FI_OP_SEND). */
void efa_test_wr_tx_sgl0(const void *wr, uint64_t *addr, uint32_t *len,
			 uint32_t *lkey);
/* RDMA local SGL entry 0 and the remote memory descriptor (FI_OP_READ/WRITE). */
void efa_test_wr_tx_rdma_local0(const void *wr, uint64_t *addr, uint32_t *len,
				uint32_t *lkey);
void efa_test_wr_tx_rdma_remote(const void *wr, uint64_t *addr, uint32_t *key,
				uint32_t *len);

/*
 * Accessors for a prepared receive work request (array of
 * struct efa_io_rx_desc). @p i selects the descriptor.
 */
void efa_test_wr_rx_desc(const void *wr, size_t i, uint64_t *addr,
			 uint16_t *len, uint32_t *lkey, int *first, int *last);

/*
 * Resolve a fi_addr inserted in the endpoint's AV to the AH number and QPN/QKEY
 * stored for it, so a modify_addr test can check the WQE got the right peer.
 */
void efa_test_av_addr_fields(struct fid_ep *ep, fi_addr_t addr, uint16_t *ahn,
			     uint16_t *qpn, uint32_t *qkey);

uint32_t efa_test_ep_rq_wqe_posted(struct fid_ep *ep);

#ifdef __cplusplus
}
#endif

#endif /* EFA_GTEST_COMMON_HELPERS_H */
