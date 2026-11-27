#include "proto/wire.h"

#include "phy/cqi_table.h"

namespace nrs::wire {
namespace {

// Explicit shifts instead of ntohs/memcpy-casts: independent of host byte
// order and alignment, and obviously correct when read.
uint16_t read_u16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t read_u32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
void put_u16(std::vector<uint8_t>& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v & 0xFF));
}
void put_u32(std::vector<uint8_t>& b, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) b.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
}

}  // namespace

std::string_view to_string(ParseError e) {
  switch (e) {
    case ParseError::kOk:
      return "ok";
    case ParseError::kTruncatedHeader:
      return "truncated_header";
    case ParseError::kBadMagic:
      return "bad_magic";
    case ParseError::kBadVersion:
      return "bad_version";
    case ParseError::kUnknownType:
      return "unknown_type";
    case ParseError::kLengthMismatch:
      return "length_mismatch";
    case ParseError::kBadPayloadSize:
      return "bad_payload_size";
    case ParseError::kBadCqi:
      return "bad_cqi";
    case ParseError::kCount:
      break;
  }
  return "unknown";
}

ParseError parse_datagram(const uint8_t* data, size_t len, std::vector<UeReport>& out) {
  if (data == nullptr || len < kHeaderSize) return ParseError::kTruncatedHeader;
  if (read_u16(data) != kMagic) return ParseError::kBadMagic;
  if (data[2] != kVersion) return ParseError::kBadVersion;
  if (data[3] != kMsgUeReport) return ParseError::kUnknownType;
  const size_t payload_len = read_u16(data + 4);
  // bytes 6-7 are reserved: ignored on receive so v1 receivers tolerate future use.
  if (payload_len != len - kHeaderSize) return ParseError::kLengthMismatch;
  if (payload_len == 0 || payload_len % kRecordSize != 0) return ParseError::kBadPayloadSize;

  const uint8_t* p = data + kHeaderSize;
  const size_t n = payload_len / kRecordSize;
  // Validate everything before touching `out` so errors are all-or-nothing.
  for (size_t i = 0; i < n; ++i) {
    const uint8_t cqi = p[i * kRecordSize + 2];
    if (cqi < kMinCqi || cqi > kMaxCqi) return ParseError::kBadCqi;
  }
  out.reserve(out.size() + n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* r = p + i * kRecordSize;
    out.push_back({read_u16(r), r[2], read_u32(r + 4)});
  }
  return ParseError::kOk;
}

std::vector<uint8_t> encode_ue_reports(const std::vector<UeReport>& reports) {
  std::vector<uint8_t> b;
  b.reserve(kHeaderSize + reports.size() * kRecordSize);
  put_u16(b, kMagic);
  b.push_back(kVersion);
  b.push_back(kMsgUeReport);
  put_u16(b, static_cast<uint16_t>(reports.size() * kRecordSize));
  put_u16(b, 0);
  for (const UeReport& r : reports) {
    put_u16(b, r.ue_id);
    b.push_back(r.cqi);
    b.push_back(0);
    put_u32(b, r.new_bytes);
  }
  return b;
}

}  // namespace nrs::wire
