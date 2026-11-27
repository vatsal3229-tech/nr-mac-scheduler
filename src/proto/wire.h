#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/types.h"

// Binary UE report format. docs/interface.md is the source of truth;
// tools/arch_check.py verifies these constants match it and the Python encoder.
namespace nrs::wire {

inline constexpr uint16_t kMagic = 0x4E52;  // "NR"
inline constexpr uint8_t kVersion = 1;
inline constexpr uint8_t kMsgUeReport = 1;
inline constexpr size_t kHeaderSize = 8;
inline constexpr size_t kRecordSize = 8;

enum class ParseError : uint8_t {
  kOk = 0,
  kTruncatedHeader,
  kBadMagic,
  kBadVersion,
  kUnknownType,
  kLengthMismatch,  // header payload_len disagrees with datagram size
  kBadPayloadSize,  // empty, or not a whole number of records
  kBadCqi,
  kCount,  // keep last: sizes per-reason counters
};
inline constexpr size_t kParseErrorCount = static_cast<size_t>(ParseError::kCount);

std::string_view to_string(ParseError e);

// Parses one datagram. On success appends every record to `out`; on any error
// appends nothing, so a half-valid packet can never partially update state.
ParseError parse_datagram(const uint8_t* data, size_t len, std::vector<UeReport>& out);

std::vector<uint8_t> encode_ue_reports(const std::vector<UeReport>& reports);

}  // namespace nrs::wire
