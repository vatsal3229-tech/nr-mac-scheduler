"""Offline tests for the agent loop, using a scripted fake model (no API calls)."""

import pathlib
import subprocess
import sys
from types import SimpleNamespace as NS

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))
from tools.ci_agent import agent, report  # noqa: E402
from tools.ci_agent.tools import Workspace, dispatch, ToolError  # noqa: E402


def tool_use(name, inp, i=0):
    return NS(type="tool_use", name=name, input=inp, id=f"tu_{name}_{i}")


def reply(*blocks, stop="tool_use", tokens=(100, 50)):
    return NS(content=list(blocks), stop_reason=stop,
              usage=NS(input_tokens=tokens[0], output_tokens=tokens[1]))


class FakeClient:
    def __init__(self, script):
        self.script = list(script)
        self.calls = []

    def create(self, **kw):
        # Snapshot: the loop keeps appending to the same list after the call.
        self.calls.append({**kw, "messages": list(kw["messages"])})
        return self.script.pop(0)


DIAG = {"summary": "s", "category": "wire_protocol", "file": "src/proto/wire.cpp", "line": 12,
        "evidence": ["log: bad_magic"], "suggested_patch": "--- a\n+++ b", "confidence": "high"}


@pytest.fixture
def ws(tmp_path):
    repo = tmp_path / "repo"
    (repo / "src").mkdir(parents=True)
    (repo / "src" / "a.cpp").write_text("int x = 1;\nint y = 2;\n")
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    subprocess.run(["git", "add", "."], cwd=repo, check=True)
    subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "x"],
                   cwd=repo, check=True)
    (repo / "src" / "a.cpp").write_text("int x = 1;\nint y = 3;\n")
    art = tmp_path / "art"
    (art / "ci-logs-build-test").mkdir(parents=True)
    (art / "ci-logs-build-test" / "build-test.log").write_text("ok\n[  FAILED  ] Wire.RoundTrip\nok\n")
    return Workspace(repo, art, base_ref="HEAD")


def test_loop_runs_tools_then_submits(ws):
    c = FakeClient([
        reply(NS(type="text", text="look at logs"), tool_use("list_artifacts", {})),
        reply(tool_use("read_ci_artifact", {"name": "ci-logs-build-test/build-test.log",
                                            "pattern": "FAILED", "offset": 0})),
        reply(tool_use("submit_diagnosis", DIAG)),
    ])
    res = agent.run(c, ws, "go")
    assert res.stop == "submitted" and res.diagnosis == DIAG
    assert res.iterations == 3 and res.input_tokens == 300
    # Tool output is fed back as a tool_result in the next request.
    last_user = c.calls[2]["messages"][-1]
    assert last_user["role"] == "user"
    assert "2: [  FAILED  ] Wire.RoundTrip" in last_user["content"][0]["content"]
    assert c.calls[0]["model"] == "claude-opus-5-5"


def test_iteration_limit_and_final_nudge(ws):
    c = FakeClient([reply(tool_use("list_artifacts", {}, i)) for i in range(3)])
    res = agent.run(c, ws, "go", agent.Limits(max_iterations=3))
    assert res.stop == "max_iterations" and res.diagnosis is None
    last = c.calls[-1]["messages"][-1]
    assert last["role"] == "user"
    assert any("Iteration limit reached" in b.get("text", "") for b in last["content"])
    roles = [m["role"] for m in c.calls[-1]["messages"]]
    assert all(a != b for a, b in zip(roles, roles[1:])), "roles must alternate"


def test_token_budget(ws):
    c = FakeClient([reply(tool_use("list_artifacts", {}), tokens=(900, 200))] * 5)
    res = agent.run(c, ws, "go", agent.Limits(max_total_tokens=1000))
    assert res.stop == "token_budget" and res.iterations == 1


def test_tool_errors_are_reported_not_raised(ws):
    c = FakeClient([
        reply(tool_use("read_file", {"path": "../../etc/passwd", "start_line": 1, "end_line": None})),
        reply(tool_use("submit_diagnosis", DIAG)),
    ])
    res = agent.run(c, ws, "go")
    tr = c.calls[1]["messages"][-1]["content"][0]
    assert tr["is_error"] is True and "outside the allowed directory" in tr["content"]
    assert res.stop == "submitted"


def test_refusal_and_no_tool_call(ws):
    assert agent.run(FakeClient([reply(stop="refusal")]), ws, "go").stop == "refusal"
    r = agent.run(FakeClient([reply(NS(type="text", text="done"), stop="end_turn")]), ws, "go")
    assert r.stop == "no_tool_call"


def test_tools(ws):
    assert "    2  int y = 3;" in dispatch(ws, "read_file", {"path": "src/a.cpp", "start_line": 2,
                                                         "end_line": None})
    assert "src/a.cpp:2:" in dispatch(ws, "grep_repo", {"pattern": "y =", "glob": "*.cpp"})
    assert "+int y = 3;" in dispatch(ws, "git_diff", {"path": None})
    with pytest.raises(ToolError):
        dispatch(ws, "read_ci_artifact", {"name": "../repo/src/a.cpp", "pattern": None, "offset": 0})


def test_markdown_report():
    res = agent.Result(diagnosis=DIAG, stop="submitted", iterations=3, input_tokens=10,
                       output_tokens=5)
    md = report.to_markdown(res)
    assert report.MARKER in md and "`src/proto/wire.cpp:12`" in md and "```diff" in md
    assert "without a diagnosis" in report.to_markdown(agent.Result(None, "max_iterations"))


def test_cli_skips_without_api_key(monkeypatch, tmp_path, capsys):
    from tools.ci_agent import __main__ as cli
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    monkeypatch.setattr(sys, "argv", ["ci_agent", "--artifacts", str(tmp_path)])
    assert cli.main() == 0
    assert "skipping" in capsys.readouterr().out
