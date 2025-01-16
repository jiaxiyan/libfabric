from efa.efa_common import efa_run_client_server_test, has_rdma
from common import perf_progress_model_cli
import pytest
import copy


@pytest.mark.parametrize("iteration_type",
                         [pytest.param("short", marks=pytest.mark.short),
                          pytest.param("standard", marks=pytest.mark.standard)])
def test_rma_bw(cmdline_args, iteration_type, rma_operation_type, rma_bw_completion_semantic, rma_bw_memory_type):
    command = "fi_rma_bw -e rdm"
    command = command + " -o " + rma_operation_type + " " + perf_progress_model_cli
    # rma_bw test with data verification takes longer to finish
    timeout = max(540, cmdline_args.timeout)
    efa_run_client_server_test(cmdline_args, command, iteration_type, rma_bw_completion_semantic, rma_bw_memory_type, "all", timeout=timeout)

@pytest.mark.parametrize("env_vars", [["FI_EFA_TX_SIZE=64"], ["FI_EFA_RX_SIZE=64"], ["FI_EFA_TX_SIZE=64", "FI_EFA_RX_SIZE=64"]])
def test_rma_bw_small_tx_rx(cmdline_args, rma_operation_type, rma_bw_completion_semantic, rma_bw_memory_type, env_vars):
    cmdline_args_copy = copy.copy(cmdline_args)
    for env_var in env_vars:
        cmdline_args_copy.append_environ(env_var)
    # Use a window size larger than tx/rx size
    command = "fi_rma_bw -e rdm -W 128"
    command = command + " -o " + rma_operation_type + " " + perf_progress_model_cli
    # rma_bw test with data verification takes longer to finish
    timeout = max(540, cmdline_args_copy.timeout)
    efa_run_client_server_test(cmdline_args_copy, command, "short", rma_bw_completion_semantic, rma_bw_memory_type, "all", timeout=timeout)

@pytest.mark.functional
def test_rma_bw_range(cmdline_args, rma_operation_type, rma_bw_completion_semantic, message_size, rma_bw_memory_type):
    command = "fi_rma_bw -e rdm"
    command = command + " -o " + rma_operation_type
    # rma_bw test with data verification takes longer to finish
    timeout = max(540, cmdline_args.timeout)
    efa_run_client_server_test(cmdline_args, command, "short", rma_bw_completion_semantic, rma_bw_memory_type, message_size, timeout=timeout)


@pytest.mark.functional
def test_rma_bw_range_no_inject(cmdline_args, rma_operation_type, rma_bw_completion_semantic, inject_message_size):
    command = "fi_rma_bw -e rdm -j 0"
    command = command + " -o " + rma_operation_type
    # rma_bw test with data verification takes longer to finish
    timeout = max(540, cmdline_args.timeout)
    efa_run_client_server_test(cmdline_args, command, "short", rma_bw_completion_semantic, "host_to_host", inject_message_size, timeout=timeout)


# This test is run in serial mode because it takes a lot of memory
@pytest.mark.serial
@pytest.mark.functional
# TODO Add "writedata", "write" back in when EFA firmware bug is fixed
@pytest.mark.parametrize("operation_type", ["read"])
def test_rma_bw_1G(cmdline_args, operation_type, rma_bw_completion_semantic):
    # Default window size is 64 resulting in 128GB being registered, which
    # exceeds max number of registered host pages
    timeout = max(540, cmdline_args.timeout)
    command = "fi_rma_bw -e rdm -W 1"
    command = command + " -o " + operation_type
    efa_run_client_server_test(cmdline_args, command, 2,
                               completion_semantic=rma_bw_completion_semantic, message_size=1073741824,
                               memory_type="host_to_host", warmup_iteration_type=0, timeout=timeout)

@pytest.mark.functional
@pytest.mark.parametrize("operation_type", ["writedata", "write"])
def test_rma_bw_use_fi_more(cmdline_args, operation_type, rma_bw_completion_semantic, inject_message_size):
    command = "fi_rma_bw -e rdm -j 0 --use-fi-more"
    command = command + " -o " + operation_type
    # rma_bw test with data verification takes longer to finish
    timeout = max(540, cmdline_args.timeout)
    efa_run_client_server_test(cmdline_args, command, "short", rma_bw_completion_semantic,
                               "host_to_host", inject_message_size, timeout=timeout)


