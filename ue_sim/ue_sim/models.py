"""CQI (channel quality) and traffic models.

Every model owns its own random.Random, seeded from the scenario seed and the
UE id. Stdlib random is used rather than numpy because its Mersenne Twister
sequence for a given seed is stable across Python versions and platforms,
which is what makes traces reproducible.
"""

from __future__ import annotations

import math
import random

MIN_CQI, MAX_CQI = 1, 15


def clamp_cqi(v: float) -> int:
    return max(MIN_CQI, min(MAX_CQI, int(round(v))))


# ---------------------------------------------------------------- CQI models
# Each model is called once per slot, in slot order: cqi(slot) -> 1..15.


class StaticCqi:
    """Fixed channel, e.g. a stationary UE."""

    def __init__(self, cqi: int):
        self.value = clamp_cqi(cqi)

    def cqi(self, slot: int) -> int:
        return self.value


class RandomWalkCqi:
    """CQI moves +-1 with probability `step_prob` each slot: slow shadowing/mobility."""

    def __init__(self, start: int, step_prob: float, rng: random.Random, lo: int = 1, hi: int = 15):
        self.value = clamp_cqi(start)
        self.step_prob = step_prob
        self.rng = rng
        self.lo, self.hi = lo, hi

    def cqi(self, slot: int) -> int:
        if self.rng.random() < self.step_prob:
            self.value += 1 if self.rng.random() < 0.5 else -1
            self.value = max(self.lo, min(self.hi, self.value))
        return self.value


class FadingCqi:
    """Fading-like sinusoid around a mean, plus +-1 measurement noise.

    Real Rayleigh fading is not sinusoidal, but a sinusoid with a random
    phase per UE gives what matters for scheduling: each UE has peaks and
    troughs that are not aligned with other UEs.
    """

    def __init__(self, mean: float, amplitude: float, period_slots: float, rng: random.Random):
        self.mean = mean
        self.amplitude = amplitude
        self.period = period_slots
        self.phase = rng.uniform(0, 2 * math.pi)
        self.rng = rng

    def cqi(self, slot: int) -> int:
        noise = self.rng.choice((-1, 0, 1))
        return clamp_cqi(
            self.mean + self.amplitude * math.sin(2 * math.pi * slot / self.period + self.phase) + noise
        )


# ------------------------------------------------------------ traffic models
# Each model is called once per slot: new_bytes(slot) -> bytes arriving in the DL buffer.


class FullBuffer:
    """Always more data than the cell can serve (bulk download)."""

    def __init__(self, bytes_per_slot: int = 10_000):
        self.bytes_per_slot = bytes_per_slot

    def new_bytes(self, slot: int) -> int:
        return self.bytes_per_slot


class PoissonBursty:
    """Bursts arrive as a Poisson process; burst sizes are exponential (web-like)."""

    def __init__(self, bursts_per_s: float, mean_burst_bytes: float, rng: random.Random,
                 slot_s: float = 0.001):
        self.lam = bursts_per_s * slot_s  # expected arrivals per slot
        self.mean = mean_burst_bytes
        self.rng = rng

    def _poisson(self) -> int:
        # Knuth's method: fine for the small per-slot rates used here.
        limit, k, p = math.exp(-self.lam), 0, 1.0
        while True:
            p *= self.rng.random()
            if p <= limit:
                return k
            k += 1

    def new_bytes(self, slot: int) -> int:
        n = self._poisson()
        return sum(max(1, int(self.rng.expovariate(1.0 / self.mean))) for _ in range(n))


class Periodic:
    """Small fixed-size packets every `period_slots` (VoIP-like: 20 ms, ~60 bytes)."""

    def __init__(self, period_slots: int, size_bytes: int, offset: int = 0):
        self.period = period_slots
        self.size = size_bytes
        self.offset = offset % period_slots

    def new_bytes(self, slot: int) -> int:
        return self.size if slot % self.period == self.offset else 0
