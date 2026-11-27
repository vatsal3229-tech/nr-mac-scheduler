#!/usr/bin/env python3
"""Offline benchmark: every policy x scenario, deterministic.

    python bench/run_bench.py                     # run, write CSV/plots to bench/out/
    python bench/run_bench.py --update-baseline   # also overwrite bench/baseline.json
    python bench/run_bench.py --check             # CI perf gate: fail on regression

Throughput and fairness depend only on the trace and the code, so a change in
either one is a real behaviour change, not noise. Scheduling-time
percentiles are recorded for information only: they depend on the machine.
"""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ue_sim"))
from ue_sim import cli as ue_cli  # noqa: E402
from ue_sim.scenario import SCENARIOS  # noqa: E402

POLICIES = ("round_robin", "proportional_fair", "max_throughput")
GATED = ("cell_throughput_bps", "jain_fairness")
BASELINE = ROOT / "bench" / "baseline.json"


def run_matrix(binary: str, out: pathlib.Path, slots: int, ues: int, seed: int) -> list[dict]:
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for sc in SCENARIOS:
        trace = out / f"{sc}.trace.csv"
        ue_cli.write_trace(str(trace), sc, ues, slots, seed)
        for pol in POLICIES:
            kpi_path = out / f"{sc}__{pol}.json"
            subprocess.run([binary, "--mode", "offline", "--policy", pol, "--trace", str(trace),
                            "--slots", str(slots), "--out", str(kpi_path)],
                           check=True, capture_output=True, text=True)
            k = json.loads(kpi_path.read_text())
            rows.append({
                "scenario": sc, "policy": pol,
                "cell_throughput_mbps": k["cell_throughput_bps"] / 1e6,
                "cell_throughput_bps": k["cell_throughput_bps"],
                "jain_fairness": k["jain_fairness"],
                "prb_utilization": k["prb_utilization"],
                "sched_time_p50_us": k["sched_time_p50_us"],
                "sched_time_p99_us": k["sched_time_p99_us"],
                "invariant_violations": k["invariant_violations"],
            })
            print(f"{sc:18s} {pol:18s} {rows[-1]['cell_throughput_mbps']:7.2f} Mbps  "
                  f"jain {k['jain_fairness']:.4f}  util {k['prb_utilization']:.3f}")
    return rows


def write_csv(rows: list[dict], path: pathlib.Path) -> None:
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)


def write_markdown(rows: list[dict], path: pathlib.Path, meta: dict) -> None:
    lines = [f"Slots: {meta['slots']}, UEs: {meta['ues']}, seed: {meta['seed']}, 52 PRBs, "
             "1 ms slots.", "",
             "| Scenario | Policy | Cell throughput (Mbps) | Jain fairness | PRB utilization | "
             "Sched p50 (us) | Sched p99 (us) |",
             "|---|---|---:|---:|---:|---:|---:|"]
    for r in rows:
        lines.append(f"| {r['scenario']} | {r['policy']} | {r['cell_throughput_mbps']:.2f} | "
                     f"{r['jain_fairness']:.3f} | {r['prb_utilization']:.3f} | "
                     f"{r['sched_time_p50_us']:.2f} | {r['sched_time_p99_us']:.2f} |")
    path.write_text("\n".join(lines) + "\n")


def plot(rows: list[dict], path: pathlib.Path) -> None:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed; skipping plot", file=sys.stderr)
        return
    # Fixed categorical order (policy -> hue) plus a distinct marker shape, so
    # identity never relies on color alone.
    style = {
        "round_robin": ("#257de0", "o", "Round robin"),
        "proportional_fair": ("#f47230", "s", "Proportional fair"),
        "max_throughput": ("#00b894", "^", "Max throughput"),
    }
    ink, ink2, grid = "#191919", "#666666", "#e6e6e6"
    fig, axes = plt.subplots(1, len(SCENARIOS), figsize=(4.2 * len(SCENARIOS), 4.0), sharey=True)
    for ax, sc in zip(axes, SCENARIOS):
        for r in (r for r in rows if r["scenario"] == sc):
            color, marker, label = style[r["policy"]]
            ax.scatter(r["jain_fairness"], r["cell_throughput_mbps"], s=90, c=color, marker=marker,
                       edgecolors="white", linewidths=2, zorder=3, label=label)
            ax.annotate(label, (r["jain_fairness"], r["cell_throughput_mbps"]),
                        textcoords="offset points", xytext=(8, 4), fontsize=8, color=ink2)
        ax.set_title(sc, fontsize=10, color=ink, loc="left")
        ax.set_xlim(0, 1.05)
        ax.set_xlabel("Jain fairness index", fontsize=9, color=ink2)
        ax.grid(True, color=grid, linewidth=0.8, zorder=0)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(grid)
        ax.tick_params(colors=ink2, labelsize=8)
    axes[0].set_ylabel("Cell throughput (Mbit/s)", fontsize=9, color=ink2)
    axes[0].set_ylim(bottom=0)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper right", ncol=3, frameon=False, fontsize=9)
    fig.suptitle("Throughput vs fairness per scheduling policy (offline replay)", x=0.01,
                 ha="left", fontsize=11, color=ink)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(path, dpi=140)
    print(f"wrote {path}")


