"""Read-only tools the CI agent can call.

Every tool is confined to two roots: the repository checkout and the CI
artifact directory. The agent cannot write files, run commands or reach the
network; the worst a confused (or prompt-injected) model can do is read files
it was already allowed to read.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
from dataclasses import dataclass

# Tool results go back into the model's context on every later turn, so each
# one is capped. The model is told when output was cut and can page with offsets.
MAX_RESULT_CHARS = 12_000
MAX_GREP_MATCHES = 60


class ToolError(Exception):
    """Reported back to the model as an is_error tool_result, not raised further."""


@dataclass
class Workspace:
    repo: pathlib.Path
    artifacts: pathlib.Path
    base_ref: str = "origin/main"

    def _inside(self, root: pathlib.Path, rel: str) -> pathlib.Path:
        p = (root / rel).resolve()
        if p != root.resolve() and root.resolve() not in p.parents:
            raise ToolError(f"path {rel!r} is outside the allowed directory")
        if not p.exists():
            raise ToolError(f"{rel!r} does not exist")
        return p


def _clip(text: str, offset: int = 0) -> str:
    chunk = text[offset : offset + MAX_RESULT_CHARS]
    if offset + MAX_RESULT_CHARS < len(text):
        chunk += (f"\n[... truncated: showing chars {offset}-{offset + MAX_RESULT_CHARS} of "
                  f"{len(text)}; call again with offset={offset + MAX_RESULT_CHARS} for more]")
    return chunk


def read_file(ws: Workspace, path: str, start_line: int = 1, end_line: int | None = None) -> str:
    p = ws._inside(ws.repo, path)
    if p.is_dir():
        return "\n".join(sorted(str(c.relative_to(ws.repo)) for c in p.iterdir()))
    lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
    end = min(end_line or len(lines), len(lines))
    start = max(1, start_line)
    # Line numbers in the output let the model cite file:line evidence accurately.
    return _clip("\n".join(f"{i:5d}  {lines[i - 1]}" for i in range(start, end + 1)))


def grep_repo(ws: Workspace, pattern: str, glob: str | None = None) -> str:
    try:
        rx = re.compile(pattern)
    except re.error as e:
        raise ToolError(f"bad regex: {e}") from e
    files = subprocess.run(["git", "ls-files"], cwd=ws.repo, capture_output=True, text=True,
                           check=True).stdout.splitlines()
    hits = []
    for rel in files:
        if glob and not pathlib.PurePath(rel).match(glob):
            continue
        try:
            text = (ws.repo / rel).read_text(encoding="utf-8")
        except (UnicodeDecodeError, FileNotFoundError):
            continue
        for n, line in enumerate(text.splitlines(), 1):
            if rx.search(line):
                hits.append(f"{rel}:{n}: {line.strip()}")
                if len(hits) >= MAX_GREP_MATCHES:
                    return "\n".join(hits) + f"\n[stopped at {MAX_GREP_MATCHES} matches]"
    return "\n".join(hits) or "no matches"


def git_diff(ws: Workspace, path: str | None = None) -> str:
    """Diff of the working tree against the merge base with the target branch:
    exactly the change under test, nothing that landed on main meanwhile."""
    base = subprocess.run(["git", "merge-base", ws.base_ref, "HEAD"], cwd=ws.repo,
                          capture_output=True, text=True)
    ref = base.stdout.strip() if base.returncode == 0 else ws.base_ref
    cmd = ["git", "diff", ref, "--"] + ([path] if path else [])
    out = subprocess.run(cmd, cwd=ws.repo, capture_output=True, text=True)
    if out.returncode != 0:
        raise ToolError(out.stderr.strip())
    return _clip(out.stdout or "no changes")


def list_artifacts(ws: Workspace) -> str:
    files = sorted(p for p in ws.artifacts.rglob("*") if p.is_file())
    return "\n".join(f"{p.relative_to(ws.artifacts)}  ({p.stat().st_size} bytes)"
                     for p in files) or "no artifacts"


def read_ci_artifact(ws: Workspace, name: str, pattern: str | None = None, offset: int = 0) -> str:
    p = ws._inside(ws.artifacts, name)
    text = p.read_text(encoding="utf-8", errors="replace")
    if pattern:
        try:
            rx = re.compile(pattern)
        except re.error as e:
            raise ToolError(f"bad regex: {e}") from e
        lines = text.splitlines()
        keep = [f"{i + 1}: {ln}" for i, ln in enumerate(lines) if rx.search(ln)]
        return _clip("\n".join(keep) or "no matching lines")
    return _clip(text, offset)


# JSON schemas sent to the API. strict=True guarantees inputs match the schema,
# so the dispatcher below never sees missing or mistyped arguments.
def _schema(props: dict, required: list[str]) -> dict:
    return {"type": "object", "properties": props, "required": required,
            "additionalProperties": False}


TOOL_DEFS = [
    {"name": "list_artifacts", "strict": True,
     "description": "List CI artifact files (job logs, pod logs, kubectl describe output, events) "
                    "with sizes. Start here.",
     "input_schema": _schema({}, [])},
    {"name": "read_ci_artifact", "strict": True,
     "description": "Read a CI artifact by the relative name from list_artifacts. Use `pattern` "
                    "(regex) to return only matching lines with line numbers, e.g. 'FAIL|error|"
                    "Expected|OOMKilled'. Use `offset` to page through long files.",
     "input_schema": _schema({"name": {"type": "string"},
                              "pattern": {"type": ["string", "null"]},
                              "offset": {"type": "integer"}}, ["name", "pattern", "offset"])},
    {"name": "git_diff", "strict": True,
     "description": "Show the change under test (diff against the merge base with the target "
                    "branch), optionally limited to one path. The root cause is usually in here.",
     "input_schema": _schema({"path": {"type": ["string", "null"]}}, ["path"])},
    {"name": "read_file", "strict": True,
     "description": "Read a repository file with line numbers (or list a directory). Use "
                    "start_line/end_line for large files.",
     "input_schema": _schema({"path": {"type": "string"},
                              "start_line": {"type": "integer"},
                              "end_line": {"type": ["integer", "null"]}},
                             ["path", "start_line", "end_line"])},
    {"name": "grep_repo", "strict": True,
     "description": "Regex search over tracked repository files; returns file:line: text. "
                    "Optional glob such as '*.cpp' or 'k8s/*.yaml'.",
     "input_schema": _schema({"pattern": {"type": "string"},
                              "glob": {"type": ["string", "null"]}}, ["pattern", "glob"])},
    {"name": "submit_diagnosis", "strict": True,
     "description": "Finish: report the root cause. Call exactly once, when you have evidence.",
     "input_schema": _schema({
         "summary": {"type": "string", "description": "one-sentence root cause"},
         "category": {"type": "string", "enum": [
             "scheduler_logic", "kpi_math", "concurrency", "wire_protocol", "config",
             "kubernetes_manifest", "resource_limits", "performance_regression", "build",
             "test_flake", "other"]},
         "file": {"type": "string", "description": "repo-relative path of the faulty file"},
         "line": {"type": ["integer", "null"]},
         "evidence": {"type": "array", "items": {"type": "string"},
                      "description": "file:line references and quoted log lines"},
         "suggested_patch": {"type": "string", "description": "unified diff fixing the bug"},
         "confidence": {"type": "string", "enum": ["high", "medium", "low"]},
     }, ["summary", "category", "file", "line", "evidence", "suggested_patch", "confidence"])},
]


def dispatch(ws: Workspace, name: str, args: dict) -> str:
    if name == "list_artifacts":
        return list_artifacts(ws)
    if name == "read_ci_artifact":
        return read_ci_artifact(ws, args["name"], args.get("pattern"), args.get("offset") or 0)
    if name == "git_diff":
        return git_diff(ws, args.get("path"))
    if name == "read_file":
        return read_file(ws, args["path"], args.get("start_line") or 1, args.get("end_line"))
    if name == "grep_repo":
        return grep_repo(ws, args["pattern"], args.get("glob"))
    raise ToolError(f"unknown tool {name}")
