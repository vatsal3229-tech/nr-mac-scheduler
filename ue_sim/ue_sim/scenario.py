"""Named scenarios: a population of UEs, each with a CQI model and a traffic model."""

from __future__ import annotations

import random
from dataclasses import dataclass, field
from typing import Iterator

from ue_sim.models import (
    FadingCqi,
    FullBuffer,
    Periodic,
    PoissonBursty,
    RandomWalkCqi,
    StaticCqi,
)
from ue_sim.wire import UeReport


@dataclass
class SimUe:
    ue_id: int
    cqi_model: object
    traffic: object
    last_cqi: int | None = field(default=None)


def _rng(seed: int, ue_id: int, stream: str) -> random.Random:
    # Independent stream per (UE, purpose): adding a UE or changing one model
    # does not perturb the random sequence of the others.
    return random.Random(f"{seed}:{ue_id}:{stream}")


def _spread(i: int, n: int, lo: float = 3.0, hi: float = 13.0) -> float:
    """Mean CQIs spread evenly from cell edge (lo) to cell centre (hi)."""
    return lo + (hi - lo) * i / max(1, n - 1)


def build(name: str, num_ues: int, seed: int, first_ue_id: int = 0) -> list[SimUe]:
    ues = []
    for i in range(num_ues):
        uid = first_ue_id + i
        c = _rng(seed, uid, "cqi")
        t = _rng(seed, uid, "traffic")
        mean = _spread(i, num_ues)
        if name == "static_full":
            cqi, traffic = StaticCqi(round(mean)), FullBuffer()
        elif name == "fading_full":
            cqi = FadingCqi(mean, 3.0, c.uniform(40, 200), c)
            traffic = FullBuffer()
        elif name == "randomwalk_bursty":
            cqi = RandomWalkCqi(round(mean), 0.05, c)
            traffic = PoissonBursty(bursts_per_s=20, mean_burst_bytes=6000, rng=t)
        elif name == "mixed":
            kind = i % 4
            cqi = FadingCqi(mean, 3.0, c.uniform(40, 200), c)
            if kind in (0, 1):
                traffic = FullBuffer()
            elif kind == 2:
                traffic = PoissonBursty(bursts_per_s=20, mean_burst_bytes=6000, rng=t)
            else:
                traffic = Periodic(period_slots=20, size_bytes=60, offset=uid)
        else:
            raise ValueError(f"unknown scenario {name!r}; choose from {', '.join(SCENARIOS)}")
        ues.append(SimUe(uid, cqi, traffic))
    return ues


SCENARIOS = ("static_full", "fading_full", "randomwalk_bursty", "mixed")


def step(ues: list[SimUe], slot: int) -> Iterator[UeReport]:
    """Advance every UE one slot. A report is emitted when there is new data or
    the CQI changed, like a UE that only sends CSI/BSR when something changes."""
    for ue in ues:
        cqi = ue.cqi_model.cqi(slot)
        nb = ue.traffic.new_bytes(slot)
        if nb > 0 or cqi != ue.last_cqi:
            ue.last_cqi = cqi
            yield UeReport(ue.ue_id, cqi, nb)
