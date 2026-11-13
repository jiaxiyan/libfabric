import pytest
from common import ClientServerTest
from efa.efa_common import DIRECT_SIZES

# Apply to all functions in this file
pytestmark = [
    pytest.mark.short,
    pytest.mark.functional,
    pytest.mark.cuda_memory,
    pytest.mark.xpu,
    pytest.mark.fabric(params=["efa-direct"]),
]

# The XPU test exchanges addresses out of band: the endpoint a kernel drives is
# selected by FI_XPU in the hints, and the destination it posts to is the raw
# peer handle fi_av_lookup2() produced, not a name fi_getinfo() can resolve.
XPU = "fi_efa_xpu -E"


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES, pr_ci_efa_direct=DIRECT_SIZES)
def test_xpu_send_recv(cmdline_args, message_sizes, fabric):
    test = ClientServerTest(cmdline_args, XPU,
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES, pr_ci_efa_direct=DIRECT_SIZES)
def test_xpu_write_bw(cmdline_args, message_sizes, rma_fabric):
    test = ClientServerTest(cmdline_args, XPU + " -o write",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES, pr_ci_efa_direct=DIRECT_SIZES)
def test_xpu_writedata_bw(cmdline_args, message_sizes, rma_fabric):
    test = ClientServerTest(cmdline_args, XPU + " -o writedata",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES, pr_ci_efa_direct=DIRECT_SIZES)
def test_xpu_read_bw(cmdline_args, message_sizes, rma_fabric):
    test = ClientServerTest(cmdline_args, XPU + " -o read",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_send_recv_cntr(cmdline_args, message_sizes, fabric):
    """Completions taken from a counter the kernel waits on, not from a CQ."""
    test = ClientServerTest(cmdline_args, XPU + " --use-cntr",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_lat(cmdline_args, message_sizes, fabric):
    """Ping-pong, so the kernel waits on receives of its own between sends."""
    test = ClientServerTest(cmdline_args, XPU + " --lat",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=fabric)
    test.run()


@pytest.mark.parametrize("scope,threads",
                         [("work_item", 8), ("subgroup", 32),
                          ("work_group", 64)])
@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_scopes(cmdline_args, message_sizes, fabric, scope, threads):
    """
    A call at a group scope is one operation for the group, so what this
    covers is that a wider scope does not turn one call into several: with -v
    the target checks every chunk against the issuer that should have sent it,
    exactly once.
    """
    test = ClientServerTest(cmdline_args,
                            f"{XPU} --scope {scope} --threads {threads}",
                            message_size=message_sizes,
                            iteration_type=64,
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_multi_block(cmdline_args, message_sizes, fabric):
    """
    Several blocks posting into the same queues, which is the only shape that
    races the posting cursors between blocks rather than between a block's own
    threads.
    """
    test = ClientServerTest(cmdline_args, XPU + " --threads 8 --blocks 4",
                            message_size=message_sizes,
                            iteration_type=64,
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_host_cq(cmdline_args, message_sizes, fabric):
    """
    The kernel only posts and the host reads the completions, far more than
    the queue holds, so a post has to wait for room rather than overrun it.
    """
    test = ClientServerTest(cmdline_args, XPU + " --host-cq",
                            message_size=message_sizes,
                            iteration_type=2000,
                            memory_type="cuda_to_cuda",
                            fabric=fabric)
    test.run()


#
# The same runs, with the kernel formatting each operation once through
# fi_xpu_wr_prepare() and queueing that one request every iteration instead of
# describing it again on every call (--use-wr). The transfers are identical, so
# -v checks the same thing: a prepared descriptor that lost an issuer's own
# arguments would show up as a chunk checked against the wrong issuer.
#

@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES, pr_ci_efa_direct=DIRECT_SIZES)
def test_xpu_wr_send_recv(cmdline_args, message_sizes, fabric):
    test = ClientServerTest(cmdline_args, XPU + " --use-wr",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES, pr_ci_efa_direct=DIRECT_SIZES)
def test_xpu_wr_write_bw(cmdline_args, message_sizes, rma_fabric):
    test = ClientServerTest(cmdline_args, XPU + " --use-wr -o write",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES, pr_ci_efa_direct=DIRECT_SIZES)
def test_xpu_wr_writedata_bw(cmdline_args, message_sizes, rma_fabric):
    test = ClientServerTest(cmdline_args, XPU + " --use-wr -o writedata",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES, pr_ci_efa_direct=DIRECT_SIZES)
def test_xpu_wr_read_bw(cmdline_args, message_sizes, rma_fabric):
    test = ClientServerTest(cmdline_args, XPU + " --use-wr -o read",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=rma_fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_wr_send_recv_cntr(cmdline_args, message_sizes, fabric):
    test = ClientServerTest(cmdline_args, XPU + " --use-wr --use-cntr",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_wr_lat(cmdline_args, message_sizes, fabric):
    """
    Ping-pong, which is where a missing flush shows: nothing is on the fabric
    until fi_xpu_tx_flush() rings for it, so a reply that never comes is a
    doorbell that was never rung.
    """
    test = ClientServerTest(cmdline_args, XPU + " --use-wr --lat",
                            message_size=message_sizes,
                            iteration_type="short",
                            memory_type="cuda_to_cuda",
                            fabric=fabric)
    test.run()


@pytest.mark.parametrize("scope,threads",
                         [("work_item", 8), ("subgroup", 32),
                          ("work_group", 64)])
@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_wr_scopes(cmdline_args, message_sizes, fabric, scope, threads):
    """
    Preparing and queueing are collective at a group scope too, so every
    thread of a group has to reach them with the same request.
    """
    test = ClientServerTest(cmdline_args,
                            f"{XPU} --use-wr --scope {scope} "
                            f"--threads {threads}",
                            message_size=message_sizes,
                            iteration_type=64,
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_wr_multi_block(cmdline_args, message_sizes, fabric):
    """
    Blocks queueing and flushing the same queues at once, which is what the
    doorbell lock exists for: a flush holds no slots and so takes no turn
    against the posters.
    """
    test = ClientServerTest(cmdline_args,
                            XPU + " --use-wr --threads 8 --blocks 4",
                            message_size=message_sizes,
                            iteration_type=64,
                            memory_type="cuda_to_cuda",
                            fabric=fabric,
                            datacheck_type="with_datacheck")
    test.run()


@pytest.mark.message_sizes(default_efa_direct=DIRECT_SIZES)
def test_xpu_wr_host_cq(cmdline_args, message_sizes, fabric):
    test = ClientServerTest(cmdline_args, XPU + " --use-wr --host-cq",
                            message_size=message_sizes,
                            iteration_type=2000,
                            memory_type="cuda_to_cuda",
                            fabric=fabric)
    test.run()
