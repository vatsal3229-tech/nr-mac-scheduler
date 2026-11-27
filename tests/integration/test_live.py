"""Live end-to-end test: real binary, real UDP, real HTTP scrape."""

import socket
import struct
import time

from conftest import http_get
from ue_sim import cli, wire


def test_probes(scheduler):
    assert http_get(scheduler.base + "/healthz")[0] == 200
    assert http_get(scheduler.base + "/readyz")[0] == 200
    assert http_get(scheduler.base + "/nope")[0] == 404


def test_live_traffic_metrics_are_sane(scheduler):
    # A few hand-made malformed datagrams alongside real simulator traffic.
    bad = [
        b"\x00\x01",                                                     # truncated header
        struct.pack(">HBBHH", 0x524E, 1, 1, 8, 0) + b"\x00" * 8,         # byte-swapped magic
        struct.pack(">HBBHH", wire.MAGIC, 9, 1, 8, 0) + b"\x00" * 8,     # bad version
        wire.encode([wire.UeReport(1, 10, 5)])[:-1],                     # truncated payload
        wire.encode([wire.UeReport(1, 0, 5)]),                           # CQI 0
    ]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        for d in bad:
            s.sendto(d, ("127.0.0.1", scheduler.udp_port))

    sent = cli.run_live("127.0.0.1", scheduler.udp_port, "mixed", num_ues=8, seed=1,
                        duration_s=2.0, interval_ms=5.0, first_ue_id=0)
    assert sent > 100
    time.sleep(0.2)  # let the slot loop consume the tail and publish
    m = scheduler.metrics()

    assert m["nrs_malformed_packets_total"] == len(bad)
    for reason in ("truncated_header", "bad_magic", "bad_version", "length_mismatch", "bad_cqi"):
        assert m[f'nrs_malformed_packets_by_reason_total{{reason="{reason}"}}'] == 1, reason
    assert m["nrs_datagrams_received_total"] == sent + len(bad)
    assert m["nrs_reports_received_total"] > 0
    assert m["nrs_queue_drops_total"] == 0
    assert m["nrs_ues"] == 8

    # ~2.5 s at 1 ms/slot; generous bounds because CI machines are noisy.
    assert 1500 < m["nrs_slots_total"] < 10000
    assert m["nrs_bytes_scheduled_total"] > 0
    assert 0.0 < m["nrs_prb_utilization"] <= 1.0
    assert 0.0 < m["nrs_jain_fairness"] <= 1.0
    # 52 PRBs at CQI 15 carry at most 52*802 bits per 1 ms slot = 41.7 Mbit/s.
    assert 0 < m["nrs_cell_throughput_bps"] <= 52 * 802 / 0.001
    assert m["nrs_invariant_violations_total"] == 0
    # Both come from the same snapshot, so they must agree exactly.
    assert m["nrs_sched_time_us_count"] == m["nrs_slots_total"]
    assert 0 < m["nrs_sched_time_p50_us"] <= m["nrs_sched_time_p99_us"]
    # Overruns happen on a non-RT kernel; just require they are rare.
    assert m["nrs_slot_overruns_total"] < 0.05 * m["nrs_slots_total"]


def test_sigterm_shuts_down_cleanly(scheduler):
    t0 = time.monotonic()
    assert scheduler.terminate() == 0
    assert time.monotonic() - t0 < 3.0
    assert "shutdown complete" in scheduler.proc.stderr.read()


def test_bad_config_fails_fast(binary):
    import os
    import subprocess
    env = dict(os.environ, NRS_POLICY="fastest")
    r = subprocess.run([binary], env=env, capture_output=True, text=True, timeout=5)
    assert r.returncode == 1
    assert "unknown policy" in r.stderr
