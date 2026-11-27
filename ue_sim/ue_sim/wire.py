"""Encoder for the UE report datagram. Must match docs/interface.md exactly.

tools/arch_check.py compares these constants with the C++ header and the doc.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = 0x4E52  # "NR"
VERSION = 1
MSG_UE_REPORT = 1
HEADER_SIZE = 8
RECORD_SIZE = 8

# Network byte order ("!"/">"): magic u16, version u8, type u8, payload_len u16, reserved u16
_HEADER = struct.Struct(">HBBHH")
# ue_id u16, cqi u8, reserved u8, new_bytes u32
_RECORD = struct.Struct(">HBBI")

# Largest record count whose datagram fits comfortably in one Ethernet MTU,
# so live mode never relies on IP fragmentation.
MAX_RECORDS_PER_DATAGRAM = 128

assert _HEADER.size == HEADER_SIZE and _RECORD.size == RECORD_SIZE


@dataclass(frozen=True)
class UeReport:
    ue_id: int
    cqi: int
    new_bytes: int


def encode(reports: list[UeReport]) -> bytes:
    payload = b"".join(_RECORD.pack(r.ue_id, r.cqi, 0, r.new_bytes) for r in reports)
    return _HEADER.pack(MAGIC, VERSION, MSG_UE_REPORT, len(payload), 0) + payload


def encode_batches(reports: list[UeReport]) -> list[bytes]:
    return [
        encode(reports[i : i + MAX_RECORDS_PER_DATAGRAM])
        for i in range(0, len(reports), MAX_RECORDS_PER_DATAGRAM)
    ]


def decode(data: bytes) -> list[UeReport]:
    """Reference decoder, used by tests to cross-check the encoder."""
    if len(data) < HEADER_SIZE:
        raise ValueError("truncated header")
    magic, version, mtype, plen, _ = _HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or mtype != MSG_UE_REPORT:
        raise ValueError("bad header")
    if plen != len(data) - HEADER_SIZE or plen == 0 or plen % RECORD_SIZE:
        raise ValueError("bad length")
    out = []
    for off in range(HEADER_SIZE, len(data), RECORD_SIZE):
        ue_id, cqi, _, nb = _RECORD.unpack_from(data, off)
        out.append(UeReport(ue_id, cqi, nb))
    return out
