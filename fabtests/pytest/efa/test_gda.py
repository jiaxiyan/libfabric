import os
import pytest
from common import ClientServerTest, has_cuda


def _skip_if_gda_unavailable(cmdline_args, fabric):
    binpath = cmdline_args.binpath or ""
    if not os.path.exists(os.path.join(binpath, "fi_efa_gda")):
        pytest.skip("fi_efa_gda is not built")

    if not cmdline_args.do_dmabuf_reg_for_hmem:
        pytest.skip("DMABUF is required for GDA tests")

    if not has_cuda(cmdline_args.client_id) or not has_cuda(cmdline_args.server_id):
        pytest.skip("Client and server both need a cuda device")

    if fabric != "efa-direct":
        pytest.skip("GDA only works for efa-direct fabric")


@pytest.mark.short
@pytest.mark.functional
@pytest.mark.cuda_memory
def test_gda_send_recv(cmdline_args, direct_message_size, fabric):
    _skip_if_gda_unavailable(cmdline_args, fabric)

    test = ClientServerTest(cmdline_args, "fi_efa_gda",
                            message_size=direct_message_size,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.short
@pytest.mark.functional
@pytest.mark.cuda_memory
def test_gda_write_bw(cmdline_args, rma_fabric):
    _skip_if_gda_unavailable(cmdline_args, rma_fabric)

    test = ClientServerTest(cmdline_args, "fi_efa_gda -o write",
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.short
@pytest.mark.functional
@pytest.mark.cuda_memory
def test_gda_writedata_bw(cmdline_args, rma_fabric):
    _skip_if_gda_unavailable(cmdline_args, rma_fabric)

    test = ClientServerTest(cmdline_args, "fi_efa_gda -o writedata",
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.short
@pytest.mark.functional
@pytest.mark.cuda_memory
def test_gda_read_bw(cmdline_args, rma_fabric):
    _skip_if_gda_unavailable(cmdline_args, rma_fabric)

    test = ClientServerTest(cmdline_args, "fi_efa_gda -o read",
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.short
@pytest.mark.functional
@pytest.mark.cuda_memory
def test_gda_send_recv_hw_cntr(cmdline_args, direct_message_size, fabric):
    _skip_if_gda_unavailable(cmdline_args, fabric)

    test = ClientServerTest(cmdline_args, "fi_efa_gda --use-hw-cntr",
                            message_size=direct_message_size,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            additional_env="FI_EFA_USE_HW_CNTR=1")
    test.run()


@pytest.mark.short
@pytest.mark.functional
@pytest.mark.cuda_memory
def test_gda_write_bw_hw_cntr(cmdline_args, rma_fabric):
    _skip_if_gda_unavailable(cmdline_args, rma_fabric)

    test = ClientServerTest(cmdline_args, "fi_efa_gda -o write --use-hw-cntr",
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            additional_env="FI_EFA_USE_HW_CNTR=1")
    test.run()


@pytest.mark.short
@pytest.mark.functional
@pytest.mark.cuda_memory
def test_gda_writedata_bw_hw_cntr(cmdline_args, rma_fabric):
    _skip_if_gda_unavailable(cmdline_args, rma_fabric)

    test = ClientServerTest(cmdline_args, "fi_efa_gda -o writedata --use-hw-cntr",
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            additional_env="FI_EFA_USE_HW_CNTR=1")
    test.run()


@pytest.mark.short
@pytest.mark.functional
@pytest.mark.cuda_memory
def test_gda_read_bw_hw_cntr(cmdline_args, rma_fabric):
    _skip_if_gda_unavailable(cmdline_args, rma_fabric)

    test = ClientServerTest(cmdline_args, "fi_efa_gda -o read --use-hw-cntr",
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            additional_env="FI_EFA_USE_HW_CNTR=1")
    test.run()
