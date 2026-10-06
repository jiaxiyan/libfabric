/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SPDX-FileCopyrightText: Copyright Amazon.com, Inc. or its affiliates. All
 * rights reserved. */

#include "efa_gtest_common_helpers.h"
#include "efa_gtest_common_resource.h"
#include <cstring>
#include <endian.h>
#include <gtest/gtest.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_wr.h>
#include "fi_ext_efa.h"

using testing::Test;

enum {
	EFA_TEST_IO_SEND = 0,
	EFA_TEST_IO_RDMA_READ = 1,
	EFA_TEST_IO_RDMA_WRITE = 2,
};

static constexpr uint64_t kRemoteAddr = 0x87654321;
static constexpr uint32_t kRemoteKey = 123456;

class EfaWrTest : public Test
{
	protected:
	struct efa_resource resource = {};
	fi_addr_t peer_addr = FI_ADDR_NOTAVAIL;
	fi_addr_t peer_addr2 = FI_ADDR_NOTAVAIL;
	uint8_t *buf = nullptr;
	uint8_t *buf2 = nullptr;
	size_t buf_size = 4096;
	struct fid_mr *mr = nullptr;
	struct fid_mr *mr2 = nullptr;
	void *desc = nullptr;
	void *desc2 = nullptr;
	void *tx_wr = nullptr;
	void *rx_wr = nullptr;
	size_t tx_wr_size = 0;
	size_t rx_wr_size = 0;

	struct fi_info *make_hints(bool rma)
	{
		struct fi_info *hints = efa_test_alloc_default_hints(
			FI_EP_RDM, EFA_DIRECT_FABRIC_NAME);
		if (!hints)
			return nullptr;
		hints->caps |= FI_WR;
		hints->mode &= ~FI_CONTEXT2;
		if (rma) {
			hints->caps |= FI_RMA;
			hints->mode |= FI_RX_CQ_DATA;
		}
		return hints;
	}

	void construct(bool rma)
	{
		struct fi_info *hints = make_hints(rma);
		ASSERT_NE(hints, nullptr);

		efa_test_resource_construct(&resource, hints);
		ASSERT_NE(resource.ep, nullptr);

		ASSERT_EQ(resource.info->caps & FI_WR, (uint64_t) FI_WR)
			<< "endpoint did not negotiate FI_WR";

		tx_wr_size = resource.info->ep_attr->max_tx_wr_size;
		rx_wr_size = resource.info->ep_attr->max_rx_wr_size;
		ASSERT_GT(tx_wr_size, 0u);
		ASSERT_GT(rx_wr_size, 0u);

		tx_wr = calloc(1, tx_wr_size);
		rx_wr = calloc(1, rx_wr_size);
		ASSERT_NE(tx_wr, nullptr);
		ASSERT_NE(rx_wr, nullptr);

		buf = (uint8_t *) calloc(buf_size, 1);
		buf2 = (uint8_t *) calloc(buf_size, 1);
		ASSERT_NE(buf, nullptr);
		ASSERT_NE(buf2, nullptr);
		uint64_t access = FI_SEND | FI_RECV;
		if (rma)
			access |= FI_READ | FI_WRITE;
		ASSERT_EQ(fi_mr_reg(resource.domain, buf, buf_size, access, 0, 0,
				    0, &mr, NULL),
			  0);
		ASSERT_EQ(fi_mr_reg(resource.domain, buf2, buf_size, access, 0,
				    0, 0, &mr2, NULL),
			  0);
		desc = fi_mr_desc(mr);
		desc2 = fi_mr_desc(mr2);

		ASSERT_EQ(efa_test_av_insert_self(resource.ep, resource.av,
						  &peer_addr),
			  1);
		if (efa_test_av_insert_fake_gid(resource.ep, resource.av,
						&peer_addr2) != 1)
			peer_addr2 = FI_ADDR_NOTAVAIL;
	}

	int prepare_send(uint64_t flags, fi_addr_t addr, void *op_buf,
			 void *op_desc, size_t len)
	{
		struct iovec iov = {op_buf, len};
		struct fi_msg msg = {};
		msg.msg_iov = &iov;
		msg.desc = &op_desc;
		msg.iov_count = 1;
		msg.addr = addr;
		struct fi_op_msg op = {};
		op.ep = resource.ep;
		op.msg = msg;
		op.flags = flags;
		struct fi_wr_attr attr = {};
		attr.op_type = FI_OP_SEND;
		attr.op.msg = &op;
		size_t wr_len = tx_wr_size;
		return fi_wr_prepare(resource.ep, &attr, tx_wr, &wr_len);
	}

