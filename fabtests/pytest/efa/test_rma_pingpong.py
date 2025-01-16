from efa.efa_common import efa_run_client_server_test, has_rdma
from common import perf_progress_model_cli
import pytest
import copy

@pytest.fixture(params=["r:4048,4,4148",
                        "r:8000,4,9000",
                        "r:17000,4,18000"])
def rma_pingpong_message_size(request):
    return request.param


@pytest.mark.parametrize("operation_type", ["writedata"])
@pytest.mark.parametrize("iteration_type",
                         [pytest.param("short", marks=pytest.mark.short),
                          pytest.param("standard", marks=pytest.mark.standard)])
def test_rma_pingpong(cmdline_args, iteration_type, operation_type, rma_bw_completion_semantic, memory_type_bi_dir):
    command = "fi_rma_pingpong -e rdm"
    command = command + " -o " + operation_type + " " + perf_progress_model_cli
    efa_run_client_server_test(cmdline_args, command, iteration_type, rma_bw_completion_semantic, memory_type_bi_dir, "all")


@pytest.mark.functional
@pytest.mark.parametrize("operation_type", ["writedata"])
def test_rma_pingpong_range(cmdline_args, operation_type, rma_bw_completion_semantic, rma_pingpong_message_size, memory_type_bi_dir):
    command = "fi_rma_pingpong -e rdm"
    command = command + " -o " + operation_type
    efa_run_client_server_test(cmdline_args, command, "short", rma_bw_completion_semantic, memory_type_bi_dir, rma_pingpong_message_size)


@pytest.mark.functional
@pytest.mark.parametrize("operation_type", ["writedata"])
def test_rma_pingpong_range_no_inject(cmdline_args, operation_type, rma_bw_completion_semantic, rma_pingpong_message_size, memory_type_bi_dir):
    command = "fi_rma_pingpong -e rdm -j 0"
    command = command + " -o " + operation_type
    efa_run_client_server_test(cmdline_args, command, "short", rma_bw_completion_semantic, memory_type_bi_dir, rma_pingpong_message_size)


@pytest.mark.parametrize("operation_type", ["writedata"])
@pytest.mark.parametrize("iteration_type",
                         [pytest.param("short", marks=pytest.mark.short),
                          pytest.param("standard", marks=pytest.mark.standard)])
def test_rma_pingpong_direct(cmdline_args, iteration_type, operation_type, rma_bw_completion_semantic,
                             memory_type_bi_dir, rma_pingpong_message_size, zcpy_recv_max_msg_size):
    if not has_rdma(cmdline_args, 'writedata'):
        pytest.skip("fi_writedata is not supported")
    command = f"fi_rma_pingpong -e rdm --max-msg-size {zcpy_recv_max_msg_size}"
    command = command + " -o " + operation_type + " " + perf_progress_model_cli
    cmdline_args_copy = copy.copy(cmdline_args)
    cmdline_args_copy.append_environ("FI_EFA_USE_EFA_DIRECT=1")
    efa_run_client_server_test(cmdline_args_copy, command, iteration_type, rma_bw_completion_semantic,
                               memory_type_bi_dir, rma_pingpong_message_size)


@pytest.mark.functional
@pytest.mark.parametrize("operation_type", ["writedata"])
def test_rma_pingpong_range_direct(cmdline_args, operation_type, rma_bw_completion_semantic,
                                   rma_pingpong_message_size, memory_type_bi_dir, zcpy_recv_max_msg_size):
    if not has_rdma(cmdline_args, 'writedata'):
        pytest.skip("fi_writedata is not supported")
    command = f"fi_rma_pingpong -e rdm  --max-msg-size {zcpy_recv_max_msg_size}"
    command = command + " -o " + operation_type
    cmdline_args_copy = copy.copy(cmdline_args)
    cmdline_args_copy.append_environ("FI_EFA_USE_EFA_DIRECT=1")
    efa_run_client_server_test(cmdline_args_copy, command, "short", rma_bw_completion_semantic, memory_type_bi_dir, rma_pingpong_message_size)
