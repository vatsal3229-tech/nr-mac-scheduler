"""CLI: python -m tools.ci_agent --artifacts DIR [--pr N --repo owner/name] [--out FILE]"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys

from tools.ci_agent import agent, report
from tools.ci_agent.tools import Workspace, list_artifacts


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--artifacts", required=True, help="directory of downloaded CI artifacts")
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--base", default="origin/main", help="target branch ref for git_diff")
    ap.add_argument("--pr", type=int)
    ap.add_argument("--repo", help="owner/name, for the PR comment")
    ap.add_argument("--out", help="also write the diagnosis JSON here")
    ap.add_argument("--max-iterations", type=int, default=agent.Limits.max_iterations)
    ap.add_argument("--max-tokens", type=int, default=agent.Limits.max_total_tokens)
    a = ap.parse_args()

    # Forked PRs and local runs have no secret: skip cleanly instead of failing the job.
    if not os.environ.get("ANTHROPIC_API_KEY"):
        print("ci_agent: ANTHROPIC_API_KEY not set; skipping analysis.")
        return 0

    import anthropic  # imported late so the skip path needs no dependencies

    ws = Workspace(pathlib.Path(a.repo_root).resolve(), pathlib.Path(a.artifacts).resolve(),
                   a.base)
    listing = list_artifacts(ws)
    failed = sorted({p.split("/")[0].split("\\")[0].removeprefix("ci-logs-")
                     for p in listing.splitlines() if p.startswith("ci-logs-")})
    client = anthropic.Anthropic()
    res = agent.run(client.beta.messages, ws, agent.task_prompt(listing, failed),
                    agent.Limits(max_iterations=a.max_iterations, max_total_tokens=a.max_tokens))

    for line in res.trace:
        print(line)
    print(f"ci_agent: stop={res.stop} iterations={res.iterations} "
          f"tokens={res.input_tokens}+{res.output_tokens}")
    if a.out:
        pathlib.Path(a.out).write_text(json.dumps(
            {"stop": res.stop, "diagnosis": res.diagnosis, "iterations": res.iterations,
             "input_tokens": res.input_tokens, "output_tokens": res.output_tokens}, indent=2))
    body = report.to_markdown(res)
    print(body)
    if a.pr and a.repo:
        token = report.github_token()
        if token:
            report.post_pr_comment(a.repo, a.pr, body, token)
            print(f"ci_agent: commented on {a.repo}#{a.pr}")
        else:
            print("ci_agent: GITHUB_TOKEN not set; not commenting", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
