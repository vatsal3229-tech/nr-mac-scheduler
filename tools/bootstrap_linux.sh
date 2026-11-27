#!/usr/bin/env bash
# One-time setup of a fresh Ubuntu 24.04 (native or WSL2) for every phase:
# compilers, Python venv, Docker Engine, kind, kubectl, kubeconform.
set -euo pipefail
cd "$(dirname "$0")/.."

sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build clang clang-format gdb git curl \
  ca-certificates python3 python3-venv

python3 -m venv .venv
.venv/bin/pip install --quiet -e ue_sim pytest pyyaml matplotlib -r tools/ci_agent/requirements.txt

if ! command -v docker >/dev/null; then
  curl -fsSL https://get.docker.com | sudo sh
  sudo usermod -aG docker "$USER"
  echo "NOTE: log out and back in (or run 'newgrp docker') so docker works without sudo."
fi

ARCH=amd64
if ! command -v kind >/dev/null; then
  curl -fsSLo /tmp/kind "https://kind.sigs.k8s.io/dl/v0.24.0/kind-linux-${ARCH}"
  sudo install /tmp/kind /usr/local/bin/kind
fi
if ! command -v kubectl >/dev/null; then
  curl -fsSLo /tmp/kubectl "https://dl.k8s.io/release/$(curl -fsSL https://dl.k8s.io/release/stable.txt)/bin/linux/${ARCH}/kubectl"
  sudo install /tmp/kubectl /usr/local/bin/kubectl
fi
if ! command -v kubeconform >/dev/null; then
  curl -fsSL "https://github.com/yannh/kubeconform/releases/download/v0.6.7/kubeconform-linux-${ARCH}.tar.gz" \
    | sudo tar -xz -C /usr/local/bin kubeconform
fi
echo "bootstrap done. Next: . .venv/bin/activate && make test"