	int prepare_rma(enum fi_op_type op_type, uint64_t flags, fi_addr_t addr,
			void *op_buf, void *op_desc, size_t len,
			uint64_t raddr, uint64_t rkey)
	{
		struct iovec iov = {op_buf, len};
		struct fi_rma_iov rma_iov = {raddr, len, rkey};
		struct fi_msg_rma msg = {};
		msg.msg_iov = &iov;
		msg.desc = &op_desc;
		msg.iov_count = 1;
		msg.addr = addr;
		msg.rma_iov = &rma_iov;
		msg.rma_iov_count = 1;
		struct fi_op_rma op = {};
		op.ep = resource.ep;
		op.msg = msg;
		op.flags = flags;
		struct fi_wr_attr attr = {};
		attr.op_type = op_type;
		attr.op.rma = &op;
		size_t wr_len = tx_wr_size;
		return fi_wr_prepare(resource.ep, &attr, tx_wr, &wr_len);
	}

	int prepare_recv(fi_addr_t addr, void *op_buf, void *op_desc, size_t len)
	{
		struct iovec iov = {op_buf, len};
		struct fi_msg msg = {};
		msg.msg_iov = &iov;
		msg.desc = &op_desc;
		msg.iov_count = 1;
		msg.addr = addr;
		struct fi_op_msg op = {};
		op.ep = resource.ep;
		op.msg = msg;
		op.flags = 0;
		struct fi_wr_attr attr = {};
		attr.op_type = FI_OP_RECV;
		attr.op.msg = &op;
		size_t wr_len = rx_wr_size;
		return fi_wr_prepare(resource.ep, &attr, rx_wr, &wr_len);
	}

	void SetUp() override
	{
		memset(&resource, 0, sizeof(resource));
		if (!efa_test_have_data_path_direct())
			GTEST_SKIP() << "build has no efa-direct data path";
		if (efa_test_device_probe() != 0)
			GTEST_SKIP() << "no efa device available";
	}

	void TearDown() override
	{
		if (mr)
			EXPECT_EQ(fi_close(&mr->fid), 0);
		if (mr2)
			EXPECT_EQ(fi_close(&mr2->fid), 0);
		free(buf);
		free(buf2);
		free(tx_wr);
		free(rx_wr);
		efa_test_resource_destruct(&resource);
	}
};

TEST_F(EfaWrTest, modify_addr_send_rewrites_ud_addr)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	if (peer_addr2 == FI_ADDR_NOTAVAIL)
		GTEST_SKIP() << "second peer unavailable (no AH mock)";
	ASSERT_EQ(prepare_send(0, peer_addr, buf, desc, buf_size), 0);

	uint16_t ahn, qpn;
	uint32_t qkey;
	efa_test_av_addr_fields(resource.ep, peer_addr2, &ahn, &qpn, &qkey);

	ASSERT_EQ(fi_wr_modify_addr(resource.ep, tx_wr, FI_OP_SEND, peer_addr2),
		  0);

	EXPECT_EQ(efa_test_wr_tx_ah(tx_wr), ahn);
	EXPECT_EQ(efa_test_wr_tx_dest_qp_num(tx_wr), qpn);
	EXPECT_EQ(efa_test_wr_tx_qkey(tx_wr), qkey);
}

TEST_F(EfaWrTest, modify_addr_recv_is_noop_success)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_recv(peer_addr, buf, desc, buf_size), 0);

	EXPECT_EQ(fi_wr_modify_addr(resource.ep, rx_wr, FI_OP_RECV, peer_addr),
		  0);
	EXPECT_EQ(fi_wr_modify_addr(resource.ep, rx_wr, FI_OP_RECV,
				    FI_ADDR_UNSPEC),
		  0);
}

TEST_F(EfaWrTest, modify_addr_send_unspec_rejected)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_send(0, peer_addr, buf, desc, buf_size), 0);

	EXPECT_EQ(fi_wr_modify_addr(resource.ep, tx_wr, FI_OP_SEND,
				    FI_ADDR_UNSPEC),
		  -FI_EINVAL);
}

