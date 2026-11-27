#!/usr/bin/env bash
# Dump everything needed to debug a failed e2e run without cluster access:
# pod logs (current + previous container), describe output and events.
# These files are also the inputs the CI agent reads (tools/ci_agent).
set -uo pipefail
OUT=${1:-artifacts/k8s}
NS=${NS:-nrs}
mkdir -p "$OUT"

kubectl get all -n "$NS" -o wide >"$OUT/get-all.txt" 2>&1
kubectl get events -n "$NS" --sort-by=.lastTimestamp >"$OUT/events.txt" 2>&1
kubectl describe pods -n "$NS" >"$OUT/describe-pods.txt" 2>&1
kubectl describe statefulset,jobs -n "$NS" >"$OUT/describe-workloads.txt" 2>&1
kubectl get configmap -n "$NS" -o yaml >"$OUT/configmaps.yaml" 2>&1

for pod in $(kubectl get pods -n "$NS" -o name 2>/dev/null); do
  name=${pod#pod/}
  kubectl logs -n "$NS" "$name" --all-containers >"$OUT/logs-$name.txt" 2>&1
  kubectl logs -n "$NS" "$name" --all-containers --previous >"$OUT/logs-$name-previous.txt" 2>/dev/null \
    || rm -f "$OUT/logs-$name-previous.txt"
done
echo "collected k8s artifacts in $OUT"
