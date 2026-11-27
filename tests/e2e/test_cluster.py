"""End-to-end against a kind cluster (run after `make deploy`).

Checks the deployment as a whole: pods healthy and non-root, each simulator
Job finished, and, most importantly, every cell saw only its own UEs,
which proves the per-cell DNS routing through the headless Service.
"""

import json
import re
import socket
import subprocess
import time
import urllib.request

import pytest

NS = "nrs"
CELLS = {"cell-0": range(0, 10), "cell-1": range(100, 110)}


def kubectl(*args: str, check=True) -> str:
    return subprocess.run(["kubectl", "-n", NS, *args], check=check, capture_output=True,
                          text=True).stdout


def parse_metrics(text: str) -> dict[str, float]:
    return {m.group(1): float(m.group(2))
            for m in (re.match(r"^(\S+)\s+(\S+)$", ln) for ln in text.splitlines()
                      if ln and not ln.startswith("#")) if m}


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="module", autouse=True)
def jobs_done():
    subprocess.run(["kubectl", "-n", NS, "wait", "--for=condition=complete", "job",
                    "-l", "app=ue-sim", "--timeout=240s"], check=True)


def scrape(pod: str, path: str = "/metrics") -> tuple[int, str]:
    port = free_port()
    pf = subprocess.Popen(["kubectl", "-n", NS, "port-forward", f"pod/{pod}", f"{port}:8080"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + 15
        while True:
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=2) as r:
                    return r.status, r.read().decode()
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.3)
    finally:
        pf.terminate()
        pf.wait()


@pytest.mark.parametrize("pod", sorted(CELLS))
def test_pod_healthy_and_hardened(pod):
    p = json.loads(kubectl("get", "pod", pod, "-o", "json"))
    assert p["status"]["phase"] == "Running"
    cs = p["status"]["containerStatuses"][0]
    assert cs["ready"] and cs["restartCount"] == 0
    c = p["spec"]["containers"][0]
    assert c["securityContext"]["readOnlyRootFilesystem"] is True
    assert p["spec"]["securityContext"]["runAsNonRoot"] is True
    assert scrape(pod, "/readyz")[0] == 200


@pytest.mark.parametrize("pod", sorted(CELLS))
def test_each_cell_sees_only_its_own_ues(pod):
    status, body = scrape(pod)
    assert status == 200
    m = parse_metrics(body)
    ue_ids = {int(k.split('"')[1]) for k in m if k.startswith("nrs_ue_cqi{")}
    assert ue_ids == set(CELLS[pod]), f"{pod} saw UEs {sorted(ue_ids)}"
    assert m["nrs_bytes_scheduled_total"] > 0
    assert m["nrs_malformed_packets_total"] == 0
    assert m["nrs_invariant_violations_total"] == 0
    assert 0 < m["nrs_prb_utilization"] <= 1
