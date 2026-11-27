#!/usr/bin/env python3
"""Evaluate the CI agent on injected bugs with known root causes.

For each bug in bugs/bugs.yaml:
  1. create a clean git worktree of HEAD and apply bugs/<id>.patch (uncommitted,
     so the agent's git_diff against HEAD shows exactly the injected change)
  2. run the pipeline stage that should catch it, capturing its log the same
     way CI does (artifacts/ci-logs-<stage>/...)
  3. if the stage failed, run the agent on those artifacts
  4. score: correct file AND correct cause (category or summary keyword)

    python -m tools.ci_agent.eval                 # all bugs, writes docs/agent_eval.md
    python -m tools.ci_agent.eval --only 04-parser-byte-order
    python -m tools.ci_agent.eval --no-agent      # only check that each bug is detected

Needs Linux (C++ stages), docker + kind for the e2e stage, and
ANTHROPIC_API_KEY for the agent step. Missing pieces are reported as
"skipped", never scored as passes.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
BUGS = ROOT / "bugs"
OUT_MD = ROOT / "docs" / "agent_eval.md"
OUT_JSON = ROOT / "docs" / "agent_eval.json"
# Shared across worktrees so each one does not re-download dependencies.
FETCH_CACHE = ROOT / "build" / "_fetchcontent_cache"

STAGES = {
    "unit": ["make", "unit", "PRESET=release"],
    "tsan": ["make", "unit", "PRESET=tsan"],
    "arch": [sys.executable, "tools/arch_check.py"],
    "perf": ["make", "perf-gate", "PRESET=release"],
    "e2e": ["bash", "-c", "make kind-up && make deploy && make e2e"],
}


def sh(cmd, cwd, log: pathlib.Path | None = None, timeout=1800) -> int:
    env = dict(os.environ, CMAKE_ARGS="", FETCHCONTENT_BASE_DIR=str(FETCH_CACHE))
    env.setdefault("PY", sys.executable)
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        out, rc = f"{e.stdout or ''}{e.stderr or ''}\nTIMEOUT after {timeout}s", 124
    else:
        out, rc = p.stdout + p.stderr, p.returncode
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(out)
    return rc


def stage_available(stage: str) -> str | None:
    """Reason the stage cannot run here, or None."""
    if stage in ("unit", "tsan", "perf") and sys.platform != "linux":
        return "needs Linux"
    if stage == "e2e" and not (shutil.which("kind") and shutil.which("docker")):
        return "needs docker + kind"
    return None


def score(diag: dict | None, bug: dict) -> dict:
    if not diag:
        return {"file_ok": False, "cause_ok": False, "correct": False}
    f = diag.get("file", "").replace("\\", "/").lstrip("./")
    file_ok = f == bug["file"] or f.endswith("/" + bug["file"]) or bug["file"].endswith("/" + f)
    text = (diag.get("summary", "") + " " + " ".join(diag.get("evidence", []))).lower()
    cause_ok = diag.get("category") in bug["categories"] or any(
        k.lower() in text for k in bug["keywords"])
    return {"file_ok": file_ok, "cause_ok": cause_ok, "correct": file_ok and cause_ok}


def run_bug(bug: dict, use_agent: bool) -> dict:
    row = {"id": bug["id"], "title": bug["title"], "stage": bug["check"]}
    reason = stage_available(bug["check"])
    if reason:
        return {**row, "status": f"skipped ({reason})"}
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="nrs-bug-"))
    wt = tmp / "wt"
    subprocess.run(["git", "worktree", "add", "--detach", str(wt), "HEAD"], cwd=ROOT, check=True,
                   capture_output=True)
    try:
        subprocess.run(["git", "apply", str(BUGS / f"{bug['id']}.patch")], cwd=wt, check=True)
        art = tmp / "artifacts"
        stage_dir = art / f"ci-logs-{bug['check']}"
        rc = sh(STAGES[bug["check"]], wt, stage_dir / f"{bug['check']}.log")
        if bug["check"] == "e2e":
            sh(["bash", "tools/collect_k8s_artifacts.sh", str(stage_dir / "k8s")], wt)
        row["detected"] = rc != 0
        if not row["detected"]:
            return {**row, "status": "not detected by stage"}
        if not use_agent:
            return {**row, "status": "detected (agent not run)"}
        if not os.environ.get("ANTHROPIC_API_KEY"):
            return {**row, "status": "detected (agent skipped: no ANTHROPIC_API_KEY)"}
        out = tmp / "diagnosis.json"
        sh([sys.executable, "-m", "tools.ci_agent", "--artifacts", str(art), "--base", "HEAD",
            "--repo-root", str(wt), "--out", str(out)], wt, tmp / "agent.log", timeout=900)
        result = json.loads(out.read_text()) if out.exists() else {"stop": "crashed"}
        d = result.get("diagnosis")
        return {**row, **score(d, bug), "status": "scored", "agent_stop": result.get("stop"),
                "agent_file": (d or {}).get("file"), "agent_category": (d or {}).get("category"),
                "agent_summary": (d or {}).get("summary"),
                "iterations": result.get("iterations"),
                "tokens": (result.get("input_tokens") or 0) + (result.get("output_tokens") or 0)}
    finally:
        if bug["check"] == "e2e":
            sh(["kubectl", "delete", "namespace", "nrs", "--ignore-not-found", "--wait=true"], wt)
        subprocess.run(["git", "worktree", "remove", "--force", str(wt)], cwd=ROOT,
                       capture_output=True)
        shutil.rmtree(tmp, ignore_errors=True)


def write_report(rows: list[dict]) -> None:
    scored = [r for r in rows if r.get("status") == "scored"]
    n_ok = sum(r["correct"] for r in scored)
    lines = [
        "# CI agent evaluation", "",
        f"Generated by `python -m tools.ci_agent.eval` on "
        f"{dt.datetime.now(dt.timezone.utc):%Y-%m-%d %H:%M UTC}. Every number below comes from that run.",
        "",
        "A diagnosis counts as **correct** only if it names the right file *and* the right cause "
        "(matching category or a ground-truth keyword in its summary/evidence). See `bugs/bugs.yaml`.",
        "",
        (f"**Result: {n_ok}/{len(scored)} correct** "
         f"(file right: {sum(r['file_ok'] for r in scored)}, cause right: "
         f"{sum(r['cause_ok'] for r in scored)}). "
         f"{len(rows) - len(scored)} bug(s) not scored, see Status.") if scored else
        "**No bugs were scored in this run.** See Status for why.",
        "",
        "| Bug | Caught by | Status | Agent file | Agent category | Correct | Iterations | Tokens |",
        "|---|---|---|---|---|:-:|--:|--:|",
    ]
    for r in rows:
        ok = "" if r.get("status") != "scored" else ("yes" if r["correct"] else "no")
        lines.append(f"| {r['id']} | {r['stage']} | {r['status']} | {r.get('agent_file') or ''} | "
                     f"{r.get('agent_category') or ''} | {ok} | {r.get('iterations') or ''} | "
                     f"{r.get('tokens') or ''} |")
    OUT_MD.write_text("\n".join(lines) + "\n")
    OUT_JSON.write_text(json.dumps(rows, indent=2) + "\n")
    print(f"wrote {OUT_MD}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--no-agent", action="store_true")
    a = ap.parse_args()
    bugs = yaml.safe_load((BUGS / "bugs.yaml").read_text())
    if a.only:
        bugs = [b for b in bugs if b["id"] in a.only]
    FETCH_CACHE.mkdir(parents=True, exist_ok=True)
    rows = []
    for b in bugs:
        print(f"== {b['id']} ({b['check']})", flush=True)
        rows.append(run_bug(b, not a.no_agent))
        print(f"   {rows[-1]['status']}", flush=True)
    write_report(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
