import os
import pathlib
import re
import signal
import socket
import subprocess
import sys
import time
import urllib.request

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "ue_sim"))


def free_port(kind=socket.SOCK_STREAM) -> int:
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http_get(url: str, timeout: float = 2.0) -> tuple[int, str]:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def parse_metrics(text: str) -> dict[str, float]:
    """Prometheus text -> {"name{labels}": value}, ignoring comments."""
    out = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^(\S+)\s+(\S+)$", line)
        if m:
            out[m.group(1)] = float(m.group(2))
    return out


class Scheduler:
    def __init__(self, binary: str, policy: str):
        self.udp_port = free_port(socket.SOCK_DGRAM)
        self.http_port = free_port()
        env = dict(os.environ,
                   NRS_POLICY=policy,
                   NRS_UDP_BIND="127.0.0.1", NRS_UDP_PORT=str(self.udp_port),
                   NRS_HTTP_BIND="127.0.0.1", NRS_HTTP_PORT=str(self.http_port))
        self.proc = subprocess.Popen([binary], env=env, stderr=subprocess.PIPE, text=True)
        self.base = f"http://127.0.0.1:{self.http_port}"

    def wait_ready(self, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"scheduler exited early: {self.proc.stderr.read()}")
            try:
                if http_get(self.base + "/readyz", 0.5)[0] == 200:
                    return
            except OSError:
                pass
            time.sleep(0.05)
        raise TimeoutError("scheduler never became ready")

    def metrics(self) -> dict[str, float]:
        status, body = http_get(self.base + "/metrics")
        assert status == 200
        return parse_metrics(body)

    def terminate(self, timeout=5.0) -> int:
        self.proc.send_signal(signal.SIGTERM)
        return self.proc.wait(timeout=timeout)


@pytest.fixture
def binary() -> str:
    path = os.environ.get("NRS_BINARY", str(ROOT / "build" / "release" / "nr_mac_scheduler"))
    if not os.path.exists(path):
        pytest.skip(f"scheduler binary not built: {path}")
    return path


@pytest.fixture
def scheduler(binary):
    s = Scheduler(binary, "proportional_fair")
    s.wait_ready()
    yield s
    if s.proc.poll() is None:
        s.proc.kill()
        s.proc.wait()
