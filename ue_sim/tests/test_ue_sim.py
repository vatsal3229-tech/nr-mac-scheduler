import hashlib

import pytest

from ue_sim import cli, scenario, wire
from ue_sim.models import FadingCqi, Periodic, PoissonBursty, RandomWalkCqi, StaticCqi


def test_encoding_matches_cpp_test_vector():
    # Same bytes as tests/unit/test_wire.cpp ExactBytesAreBigEndian.
    got = wire.encode([wire.UeReport(0x0102, 9, 0x0A0B0C0D)])
    assert got == bytes([0x4E, 0x52, 0x01, 0x01, 0x00, 0x08, 0x00, 0x00,
                         0x01, 0x02, 0x09, 0x00, 0x0A, 0x0B, 0x0C, 0x0D])


def test_roundtrip_and_batching():
    reports = [wire.UeReport(i, 1 + i % 15, i * 100) for i in range(300)]
    dgrams = wire.encode_batches(reports)
    assert len(dgrams) == 3
    assert all(len(d) <= wire.HEADER_SIZE + wire.MAX_RECORDS_PER_DATAGRAM * wire.RECORD_SIZE
               for d in dgrams)
    assert [r for d in dgrams for r in wire.decode(d)] == reports


@pytest.mark.parametrize("model", [
    StaticCqi(20),
    RandomWalkCqi(8, 0.5, __import__("random").Random(1)),
    FadingCqi(8, 10, 50, __import__("random").Random(2)),
])
def test_cqi_models_stay_in_range(model):
    assert all(1 <= model.cqi(s) <= 15 for s in range(5000))


def test_traffic_models():
    import random
    p = Periodic(20, 60, offset=3)
    assert [p.new_bytes(s) for s in range(0, 40)].count(60) == 2
    assert p.new_bytes(3) == 60 and p.new_bytes(23) == 60
    b = PoissonBursty(bursts_per_s=20, mean_burst_bytes=6000, rng=random.Random(5))
    total = sum(b.new_bytes(s) for s in range(100_000))  # 100 s
    # Expected 20 bursts/s * 6000 B * 100 s = 12 MB; allow sampling noise.
    assert 10e6 < total < 14e6


def test_trace_is_reproducible(tmp_path):
    def digest(seed):
        path = tmp_path / f"t{seed}.csv"
        cli.write_trace(str(path), "mixed", 8, 2000, seed)
        return hashlib.sha256(path.read_bytes()).hexdigest()
    assert digest(3) == digest(3)
    assert digest(3) != digest(4)


def test_trace_format(tmp_path):
    path = tmp_path / "t.csv"
    rows = cli.write_trace(str(path), "static_full", 3, 5, 1)
    lines = path.read_text().splitlines()
    assert lines[0] == "slot,ue_id,cqi,new_bytes"
    assert len(lines) == rows + 1 == 16
    slots = [int(l.split(",")[0]) for l in lines[1:]]
    assert slots == sorted(slots)


def test_unknown_scenario():
    with pytest.raises(ValueError):
        scenario.build("nope", 1, 1)