def update_readme(out: pathlib.Path) -> None:
    """Replace the README benchmark section with this run's table and plot."""
    readme = ROOT / "README.md"
    text = readme.read_text(encoding="utf-8")
    begin, end = "<!-- BENCH:BEGIN -->", "<!-- BENCH:END -->"
    table = (out / "results.md").read_text(encoding="utf-8")
    plot = ROOT / "docs" / "throughput_vs_fairness.png"
    if (out / "throughput_vs_fairness.png").exists():
        plot.write_bytes((out / "throughput_vs_fairness.png").read_bytes())
    body = (f"{begin}\nFrom `make bench` (offline replay, deterministic; scheduling times are "
            f"machine-dependent).\n\n{table}\n![Throughput vs fairness](docs/{plot.name})\n{end}")
    head, rest = text.split(begin, 1)
    readme.write_text(head + body + rest.split(end, 1)[1], encoding="utf-8", newline="\n")
    print(f"updated {readme}")


def to_baseline(rows: list[dict], meta: dict) -> dict:
    return {"meta": meta,
            "results": {f"{r['scenario']}/{r['policy']}": {k: r[k] for k in GATED} for r in rows}}


def check(rows: list[dict], baseline: dict, tol: float) -> list[str]:
    """Regression = a gated metric dropped by more than `tol` (relative).
    Improvements pass; they should be locked in with --update-baseline."""
    failures = []
    current = to_baseline(rows, baseline["meta"])["results"]
    for key, base in baseline["results"].items():
        if key not in current:
            failures.append(f"{key}: missing from current run")
            continue
        for metric in GATED:
            b, c = base[metric], current[key][metric]
            if c < b * (1 - tol):
                failures.append(f"{key} {metric}: {c:.6g} < baseline {b:.6g} (-{(1 - c / b):.2%})")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--binary", default=str(ROOT / "build" / "release" / "nr_mac_scheduler"))
    ap.add_argument("--out", default=str(ROOT / "bench" / "out"))
    ap.add_argument("--slots", type=int, default=20000)
    ap.add_argument("--ues", type=int, default=10)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--tolerance", type=float, default=0.02)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--readme", action="store_true", help="write the results table into README.md")
    a = ap.parse_args()

    out = pathlib.Path(a.out)
    meta = {"slots": a.slots, "ues": a.ues, "seed": a.seed}
    if a.check:
        base = json.loads(BASELINE.read_text())
        meta = base["meta"]  # always compare like with like
    rows = run_matrix(a.binary, out, meta["slots"], meta["ues"], meta["seed"])
    write_csv(rows, out / "results.csv")
    write_markdown(rows, out / "results.md", meta)
    plot(rows, out / "throughput_vs_fairness.png")

    if any(r["invariant_violations"] for r in rows):
        print("FAIL: scheduler produced invalid allocations", file=sys.stderr)
        return 1
    if a.readme:
        update_readme(out)
    if a.update_baseline:
        BASELINE.write_text(json.dumps(to_baseline(rows, meta), indent=2) + "\n")
        print(f"updated {BASELINE}")
    if a.check:
        failures = check(rows, base, a.tolerance)
        if failures:
            print("PERFORMANCE REGRESSION:\n  " + "\n  ".join(failures), file=sys.stderr)
            return 1
        print(f"perf gate passed ({len(base['results'])} cases, tolerance {a.tolerance:.0%})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
