#pragma once

#include <array>
#include <cstdint>

namespace nrs {

inline constexpr uint8_t kMinCqi = 1;
inline constexpr uint8_t kMaxCqi = 15;
inline constexpr int kSubcarriersPerPrb = 12;
inline constexpr int kSymbolsPerSlot = 14;  // normal cyclic prefix

// Spectral efficiency (bits per resource element) from 3GPP TS 38.214
// Table 5.2.2.1-2 ("CQI Table 1"). Returns 0.0 for CQI outside 1..15 so a bad
// value can never translate into a schedulable rate.
double cqi_to_efficiency(uint8_t cqi);

// Bits one PRB carries in one slot:
//   efficiency * 12 subcarriers * 14 symbols * (1 - overhead)
// Overhead models DMRS/PDCCH/CSI-RS resource elements that carry no user data.
uint32_t bits_per_prb(uint8_t cqi, double overhead);

// Precomputed per-CQI capacity for one cell, so the per-slot hot path is a
// table lookup instead of floating-point math.
class LinkModel {
 public:
  explicit LinkModel(double overhead);

  uint32_t bits_per_prb(uint8_t cqi) const;
  // PRBs needed to drain `bytes` at this CQI (rounded up). 0 if the CQI cannot carry data.
  uint32_t prbs_for_bytes(uint8_t cqi, uint64_t bytes) const;
  // Whole bytes that fit in `prbs` PRBs at this CQI.
  uint64_t bytes_for_prbs(uint8_t cqi, uint32_t prbs) const;

 private:
  std::array<uint32_t, kMaxCqi + 1> bits_{};
};

}  // namespace nrs
