# Design

## 1. RAN concepts, in plain language

**gNB, CU, DU.** A 5G base station (gNB) is split into a Central Unit and one or
more Distributed Units. The DU runs the time-critical layers: RLC, MAC and
upper PHY. The **MAC scheduler** lives in the DU. Every slot it decides which
UEs (phones) get radio resources, and how many.

**Slot.** The basic time unit of NR scheduling. With 15 kHz subcarrier spacing
(numerology 0) a slot is **1 ms** long and has **14 OFDM symbols**. The
scheduler must decide before the slot starts. Missing that deadline means
the slot goes out empty or late, which this project calls an *overrun*.

**PRB (Physical Resource Block).** The unit of frequency the scheduler hands
out: 12 subcarriers × one slot. A 10 MHz carrier at 15 kHz has **52 PRBs**.
Each slot the scheduler splits those 52 PRBs among UEs.

**CQI (Channel Quality Indicator).** A number 1-15 the UE reports, saying how
good its radio channel is right now. A high CQI means the gNB can use a dense
modulation (64QAM) and a high code rate, so each PRB carries more bits. A low
CQI means QPSK and heavy coding. The mapping from CQI to bits per resource
element is TS 38.214 Table 5.2.2.1-2 (CQI Table 1), in `src/phy/cqi_table.cpp`.

**Buffer status.** How many bytes are waiting in the gNB for each UE (the RLC
queue). There is no point giving PRBs to a UE with nothing to send.

**Proportional-fair (PF) scheduling.** Each slot, rank UEs by

    priority = (rate the UE could get this slot) / (rate it has averaged recently)

and serve the highest first. A UE whose channel is temporarily *better than
its own usual* has a high ratio, so PF tends to serve each UE near its fading
peaks. This beats round robin on throughput (multi-user diversity gain). Because
the average grows whenever a UE is served, no UE keeps winning forever, so PF
is much fairer than max-throughput. The average is an EWMA:
`avg ← (1 − 1/Tc)·avg + (1/Tc)·served_rate`, with `Tc = 100` slots by default.

**Round robin (RR).** Equal PRB share for every UE with data, regardless of
channel. Fair in resources but wasteful, because it serves UEs in their fading troughs.

**Max-throughput (MaxT).** Always serve the best CQI first. This gives the highest
cell throughput but starves cell-edge UEs. It is here as a baseline.

**Jain's fairness index.** `(Σx)² / (n·Σx²)` over per-UE throughputs. 1.0 means
everyone got the same throughput, and 1/n means one UE got everything.

## 2. Architecture

```
            UDP :38472                           HTTP :8080
UE sim ───────────────► [receiver thread]        [http thread] ◄── Prometheus / kubelet probes
                        epoll + eventfd               │ reads
                        parse_datagram()              ▼
                              │ try_push       MetricsHub (mutex snapshot + atomics)
                              ▼                       ▲ publish every 10 slots
                     ConcurrentQueue<UeReport>        │
                              │ drain (once/slot)     │
                              ▼                       │
                        [slot loop thread] ── Cell ── Scheduler (RR | PF | MaxT)
                        sleep_until(deadline)  │
                                               └── KpiCollector
main thread: sigwait(SIGTERM|SIGINT) → ordered shutdown
```

Offline mode runs exactly the same `Cell` + `Scheduler` + `KpiCollector` with no
threads, sockets or sleeping (`src/offline/replay.cpp`).

## 3. Design decisions and why

