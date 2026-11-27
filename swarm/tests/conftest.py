"""Fixtures shared by the myra-swarm suites: the scripted server, a clean environment, and
the mode each test runs in: its module's MODE, or a mode(name) mark."""
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))


@pytest.fixture(scope="session")
def server():
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "mock.py")], stdout=subprocess.PIPE,
                         text=True)
    port = int(p.stdout.readline())
    yield f"http://127.0.0.1:{port}/v1"
    p.kill()
    p.wait()


def pytest_configure(config):
    config.addinivalue_line("markers", "mode(name): run this test in --mode name, not the module's MODE")


@pytest.fixture
def mode(request):
    """The test's --mode: its mode mark, else its module's MODE. Each test runs once."""
    mark = request.node.get_closest_marker("mode")
    return mark.args[0] if mark else request.module.MODE


@pytest.fixture
def swarm_cmd(mode):
    """myra-swarm's argv up to its mode-specific arguments; MYRA_BIN is the child myra."""
    return [os.environ["MYRA_SWARM"], "-P", "local", "--mode", mode, "--myra", os.environ["MYRA_BIN"]]


@pytest.fixture
def env(server, tmp_path):
    return {"PATH": "/usr/bin:/bin", "HOME": str(tmp_path), "LOCAL_BASE_URL": server,
            "XDG_STATE_HOME": str(tmp_path / "state")}