TEST_F(EfaWrTest, modify_iov_send_rewrites_sgl)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_send(0, peer_addr, buf, desc, buf_size), 0);

	struct iovec iov = {buf2, 2048};
	ASSERT_EQ(fi_wr_modify_iov(resource.ep, tx_wr, FI_OP_SEND, &iov, &desc2,
				   1),
		  0);

	uint64_t addr;
	uint32_t len, lkey;
	efa_test_wr_tx_sgl0(tx_wr, &addr, &len, &lkey);
	EXPECT_EQ(addr, (uint64_t) buf2);
	EXPECT_EQ(len, 2048u);
	EXPECT_EQ(efa_test_wr_tx_length(tx_wr), 1);
}

TEST_F(EfaWrTest, modify_iov_write_syncs_remote_length)
{
	if (!efa_test_device_supports_rma())
		GTEST_SKIP() << "device does not support RDMA";
	ASSERT_NO_FATAL_FAILURE(construct(true));
	ASSERT_EQ(prepare_rma(FI_OP_WRITE, 0, peer_addr, buf, desc, buf_size,
			      kRemoteAddr, kRemoteKey),
		  0);

	struct iovec iov = {buf2, 1024};
	ASSERT_EQ(fi_wr_modify_iov(resource.ep, tx_wr, FI_OP_WRITE, &iov,
				   &desc2, 1),
		  0);

	uint64_t laddr;
	uint32_t llen, llkey;
	efa_test_wr_tx_rdma_local0(tx_wr, &laddr, &llen, &llkey);
	EXPECT_EQ(laddr, (uint64_t) buf2);
	EXPECT_EQ(llen, 1024u);

	uint32_t rlen;
	efa_test_wr_tx_rdma_remote(tx_wr, nullptr, nullptr, &rlen);
	EXPECT_EQ(rlen, 1024u);
}

TEST_F(EfaWrTest, modify_iov_recv_rebuilds_descriptors)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_recv(peer_addr, buf, desc, buf_size), 0);

	struct iovec iov = {buf2, 512};
	ASSERT_EQ(fi_wr_modify_iov(resource.ep, rx_wr, FI_OP_RECV, &iov, &desc2,
				   1),
		  0);

	uint64_t addr;
	uint16_t len;
	uint32_t lkey;
	int first, last;
	efa_test_wr_rx_desc(rx_wr, 0, &addr, &len, &lkey, &first, &last);
	EXPECT_EQ(addr, (uint64_t) buf2);
	EXPECT_EQ(len, 512);
	EXPECT_EQ(first, 1);
	EXPECT_EQ(last, 1);
}

TEST_F(EfaWrTest, modify_rma_iov_sets_remote)
{
	if (!efa_test_device_supports_rma())
		GTEST_SKIP() << "device does not support RDMA";
	ASSERT_NO_FATAL_FAILURE(construct(true));
	ASSERT_EQ(prepare_rma(FI_OP_READ, 0, peer_addr, buf, desc, buf_size,
			      kRemoteAddr, kRemoteKey),
		  0);

	struct fi_rma_iov rma_iov = {0x1122334455667788ULL, buf_size, 0xABCD};
	ASSERT_EQ(fi_wr_modify_rma_iov(resource.ep, tx_wr, FI_OP_READ, &rma_iov,
				       1),
		  0);

	uint64_t raddr;
	uint32_t rkey;
	efa_test_wr_tx_rdma_remote(tx_wr, &raddr, &rkey, nullptr);
	EXPECT_EQ(raddr, 0x1122334455667788ULL);
	EXPECT_EQ(rkey, 0xABCDu);
}

TEST_F(EfaWrTest, modify_tag_rejected)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_send(0, peer_addr, buf, desc, buf_size), 0);

	EXPECT_EQ(fi_wr_modify_tag(resource.ep, tx_wr, FI_OP_SEND, 0x1234, 0),
		  -FI_EINVAL);
}

