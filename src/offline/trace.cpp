#include "offline/trace.h"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace nrs {
namespace {

template <typename T>
T field(std::string_view s, size_t line) {
  T v{};
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) {
    throw std::runtime_error("trace line " + std::to_string(line) + ": bad number '" +
                             std::string(s) + "'");
  }
  return v;
}

}  // namespace

std::vector<TraceRow> parse_trace(std::istream& in) {
  std::vector<TraceRow> rows;
  std::string line;
  size_t lineno = 0;
  if (!std::getline(in, line)) throw std::runtime_error("trace: empty file");
  ++lineno;
  if (!line.empty() && line.back() == '\r') line.pop_back();
  if (line != "slot,ue_id,cqi,new_bytes") {
    throw std::runtime_error("trace: expected header 'slot,ue_id,cqi,new_bytes'");
  }
  while (std::getline(in, line)) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    std::string_view rest(line);
    std::string_view cols[4];
    for (int i = 0; i < 4; ++i) {
      const size_t comma = rest.find(',');
      if ((i < 3) == (comma == std::string_view::npos)) {
        throw std::runtime_error("trace line " + std::to_string(lineno) + ": expected 4 columns");
      }
      cols[i] = rest.substr(0, comma);
      rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    }
    TraceRow r;
    r.slot = field<uint64_t>(cols[0], lineno);
    r.report.ue_id = field<uint16_t>(cols[1], lineno);
    // Out-of-range CQIs are kept (Cell rejects and counts them) but must not wrap around.
    const auto cqi = field<uint16_t>(cols[2], lineno);
    if (cqi > 255) throw std::runtime_error("trace line " + std::to_string(lineno) + ": cqi > 255");
    r.report.cqi = static_cast<uint8_t>(cqi);
    r.report.new_bytes = field<uint32_t>(cols[3], lineno);
    if (!rows.empty() && r.slot < rows.back().slot) {
      throw std::runtime_error("trace line " + std::to_string(lineno) + ": slots must not decrease");
    }
    rows.push_back(r);
  }
  return rows;
}

std::vector<TraceRow> load_trace(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open trace " + path);
  return parse_trace(f);
}

}  // namespace nrs
