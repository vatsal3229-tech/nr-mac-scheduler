#!/usr/bin/env python3
"""Architecture conformance checks, run in CI on every push.

1. Every workload in k8s/ has resource requests+limits, non-root and
   read-only-root settings; long-running containers have liveness and
   readiness probes, and every probe targets a port the container declares.
2. ConfigMap scheduler config only uses keys the scheduler understands
   (the same table as docs/interface.md section 5).
3. Manifests pass Kubernetes schema validation (kubeconform, if installed).
4. Wire-format constants agree between docs/interface.md, the C++ header and
   the Python encoder.

Exit code 0 = all checks passed. Each failure is printed as `file: message`.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKLOAD_KINDS = {"Deployment", "StatefulSet", "DaemonSet", "Job", "CronJob"}
LONG_RUNNING = {"Deployment", "StatefulSet", "DaemonSet"}
WIRE_KEYS = ("MAGIC", "VERSION", "MSG_UE_REPORT", "HEADER_SIZE", "RECORD_SIZE")


class Report:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.checks = 0

    def check(self, ok: bool, where: str, msg: str) -> None:
        self.checks += 1
        if not ok:
            self.errors.append(f"{where}: {msg}")


def pod_spec(doc: dict) -> dict:
    spec = doc.get("spec", {})
    if doc["kind"] == "CronJob":
        spec = spec.get("jobTemplate", {}).get("spec", {})
    return spec.get("template", {}).get("spec", {})


def declared_ports(c: dict) -> set:
    ports = set()
    for p in c.get("ports", []):
        ports.add(p.get("containerPort"))
        if "name" in p:
            ports.add(p["name"])
    return ports


def check_workload(r: Report, where: str, doc: dict) -> None:
    spec = pod_spec(doc)
    psc = spec.get("securityContext", {})
    name = f"{where} {doc['kind']}/{doc['metadata']['name']}"
    for c in spec.get("containers", []):
        cw = f"{name} container {c.get('name')}"
        res = c.get("resources", {})
        for kind in ("requests", "limits"):
            for k in ("cpu", "memory"):
                r.check(k in res.get(kind, {}), cw, f"missing resources.{kind}.{k}")
        csc = c.get("securityContext", {})
        r.check(csc.get("runAsNonRoot", psc.get("runAsNonRoot")) is True, cw,
                "runAsNonRoot must be true (pod or container securityContext)")
        uid = csc.get("runAsUser", psc.get("runAsUser"))
        r.check(uid is None or uid != 0, cw, "runAsUser must not be 0")
        r.check(csc.get("readOnlyRootFilesystem") is True, cw, "readOnlyRootFilesystem must be true")
        r.check(csc.get("allowPrivilegeEscalation") is False, cw,
                "allowPrivilegeEscalation must be false")
        if doc["kind"] in LONG_RUNNING:
            ports = declared_ports(c)
            for probe in ("livenessProbe", "readinessProbe"):
                p = c.get(probe)
                r.check(p is not None, cw, f"missing {probe}")
                if not p:
                    continue
                target = (p.get("httpGet") or p.get("tcpSocket") or {}).get("port")
                if target is not None:
                    r.check(target in ports, cw,
                            f"{probe} targets port {target!r}, which the container does not declare "
                            f"(declared: {sorted(map(str, ports))})")


def interface_config_keys() -> set[str]:
    """Valid YAML keys = first column of the config table in docs/interface.md."""
    text = (ROOT / "docs" / "interface.md").read_text(encoding="utf-8")
    section = text.split("## 5. Configuration", 1)[1]
    keys = set()
    for m in re.finditer(r"^\| `([a-z_.]+)`(?: / `([a-z_]+)`)? \|", section, re.M):
        keys.add(m.group(1))
        if m.group(2):  # "network.udp_bind / udp_port" shorthand
            keys.add(m.group(1).rsplit(".", 1)[0] + "." + m.group(2))
    return keys


def flatten(d: dict, prefix: str = "") -> list[str]:
    out = []
    for k, v in d.items():
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            out += flatten(v, key + ".")
        else:
            out.append(key)
    return out


def check_configmap(r: Report, where: str, doc: dict, valid: set[str]) -> None:
    for fname, body in (doc.get("data") or {}).items():
        if not fname.endswith((".yaml", ".yml")):
            continue
        try:
            cfg = yaml.safe_load(body) or {}
        except yaml.YAMLError as e:
            r.check(False, where, f"ConfigMap {fname} is not valid YAML: {e}")
            continue
        for key in flatten(cfg):
            r.check(key in valid, where,
                    f"ConfigMap {doc['metadata']['name']}/{fname}: unknown scheduler key '{key}'")


def check_manifests(r: Report, k8s_dir: pathlib.Path) -> list[pathlib.Path]:
    files = sorted(k8s_dir.glob("*.yaml"))
    r.check(bool(files), str(k8s_dir), "no manifests found")
    valid_keys = interface_config_keys()
    for f in files:
        where = str(f.relative_to(ROOT))
        for doc in yaml.safe_load_all(f.read_text(encoding="utf-8")):
            if not doc:
                continue
            if doc.get("kind") in WORKLOAD_KINDS:
                check_workload(r, where, doc)
            elif doc.get("kind") == "ConfigMap":
                check_configmap(r, where, doc, valid_keys)
    return files


def check_schema(r: Report, files: list[pathlib.Path], require: bool) -> None:
    exe = shutil.which("kubeconform")
    if exe is None:
        r.check(not require, "kubeconform", "not installed (required with --require-schema)")
        if not require:
            print("note: kubeconform not installed, skipping schema validation", file=sys.stderr)
        return
    res = subprocess.run([exe, "-strict", "-summary", *map(str, files)], capture_output=True,
                         text=True)
    r.check(res.returncode == 0, "kubeconform", res.stdout.strip() + res.stderr.strip())


def parse_int(s: str) -> int:
    return int(s, 0)


def wire_constants() -> dict[str, dict[str, int]]:
    doc = (ROOT / "docs" / "interface.md").read_text(encoding="utf-8")
    m = re.search(r"<!-- arch_check: (.*?) -->", doc)
    from_doc = {k: parse_int(v) for k, v in (kv.split("=") for kv in m.group(1).split())} if m else {}

    hdr = (ROOT / "src" / "proto" / "wire.h").read_text(encoding="utf-8")
    cpp_names = {"MAGIC": "kMagic", "VERSION": "kVersion", "MSG_UE_REPORT": "kMsgUeReport",
                 "HEADER_SIZE": "kHeaderSize", "RECORD_SIZE": "kRecordSize"}
    from_cpp = {}
    for k, cname in cpp_names.items():
        mm = re.search(rf"\b{cname}\s*=\s*(0x[0-9A-Fa-f]+|\d+)", hdr)
        if mm:
            from_cpp[k] = parse_int(mm.group(1))

    py = (ROOT / "ue_sim" / "ue_sim" / "wire.py").read_text(encoding="utf-8")
    from_py = {}
    for k in WIRE_KEYS:
        mm = re.search(rf"^{k}\s*=\s*(0x[0-9A-Fa-f]+|\d+)", py, re.M)
        if mm:
            from_py[k] = parse_int(mm.group(1))
    return {"docs/interface.md": from_doc, "src/proto/wire.h": from_cpp,
            "ue_sim/ue_sim/wire.py": from_py}


def check_wire(r: Report) -> None:
    sources = wire_constants()
    ref = sources["docs/interface.md"]
    for k in WIRE_KEYS:
        r.check(k in ref, "docs/interface.md", f"arch_check marker missing {k}")
        for src, vals in sources.items():
            r.check(k in vals, src, f"constant {k} not found")
            if k in vals and k in ref:
                r.check(vals[k] == ref[k], src,
                        f"{k} = {vals[k]:#x} but docs/interface.md says {ref[k]:#x}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--k8s-dir", default=str(ROOT / "k8s"))
    ap.add_argument("--require-schema", action="store_true", help="fail if kubeconform is missing")
    a = ap.parse_args()

    r = Report()
    files = check_manifests(r, pathlib.Path(a.k8s_dir))
    check_schema(r, files, a.require_schema)
    check_wire(r)
    if r.errors:
        print(f"arch_check: {len(r.errors)} problem(s):")
        for e in r.errors:
            print(f"  FAIL {e}")
        return 1
    print(f"arch_check: all {r.checks} checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
