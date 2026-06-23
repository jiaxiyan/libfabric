import pytest
from common import ClientServerTest


@pytest.mark.functional
def test_thread_completion_separate_cq(cmdline_args):
    """Separate TX CQ and RX CQ with concurrent TX/RX threads."""
    cmd = "fi_efa_thread_completion"
    test = ClientServerTest(cmdline_args, cmd, message_size=64, fabric="efa",
                            additional_env="FI_EFA_ENABLE_SHM_TRANSFER=0")
    test.run()


@pytest.mark.functional
def test_thread_completion_shared_cq(cmdline_args):
    """Shared CQ with serialized access."""
    cmd = "fi_efa_thread_completion --shared-cq"
    test = ClientServerTest(cmdline_args, cmd, message_size=64, fabric="efa",
                            additional_env="FI_EFA_ENABLE_SHM_TRANSFER=0")
    test.run()


@pytest.mark.functional
def test_thread_completion_separate_cntr(cmdline_args):
    """Separate TX counter and RX counter with concurrent threads."""
    cmd = "fi_efa_thread_completion -t counter"
    test = ClientServerTest(cmdline_args, cmd, message_size=64, fabric="efa",
                            completion_type="counter",
                            additional_env="FI_EFA_ENABLE_SHM_TRANSFER=0")
    test.run()


@pytest.mark.functional
def test_thread_completion_shared_cntr(cmdline_args):
    """Shared counter with serialized access."""
    cmd = "fi_efa_thread_completion --shared-cntr"
    test = ClientServerTest(cmdline_args, cmd, message_size=64, fabric="efa",
                            additional_env="FI_EFA_ENABLE_SHM_TRANSFER=0")
    test.run()


@pytest.mark.functional
@pytest.mark.parametrize("extra_args", [
    "",
    "--shared-cq",
    "-t counter",
    "--shared-cntr",
])
def test_thread_completion_high_volume(cmdline_args, extra_args):
    """Run all configurations with higher message count to stress concurrency."""
    cmd = f"fi_efa_thread_completion --num-msgs 1000 {extra_args}"
    test = ClientServerTest(cmdline_args, cmd, message_size=64, fabric="efa",
                            additional_env="FI_EFA_ENABLE_SHM_TRANSFER=0")
    test.run()