TEST_F(EfaWrTest, modify_data_updates_immediate)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_send(FI_REMOTE_CQ_DATA, peer_addr, buf, desc,
			       buf_size),
		  0);

	ASSERT_EQ(efa_test_wr_tx_has_imm(tx_wr), 1);
	ASSERT_EQ(fi_wr_modify_data(resource.ep, tx_wr, FI_OP_SEND, 0xDEADBEEF),
		  0);

	EXPECT_EQ(efa_test_wr_tx_imm_data(tx_wr), be32toh(0xDEADBEEF));
	EXPECT_EQ(efa_test_wr_tx_has_imm(tx_wr), 1);
}

TEST_F(EfaWrTest, modify_data_without_cq_data_rejected)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_send(0, peer_addr, buf, desc, buf_size), 0);

	ASSERT_EQ(efa_test_wr_tx_has_imm(tx_wr), 0);
	EXPECT_EQ(fi_wr_modify_data(resource.ep, tx_wr, FI_OP_SEND, 0xDEADBEEF),
		  -FI_EINVAL);
}

TEST_F(EfaWrTest, modify_flags_cq_data_mismatch_rejected)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_send(0, peer_addr, buf, desc, buf_size), 0);

	EXPECT_EQ(fi_wr_modify_flags(resource.ep, tx_wr, FI_OP_SEND,
				     FI_REMOTE_CQ_DATA),
		  -FI_EINVAL);
	EXPECT_EQ(fi_wr_modify_flags(resource.ep, tx_wr, FI_OP_SEND, 0), 0);
}

TEST_F(EfaWrTest, modify_flags_high_pps_set_and_clear)
{
	if (!efa_test_device_supports_rma())
		GTEST_SKIP() << "device does not support RDMA";
	ASSERT_NO_FATAL_FAILURE(construct(true));
	ASSERT_EQ(prepare_rma(FI_OP_WRITE, 0, peer_addr, buf, desc, buf_size,
			      kRemoteAddr, kRemoteKey),
		  0);

	ASSERT_EQ(fi_wr_modify_flags(resource.ep, tx_wr, FI_OP_WRITE,
				     FI_EFA_WR_HIGH_PPS),
		  0);
	EXPECT_EQ(efa_test_wr_tx_high_pps(tx_wr), 1);

	ASSERT_EQ(fi_wr_modify_flags(resource.ep, tx_wr, FI_OP_WRITE, 0), 0);
	EXPECT_EQ(efa_test_wr_tx_high_pps(tx_wr), 0);
}

TEST_F(EfaWrTest, modify_flags_high_pps_on_send_rejected)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_send(0, peer_addr, buf, desc, buf_size), 0);

	EXPECT_EQ(fi_wr_modify_flags(resource.ep, tx_wr, FI_OP_SEND,
				     FI_EFA_WR_HIGH_PPS),
		  -FI_EINVAL);
}

TEST_F(EfaWrTest, prepare_send_formats_wqe)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));

	uint16_t ahn, qpn;
	uint32_t qkey;
	efa_test_av_addr_fields(resource.ep, peer_addr, &ahn, &qpn, &qkey);

	ASSERT_EQ(prepare_send(0, peer_addr, buf, desc, 1024), 0);

	EXPECT_EQ(efa_test_wr_tx_op_type(tx_wr), EFA_TEST_IO_SEND);
	EXPECT_EQ(efa_test_wr_tx_ah(tx_wr), ahn);
	EXPECT_EQ(efa_test_wr_tx_dest_qp_num(tx_wr), qpn);
	EXPECT_EQ(efa_test_wr_tx_qkey(tx_wr), qkey);
	EXPECT_EQ(efa_test_wr_tx_has_imm(tx_wr), 0);

	uint64_t addr;
	uint32_t len, lkey;
	efa_test_wr_tx_sgl0(tx_wr, &addr, &len, &lkey);
	EXPECT_EQ(addr, (uint64_t) buf);
	EXPECT_EQ(len, 1024u);
}

TEST_F(EfaWrTest, prepare_send_with_cq_data_sets_imm)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_send(FI_REMOTE_CQ_DATA, peer_addr, buf, desc, 1024),
		  0);
	EXPECT_EQ(efa_test_wr_tx_has_imm(tx_wr), 1);
}

