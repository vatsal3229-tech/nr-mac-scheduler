"""Prove arch_check catches each class of mistake it claims to catch."""

import pathlib
import shutil
import sys

import pytest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import arch_check  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[2]


@pytest.fixture
def repo(tmp_path, monkeypatch):
    for rel in ("k8s", "docs", "src/proto", "ue_sim/ue_sim"):
        shutil.copytree(REPO / rel, tmp_path / rel)
    monkeypatch.setattr(arch_check, "ROOT", tmp_path)
    return tmp_path


def run(repo):
    r = arch_check.Report()
    arch_check.check_manifests(r, repo / "k8s")
    arch_check.check_wire(r)
    return r.errors


def edit(path: pathlib.Path, old: str, new: str):
    text = path.read_text()
    assert old in text, f"{old!r} not in {path}"
    path.write_text(text.replace(old, new, 1))


def test_clean_repo_passes(repo):
    assert run(repo) == []


@pytest.mark.parametrize("old,new,expect", [
    ("              port: http\n            initialDelaySeconds",
     "              port: 9090\n            initialDelaySeconds", "livenessProbe targets port 9090"),
    ("              memory: 128Mi\n", "", "missing resources.limits.memory"),
    ("            readOnlyRootFilesystem: true\n", "", "readOnlyRootFilesystem"),
    ("          readinessProbe:", "          xreadinessProbe:", "missing readinessProbe"),
])
def test_statefulset_mistakes(repo, old, new, expect):
    edit(repo / "k8s" / "30-cell-statefulset.yaml", old, new)
    assert any(expect in e for e in run(repo)), run(repo)


def test_root_user_rejected(repo):
    edit(repo / "k8s" / "40-ue-sim-jobs.yaml", "runAsUser: 10001", "runAsUser: 0")
    assert any("runAsUser must not be 0" in e for e in run(repo))


def test_configmap_typo(repo):
    edit(repo / "k8s" / "10-configmap.yaml", "num_prbs:", "num_prb:")
    assert any("unknown scheduler key 'cell.num_prb'" in e for e in run(repo))


def test_wire_constant_drift(repo):
    edit(repo / "ue_sim" / "ue_sim" / "wire.py", "MAGIC = 0x4E52", "MAGIC = 0x524E")
    errs = run(repo)
    assert any("ue_sim/ue_sim/wire.py: MAGIC" in e for e in errs), errs