| Decision | Why |
|---|---|
| **Strategy pattern**: `Scheduler::schedule(const SlotInput&) → vector<Allocation>` | Policies are swappable by config, and each is a near-pure function that can be unit-tested with hand-built UE lists. |
| **Schedulers propose, `Cell` disposes** | Buffer draining, EWMA update and invariant checks live in one place. A buggy policy cannot over-allocate the carrier or send bytes that don't exist; such allocations are dropped and counted in `nrs_invariant_violations_total`. This is defence in depth, and it makes bugs visible in metrics. |
| **`SlotInput` is const** | Policies cannot mutate UE state; the only output is the allocation list. |
| **EWMA updated for every UE every slot**, served or not | A starved UE's average decays, so its PF priority rises. Updating only served UEs is a classic PF bug that causes permanent starvation. |
| **PF with one wideband CQI gives a UE all the PRBs it can use** | With no per-subband CQI every PRB has the same rate for a given UE, so PF priority is the same on every PRB and the greedy fill is optimal. Leftover PRBs (small buffer) go to the next UE. |
| **RR = equal *PRB* share with water-filling and a rotating remainder** | That is what "round robin" means for resource fairness. PRBs a UE can't use go back to the others, and the rotating pointer makes leftover PRBs fair over time. Tested: 5 UEs × 5 slots → exactly 52 PRBs each. |
| **Deterministic tie-breaking by UE id; UEs stored sorted by id** | Offline replay must be bit-for-bit reproducible for the CI performance gate. |
| **Precomputed `LinkModel` table** (bits per PRB per CQI) | The hot path is a table lookup, not floating-point math per UE per slot. |
| **Receiver: epoll + eventfd** | epoll waits on the socket without busy-polling. The eventfd lets `stop()` wake the thread instantly, so there is no shutdown latency from poll timeouts. |
| **Bounded mutex queue with `try_push`** | The receiver must never block (it would stop draining the socket and the kernel would drop datagrams). A bound keeps memory flat if the slot loop stalls, and drops are counted. A mutex taken once per slot by the consumer has negligible contention, and TSan can verify it, unlike a hand-rolled lock-free ring. |
| **Slot pacing with `sleep_until(absolute deadline)`** | A relative `sleep_for(1ms)` accumulates drift (work time + oversleep). If the loop falls more than one slot behind it resynchronises instead of bursting through stale slots. |
| **Signals: block in all threads, `sigwait` in main** | Shutdown logic runs in normal code, not in an async-signal handler, where almost nothing is safe to call. Order: mark not-ready → stop receiver → stop slot loop → stop HTTP. |
| **Liveness ≠ readiness** | `/healthz` fails only if the slot loop stops progressing (restart the pod). `/readyz` fails while starting or draining (stop sending traffic). Mixing them up causes restart loops. |
| **Metrics snapshot every 10 slots** under a mutex | A scrape sees one consistent snapshot. Copying per-UE state every slot would put needless work in the hot path; 10 ms is far finer than any scrape interval. |
| **Strict config: unknown keys fail startup** | A ConfigMap typo (`num_prb:`) silently falling back to a default is the kind of bug that survives to production. |
| **Bounded UE table and per-UE buffer** | A flood of spoofed UE ids or bytes cannot grow memory without limit. Overflow is counted, like an RLC tail-drop. |
| **Explicit-shift big-endian parsing** | Independent of host endianness and alignment. Byte-swapped magic is a dedicated test case. |
| **All-or-nothing datagram parsing** | A corrupted datagram never half-applies. |
| **Python simulator uses stdlib `random` with string seeds per (UE, purpose)** | Reproducible across platforms and Python versions; changing one UE's model doesn't shift others' random streams. |
| **Trace in CSV, not binary** | Easy to read, diff and generate. Replay speed is dominated by scheduling, not parsing. |
| **Scheduling time excluded from the CI gate** | It depends on the machine. Throughput and fairness are deterministic functions of code + trace, so any change in them is real. |

## 4. Simplifications compared to real 5G NR

| This project | Real gNB |
|---|---|
| Downlink only | DL and UL; UL is scheduled via grants based on BSR/SR |
| Single layer, no MIMO | Up to 4-8 layers DL; rank indicator (RI) and PMI reported with CQI |
| One wideband CQI per UE | Subband CQI, so frequency-selective scheduling is possible |
| CQI → efficiency directly (Table 1) | CQI → MCS selection with link adaptation (OLLA) correcting for BLER |
| Fixed overhead factor (0.14) | Exact count of DMRS, PDCCH CORESET, CSI-RS, SSB, PTRS resource elements per slot |
| Transport block = floor(bits) | TBS determination per TS 38.214 §5.1.3.2 (quantisation, CRC, LDPC segmentation) |
| No HARQ, no retransmissions, 0% BLER | HARQ with up to 16 processes, retransmissions prioritised, K1 timing |
| Every allocation is error-free | Decoding failures happen at ~10% target BLER |
| No PDCCH capacity limit | Number of UEs per slot limited by control-channel (CCE) capacity |
| Allocation unit = PRB count | Allocation = PRB positions (type 0 RBG bitmap / type 1 contiguous), BWPs |
| One numerology (15 kHz, 1 ms slots) | Numerologies 0-4 (up to 0.125 ms slots), mini-slots |
| No QoS / 5QI, no logical channels | Per-logical-channel priority, GBR flows, PDB-aware scheduling |
| No DRX, no measurement gaps | UEs unavailable during DRX off / gaps |
| Reports arrive over UDP from a simulator | CSI arrives on PUCCH/PUSCH via the PHY (FAPI interface), BSR via MAC CE |
| Soft real time on a general-purpose Linux kernel | RT kernel, CPU isolation, core pinning, often DPDK |

## 5. Kubernetes: why a StatefulSet per cell

A scheduler instance **is** a cell. It holds per-UE state (CQI, buffer, PF
average) for the UEs attached to that cell, and only those. That has three consequences:

1. **Requests must reach a specific instance.** A normal ClusterIP Service
   load-balances each UDP flow to *some* pod. A UE whose reports landed on
   cell-1 one moment and cell-0 the next would have its buffer split across two
   schedulers that each see half the data, and its PF average would be meaningless.
   Real RAN has the same rule: a UE is attached to one cell at a time, and moving
   it is an explicit handover, not a load-balancing decision.
