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

Covered in Phase 2.

## 6. Known limitations of this implementation

* Throughput, fairness and utilization on `/metrics` are cumulative since start, not windowed.
  Use PromQL `rate()` on the `_total` counters for windowed rates.
* p50/p99 come from a rolling window of the last 4096 slots. The histogram covers all slots.
* On a non-RT kernel (and especially under WSL2 or in CI VMs) some slot overruns
  are expected. They are counted, not hidden.
