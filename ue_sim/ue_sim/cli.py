"""Command line: `python -m ue_sim trace ...` or `python -m ue_sim live ...`."""

from __future__ import annotations

import argparse
import socket
import sys
import time

from ue_sim import scenario, wire


def write_trace(path: str, name: str, num_ues: int, slots: int, seed: int) -> int:
    ues = scenario.build(name, num_ues, seed)
    rows = 0
    # newline="\n" so the file is byte-identical on Windows and Linux.
    with open(path, "w", newline="\n") as f:
        f.write("slot,ue_id,cqi,new_bytes\n")
        for s in range(slots):
            for r in scenario.step(ues, s):
                f.write(f"{s},{r.ue_id},{r.cqi},{r.new_bytes}\n")
                rows += 1
    return rows


def resolve(host: str, timeout_s: float = 60.0) -> str:
    """A headless Service only publishes DNS for *ready* pods, so in Kubernetes
    the simulator may start before its cell's name exists. Retry instead of crashing."""
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            return socket.gethostbyname(host)
        except socket.gaierror:
            if time.monotonic() > deadline:
                raise
            print(f"waiting for DNS {host}", file=sys.stderr)
            time.sleep(1.0)


def run_live(host: str, port: int, name: str, num_ues: int, seed: int, duration_s: float,
             interval_ms: float, first_ue_id: int) -> int:
    """Send reports in real time. Python cannot reliably tick every 1 ms, so it
    advances the simulated slot clock to wall-clock time every `interval_ms`
    and sends one batch per tick. Bytes are summed and the latest CQI kept per UE."""
    ues = scenario.build(name, num_ues, seed, first_ue_id)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (resolve(host), port)
    start = time.monotonic()
    slot = 0
    sent = 0
    while True:
        elapsed = time.monotonic() - start
        if elapsed >= duration_s:
            break
        target = int(elapsed * 1000)
        pending: dict[int, wire.UeReport] = {}
        while slot <= target:
            for r in scenario.step(ues, slot):
                prev = pending.get(r.ue_id)
                pending[r.ue_id] = wire.UeReport(
                    r.ue_id, r.cqi, (prev.new_bytes if prev else 0) + r.new_bytes)
            slot += 1
        for dgram in wire.encode_batches(list(pending.values())):
            sock.sendto(dgram, addr)
            sent += 1
        time.sleep(interval_ms / 1000)
    sock.close()
    return sent


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="ue_sim", description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("trace", "live"):
        sp = sub.add_parser(name)
        sp.add_argument("--scenario", default="fading_full", choices=scenario.SCENARIOS)
        sp.add_argument("--ues", type=int, default=10)
        sp.add_argument("--seed", type=int, default=1)
    t = sub.choices["trace"]
    t.add_argument("--slots", type=int, required=True)
    t.add_argument("--out", required=True)
    lv = sub.choices["live"]
    lv.add_argument("--host", default="127.0.0.1")
    lv.add_argument("--port", type=int, default=38472)
    lv.add_argument("--duration", type=float, default=10.0, help="seconds")
    lv.add_argument("--interval-ms", type=float, default=5.0)
    lv.add_argument("--first-ue-id", type=int, default=0)
    a = p.parse_args(argv)

    if a.cmd == "trace":
        rows = write_trace(a.out, a.scenario, a.ues, a.slots, a.seed)
        print(f"wrote {rows} rows to {a.out}", file=sys.stderr)
    else:
        sent = run_live(a.host, a.port, a.scenario, a.ues, a.seed, a.duration, a.interval_ms,
                        a.first_ue_id)
        print(f"sent {sent} datagrams to {a.host}:{a.port}", file=sys.stderr)
    return 0