2. **Instances need stable identities.** A StatefulSet gives pods fixed names
   (`cell-0`, `cell-1`, ...) that survive restarts. A **headless Service**
   (`clusterIP: None`) gives each one a DNS record,
   `cell-N.cell.nrs.svc.cluster.local`, with no virtual IP in between. Each
   simulator Job targets one of these names, like a UE camped on one cell.
3. **Scaling is adding cells, not adding replicas of a cell.** `replicas: 3`
   means a third cell with its own UEs. It does not mean more capacity for existing UEs.
   A Deployment + ClusterIP (the default for stateless web services) would be
   wrong on all three counts.

No PersistentVolume is used: scheduler state is soft state that UEs rebuild
within a few report periods after a restart (fresh CQI and buffer reports), so
a StatefulSet is used for identity, not storage.

**Hardening.** The namespace enforces the Pod Security Standard `restricted`.
Pods run as non-root (distroless `nonroot`, uid 65532), with a read-only root
filesystem, all capabilities dropped, no privilege escalation, RuntimeDefault
seccomp and no service-account token. The scheduler needs none of these
privileges, and `tools/arch_check.py` fails CI if any manifest loses them.

**Probes.** Liveness is `/healthz`: has the slot loop made progress within 2 s?
If not, the kubelet restarts the pod. Readiness is `/readyz`: are the sockets bound
and has the first slot run? Until then the pod's DNS record is not published.
The simulator retries DNS for this reason.

**Resources.** CPU requests equal limits (500m) so the slot loop is not
throttled by CFS in bursts. Memory is bounded by design (bounded queue, UE
table and buffers), so a 128 Mi limit holds with a wide margin. In production
the DU would be in the Guaranteed QoS class with whole-CPU pinning (CPU Manager
`static` policy) and an RT kernel. That is out of scope for kind.

**CI pipeline.** Every push runs five independent jobs:
- unit + live tests with a clang-format check
- ASan/UBSan and TSan builds of the unit tests
- the offline performance gate against `bench/baseline.json`, with a 2% tolerance on throughput and fairness
- `arch_check.py`: manifest policy, ConfigMap keys, kubeconform schema, wire constants
- kind end-to-end, which proves each cell only sees its own UEs

On failure, each job uploads its log, and e2e also uploads pod logs,
`kubectl describe` output and events. The Phase 3 agent reads those.

## 6. CI failure agent (Phase 3)

`tools/ci_agent/` is a ReAct loop on the Claude Messages API (`claude-opus-5-5`).
In each turn the model reasons, calls one or more tools, and gets the results back.
It finishes by calling `submit_diagnosis`, a tool with a strict JSON schema
(file, line, category, evidence, patch, confidence). That way the output is
structured data, not prose that has to be parsed.

| Decision | Why |
|---|---|
| Hand-written loop, not the SDK tool runner | The limits are the point: every turn counts against `max_iterations` (15) and a total token budget (400k). The loop decides when to stop. On the last turn it tells the model to submit. |
| Only read-only tools (`list_artifacts`, `read_ci_artifact`, `git_diff`, `read_file`, `grep_repo`) confined to the repo and artifact dirs | Logs and diffs come from the PR under test, so they are untrusted. A prompt-injected model can at worst read files it was already allowed to read. The system prompt also tells it never to follow instructions in data. |
| Tool output capped at 12k chars, with paging (`offset`) and regex filtering (`pattern`) | CI logs are long and repetitive. Filtering for `FAILED|error|OOMKilled` first keeps context small, and paging means nothing is unreachable. |
| `git_diff` against the merge base | Shows exactly the change under test, which is where a regression almost always lives. |
| `strict: true` schemas on every tool | Inputs are guaranteed to match the schema, so the dispatcher needs no defensive parsing. |
| Adaptive thinking, effort `high`, server-side refusal fallback | Diagnosis is reasoning-heavy, and a refusal is retried on a fallback model rather than failing the job. |
| Skips with exit 0 when `ANTHROPIC_API_KEY` is absent | Forked PRs have no secrets, and the analysis is advisory, so it must never turn a run red by itself. |
| PR comment, not auto-commit | A suggested patch is a hypothesis. A human applies it. |

**Evaluation.** `bugs/` holds 12 patches, one injected bug each, with ground truth in
`bugs/bugs.yaml`. `python -m tools.ci_agent.eval` applies each patch in a fresh
worktree, runs the stage that should catch it (unit, TSan, arch check, perf gate
or e2e), checks that the stage fails, then runs the agent on the captured logs. A
diagnosis is correct only with the right file **and** the right cause. Results
go in `docs/agent_eval.md`. The first half of this, "does the pipeline catch the
bug?", is itself a test of the CI design.

## 7. Known limitations of this implementation

* Throughput, fairness and utilization on `/metrics` are cumulative since start, not windowed.
  Use PromQL `rate()` on the `_total` counters for windowed rates.
* p50/p99 come from a rolling window of the last 4096 slots. The histogram covers all slots.
* On a non-RT kernel (and especially under WSL2 or in CI VMs) some slot overruns
  are expected. They are counted, not hidden.
