#include "phy/cqi_table.h"

#include <cmath>

namespace nrs {
namespace {

// Index = CQI. Entry 0 is "out of range" in the spec: the UE cannot decode anything.
constexpr std::array<double, kMaxCqi + 1> kEfficiency = {
    0.0,     // 0  out of range
    0.1523,  // 1  QPSK  78/1024
    0.2344,  // 2  QPSK  120
    0.3770,  // 3  QPSK  193
    0.6016,  // 4  QPSK  308
    0.8770,  // 5  QPSK  449
    1.1758,  // 6  QPSK  602
    1.4766,  // 7  16QAM 378
    1.9141,  // 8  16QAM 490
    2.4063,  // 9  16QAM 616
    2.7305,  // 10 64QAM 466
    3.3223,  // 11 64QAM 567
    3.9023,  // 12 64QAM 666
    4.5234,  // 13 64QAM 772
    5.1152,  // 14 64QAM 873
    5.5547,  // 15 64QAM 948
};

}  // namespace

double cqi_to_efficiency(uint8_t cqi) {
  if (cqi < kMinCqi || cqi > kMaxCqi) return 0.0;
  return kEfficiency[cqi];
}

uint32_t bits_per_prb(uint8_t cqi, double overhead) {
  const double re_per_prb = kSubcarriersPerPrb * kSymbolsPerSlot * (1.0 - overhead);
  // Floor: a transport block cannot carry a fractional bit.
  return static_cast<uint32_t>(std::floor(cqi_to_efficiency(cqi) * re_per_prb));
}

LinkModel::LinkModel(double overhead) {
  for (uint8_t cqi = 0; cqi <= kMaxCqi; ++cqi) bits_[cqi] = nrs::bits_per_prb(cqi, overhead);
}

uint32_t LinkModel::bits_per_prb(uint8_t cqi) const { return cqi <= kMaxCqi ? bits_[cqi] : 0; }

uint32_t LinkModel::prbs_for_bytes(uint8_t cqi, uint64_t bytes) const {
  const uint64_t bits = bits_per_prb(cqi);
  if (bits == 0 || bytes == 0) return 0;
  const uint64_t need = (bytes * 8 + bits - 1) / bits;
  // Saturate: no cell has more than 275 PRBs, so this cap never binds in practice.
  return need > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(need);
}

uint64_t LinkModel::bytes_for_prbs(uint8_t cqi, uint32_t prbs) const {
  return static_cast<uint64_t>(bits_per_prb(cqi)) * prbs / 8;
}

}  // namespace nrs
