import pytest
from common import UnitTest
from efa.efa_common import has_rdma


@pytest.mark.pr_ci
@pytest.mark.unit
def test_efa_mmap(cmdline_args):
    rdma_read = has_rdma(cmdline_args, "read")
    cmd = "fi_efa_mmap_test --device-rdma-read {}".format(int(rdma_read))
    test = UnitTest(cmdline_args, cmd)
    test.run()
