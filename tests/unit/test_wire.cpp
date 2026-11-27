#include <gtest/gtest.h>

#include "proto/wire.h"

namespace nrs::wire {
namespace {

std::vector<uint8_t> valid() { return encode_ue_reports({{0x0102, 9, 0x0A0B0C0D}, {7, 15, 1}}); }

ParseError parse(const std::vector<uint8_t>& b, std::vector<UeReport>& out) {
  return parse_datagram(b.data(), b.size(), out);
}

TEST(Wire, ExactBytesAreBigEndian) {
  const auto b = encode_ue_reports({{0x0102, 9, 0x0A0B0C0D}});
  const std::vector<uint8_t> expected = {
      0x4E, 0x52, 0x01, 0x01, 0x00, 0x08, 0x00, 0x00,  // header
      0x01, 0x02, 0x09, 0x00, 0x0A, 0x0B, 0x0C, 0x0D,  // record
  };
  EXPECT_EQ(b, expected);
}

TEST(Wire, RoundTrip) {
  std::vector<UeReport> out;
  ASSERT_EQ(parse(valid(), out), ParseError::kOk);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].ue_id, 0x0102);
  EXPECT_EQ(out[0].cqi, 9);
  EXPECT_EQ(out[0].new_bytes, 0x0A0B0C0Du);
  EXPECT_EQ(out[1].ue_id, 7);
  EXPECT_EQ(out[1].cqi, 15);
  EXPECT_EQ(out[1].new_bytes, 1u);
}

TEST(Wire, ReservedHeaderBytesIgnored) {
  auto b = valid();
  b[6] = 0xFF;
  b[7] = 0xFF;
  std::vector<UeReport> out;
  EXPECT_EQ(parse(b, out), ParseError::kOk);
}

TEST(Wire, RejectsBadHeaderFields) {
  std::vector<UeReport> out;
  auto b = valid();
  b[0] = 0x52;  // byte-swapped magic, a classic endianness bug
  b[1] = 0x4E;
  EXPECT_EQ(parse(b, out), ParseError::kBadMagic);
  b = valid();
  b[2] = 2;
  EXPECT_EQ(parse(b, out), ParseError::kBadVersion);
  b = valid();
  b[3] = 9;
  EXPECT_EQ(parse(b, out), ParseError::kUnknownType);
  EXPECT_TRUE(out.empty());
}

TEST(Wire, RejectsEveryTruncation) {
  const auto b = valid();
  for (size_t len = 0; len < b.size(); ++len) {
    std::vector<UeReport> out;
    const ParseError e = parse_datagram(b.data(), len, out);
    EXPECT_NE(e, ParseError::kOk) << "len " << len;
    EXPECT_EQ(e, len < kHeaderSize ? ParseError::kTruncatedHeader : ParseError::kLengthMismatch)
        << "len " << len;
    EXPECT_TRUE(out.empty());
  }
}

TEST(Wire, RejectsTrailingBytes) {
  auto b = valid();
  b.push_back(0);
  std::vector<UeReport> out;
  EXPECT_EQ(parse(b, out), ParseError::kLengthMismatch);
}

TEST(Wire, RejectsEmptyAndPartialRecordPayload) {
  std::vector<UeReport> out;
  auto empty = encode_ue_reports({});
  EXPECT_EQ(parse(empty, out), ParseError::kBadPayloadSize);

  // Header claims 5 payload bytes and 5 are present: consistent length, but not a whole record.
  std::vector<uint8_t> partial = {0x4E, 0x52, 1, 1, 0, 5, 0, 0, 1, 2, 3, 4, 5};
  EXPECT_EQ(parse(partial, out), ParseError::kBadPayloadSize);
}

TEST(Wire, BadCqiRejectsWholeDatagram) {
  auto b = encode_ue_reports({{1, 10, 5}, {2, 16, 5}});
  std::vector<UeReport> out;
  EXPECT_EQ(parse(b, out), ParseError::kBadCqi);
  EXPECT_TRUE(out.empty()) << "a bad record must not let earlier records through";
  b = encode_ue_reports({{1, 0, 5}});
  EXPECT_EQ(parse(b, out), ParseError::kBadCqi);
}

TEST(Wire, NullPointerIsTruncated) {
  std::vector<UeReport> out;
  EXPECT_EQ(parse_datagram(nullptr, 0, out), ParseError::kTruncatedHeader);
}

TEST(Wire, ErrorNamesAreDistinct) {
  for (size_t i = 0; i < kParseErrorCount; ++i) {
    for (size_t j = i + 1; j < kParseErrorCount; ++j) {
      EXPECT_NE(to_string(static_cast<ParseError>(i)), to_string(static_cast<ParseError>(j)));
    }
  }
}

}  // namespace
}  // namespace nrs::wire
