# Developer entry points. CI calls the same targets so local == CI.
SHELL := /bin/bash
PRESET ?= release
BUILD := build/$(PRESET)
CLUSTER := nrs
PY ?= python3

.PHONY: all build test unit pytest asan tsan bench perf-gate format format-check arch-check \
        image kind-up deploy e2e kind-down collect-artifacts clean

all: build

build:
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET)

unit: build
	ctest --preset $(PRESET)

pytest: build
	$(PY) -m pytest -q ue_sim/tests
	NRS_BINARY=$(BUILD)/nr_mac_scheduler $(PY) -m pytest -q tests/integration

test: unit pytest

asan:
	$(MAKE) unit PRESET=asan-ubsan

tsan:
	$(MAKE) unit PRESET=tsan

bench: build
	$(PY) bench/run_bench.py --binary $(BUILD)/nr_mac_scheduler

perf-gate: build
	$(PY) bench/run_bench.py --binary $(BUILD)/nr_mac_scheduler --check

format:
	clang-format -i $$(git ls-files '*.cpp' '*.h')

format-check:
	clang-format --dry-run --Werror $$(git ls-files '*.cpp' '*.h')

arch-check:
	$(PY) tools/arch_check.py

image:
	docker build -t nr-mac-scheduler:dev .
	docker build -t ue-sim:dev ue_sim

kind-up:
	kind get clusters | grep -qx $(CLUSTER) || kind create cluster --config tools/kind-config.yaml --wait 120s

# Jobs are immutable, so delete them first to make deploy re-runnable.
deploy: image
	kind load docker-image nr-mac-scheduler:dev ue-sim:dev --name $(CLUSTER)
	kubectl apply -f k8s/00-namespace.yaml
	kubectl -n nrs delete job -l app=ue-sim --ignore-not-found
	kubectl apply -f k8s/
	kubectl -n nrs rollout restart statefulset/cell
	kubectl -n nrs rollout status statefulset/cell --timeout=180s

e2e:
	$(PY) -m pytest -q tests/e2e

collect-artifacts:
	tools/collect_k8s_artifacts.sh artifacts/k8s

kind-down:
	kind delete cluster --name $(CLUSTER)

clean:
	rm -rf build bench/out artifacts