TEST_F(EfaWrTest, prepare_write_formats_wqe)
{
	if (!efa_test_device_supports_rma())
		GTEST_SKIP() << "device does not support RDMA";
	ASSERT_NO_FATAL_FAILURE(construct(true));
	ASSERT_EQ(prepare_rma(FI_OP_WRITE, 0, peer_addr, buf, desc, 2048,
			      kRemoteAddr, kRemoteKey),
		  0);

	EXPECT_EQ(efa_test_wr_tx_op_type(tx_wr), EFA_TEST_IO_RDMA_WRITE);

	uint64_t raddr;
	uint32_t rkey, rlen;
	efa_test_wr_tx_rdma_remote(tx_wr, &raddr, &rkey, &rlen);
	EXPECT_EQ(raddr, kRemoteAddr);
	EXPECT_EQ(rkey, kRemoteKey);
	EXPECT_EQ(rlen, 2048u);
}

TEST_F(EfaWrTest, prepare_read_formats_wqe)
{
	if (!efa_test_device_supports_rma())
		GTEST_SKIP() << "device does not support RDMA";
	ASSERT_NO_FATAL_FAILURE(construct(true));
	ASSERT_EQ(prepare_rma(FI_OP_READ, 0, peer_addr, buf, desc, 2048,
			      kRemoteAddr, kRemoteKey),
		  0);

	EXPECT_EQ(efa_test_wr_tx_op_type(tx_wr), EFA_TEST_IO_RDMA_READ);

	uint64_t laddr;
	uint32_t llen, llkey;
	efa_test_wr_tx_rdma_local0(tx_wr, &laddr, &llen, &llkey);
	EXPECT_EQ(laddr, (uint64_t) buf);
	EXPECT_EQ(llen, 2048u);
}

TEST_F(EfaWrTest, prepare_recv_formats_descriptor)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_recv(peer_addr, buf, desc, 512), 0);

	uint64_t addr;
	uint16_t len;
	uint32_t lkey;
	int first, last;
	efa_test_wr_rx_desc(rx_wr, 0, &addr, &len, &lkey, &first, &last);
	EXPECT_EQ(addr, (uint64_t) buf);
	EXPECT_EQ(len, 512);
	EXPECT_EQ(first, 1);
	EXPECT_EQ(last, 1);
}

TEST_F(EfaWrTest, prepare_too_small_buffer_rejected)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));

	struct iovec iov = {buf, 1024};
	struct fi_msg msg = {};
	msg.msg_iov = &iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = peer_addr;
	struct fi_op_msg op = {};
	op.ep = resource.ep;
	op.msg = msg;
	op.flags = 0;
	struct fi_wr_attr attr = {};
	attr.op_type = FI_OP_SEND;
	attr.op.msg = &op;

	size_t wr_len = 1;
	EXPECT_EQ(fi_wr_prepare(resource.ep, &attr, tx_wr, &wr_len),
		  -FI_ETOOSMALL);
}

TEST_F(EfaWrTest, queue_tx_null_wr_rejected)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	EXPECT_EQ(fi_wr_queue_tx(resource.ep, nullptr, nullptr), -FI_EINVAL);
}

TEST_F(EfaWrTest, queue_tx_stages_wqe)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_send(0, peer_addr, buf, desc, 1024), 0);

	uint32_t before = efa_test_ep_sq_num_wqe_pending(resource.ep);
	ssize_t ret = fi_wr_queue_tx(resource.ep, tx_wr, (void *) 0x1);
	ASSERT_EQ(ret, 0);
	EXPECT_EQ(efa_test_ep_sq_num_wqe_pending(resource.ep), before + 1);
}

TEST_F(EfaWrTest, queue_recv_null_wr_rejected)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	EXPECT_EQ(fi_wr_queue_recv(resource.ep, nullptr, nullptr), -FI_EINVAL);
}

TEST_F(EfaWrTest, queue_recv_stages_descriptor)
{
	ASSERT_NO_FATAL_FAILURE(construct(false));
	ASSERT_EQ(prepare_recv(peer_addr, buf, desc, 512), 0);

	uint32_t before = efa_test_ep_rq_wqe_posted(resource.ep);
	ssize_t ret = fi_wr_queue_recv(resource.ep, rx_wr, (void *) 0x1);
	ASSERT_EQ(ret, 0);
	EXPECT_EQ(efa_test_ep_rq_wqe_posted(resource.ep), before + 1);
}