def test_rma_bw_direct(cmdline_args, direct_rma_operation_type,
                       rma_bw_completion_semantic, rma_bw_memory_type,
                       message_size, zcpy_recv_max_msg_size):
    command = f"fi_rma_bw -e rdm --max-msg-size {zcpy_recv_max_msg_size}"
    command = command + " -o " + direct_rma_operation_type + " " + perf_progress_model_cli
    # rma_bw test with data verification takes longer to finish
    timeout = max(540, cmdline_args.timeout)
    cmdline_args_copy = copy.copy(cmdline_args)
    cmdline_args_copy.append_environ("FI_EFA_USE_EFA_DIRECT=1")
    efa_run_client_server_test(cmdline_args_copy, command, "short", rma_bw_completion_semantic, rma_bw_memory_type,
                               message_size, timeout=timeout)

@pytest.mark.functional
def test_rma_bw_range_direct(cmdline_args, direct_rma_operation_type, rma_bw_completion_semantic,
                             message_size, rma_bw_memory_type, zcpy_recv_max_msg_size):
    command = f"fi_rma_bw -e rdm --max-msg-size {zcpy_recv_max_msg_size}"
    command = command + " -o " + direct_rma_operation_type
    # rma_bw test with data verification takes longer to finish
    timeout = max(540, cmdline_args.timeout)
    cmdline_args_copy = copy.copy(cmdline_args)
    cmdline_args_copy.append_environ("FI_EFA_USE_EFA_DIRECT=1")
    efa_run_client_server_test(cmdline_args_copy, command, "short", rma_bw_completion_semantic, rma_bw_memory_type, message_size, timeout=timeout)

# This test is run in serial mode because it takes a lot of memory
@pytest.mark.serial
@pytest.mark.functional
# TODO Add "writedata", "write" back in when EFA firmware bug is fixed
@pytest.mark.parametrize("operation_type", ["read"])
def test_rma_bw_1G_direct(cmdline_args, operation_type, rma_bw_completion_semantic, zcpy_recv_max_msg_size):
    # Default window size is 64 resulting in 128GB being registered, which
    # exceeds max number of registered host pages
    if not has_rdma(cmdline_args, operation_type):
        pytest.skip("fi_{} is not supported".format(operation_type))
    timeout = max(540, cmdline_args.timeout)
    command = f"fi_rma_bw -e rdm -W 1 --max-msg-size {zcpy_recv_max_msg_size}"
    command = command + " -o " + operation_type
    cmdline_args_copy = copy.copy(cmdline_args)
    cmdline_args_copy.append_environ("FI_EFA_USE_EFA_DIRECT=1")
    efa_run_client_server_test(cmdline_args_copy, command, 2,
                               completion_semantic=rma_bw_completion_semantic, message_size=1073741824,
                               memory_type="host_to_host", warmup_iteration_type=0, timeout=timeout)

@pytest.mark.functional
@pytest.mark.parametrize("operation_type", ["writedata", "write"])
def test_rma_bw_use_fi_more_direct(cmdline_args, operation_type, rma_bw_completion_semantic,
                                   inject_message_size, zcpy_recv_max_msg_size):
    if not has_rdma(cmdline_args, operation_type):
        pytest.skip("fi_{} is not supported".format(operation_type))
    command = f"fi_rma_bw -e rdm --use-fi-more --max-msg-size {zcpy_recv_max_msg_size}"
    command = command + " -o " + operation_type
    # rma_bw test with data verification takes longer to finish
    timeout = max(540, cmdline_args.timeout)
    cmdline_args_copy = copy.copy(cmdline_args)
    cmdline_args_copy.append_environ("FI_EFA_USE_EFA_DIRECT=1")
    efa_run_client_server_test(cmdline_args_copy, command, "short", rma_bw_completion_semantic,
                               "host_to_host", inject_message_size, timeout=timeout)
