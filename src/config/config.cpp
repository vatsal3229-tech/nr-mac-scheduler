#include "config/config.h"

#include <yaml-cpp/yaml.h>

#include <cerrno>
#include <cstdlib>
#include <set>
#include <stdexcept>

namespace nrs {
namespace {

[[noreturn]] void fail(const std::string& msg) { throw std::invalid_argument("config: " + msg); }

uint64_t parse_uint(const std::string& key, const std::string& s, uint64_t lo, uint64_t hi) {
  if (s.empty() || s[0] == '-') fail(key + " must be a non-negative integer, got '" + s + "'");
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
  if (errno != 0 || *end != '\0') fail(key + " must be an integer, got '" + s + "'");
  if (v < lo || v > hi) {
    fail(key + " must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "], got " + s);
  }
  return v;
}

double parse_double(const std::string& key, const std::string& s) {
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (s.empty() || errno != 0 || *end != '\0') fail(key + " must be a number, got '" + s + "'");
  return v;
}

Policy parse_policy_or_fail(const std::string& s) {
  auto p = parse_policy(s);
  if (!p) fail("unknown policy '" + s + "' (round_robin | proportional_fair | max_throughput)");
  return *p;
}

// One setter per key, shared by YAML and env so both paths parse identically.
void set_key(Config& c, const std::string& key, const std::string& v) {
  if (key == "policy") {
    c.policy = parse_policy_or_fail(v);
  } else if (key == "cell.num_prbs") {
    // 275 is the NR maximum PRBs per carrier (TS 38.101-1).
    c.cell.num_prbs = static_cast<uint16_t>(parse_uint(key, v, 1, 275));
  } else if (key == "cell.overhead") {
    c.cell.overhead = parse_double(key, v);
  } else if (key == "cell.ewma_time_constant_slots") {
    c.cell.ewma_time_constant_slots = parse_double(key, v);
  } else if (key == "cell.slot_duration_us") {
    c.cell.slot_duration_us = static_cast<uint32_t>(parse_uint(key, v, 62, 1'000'000));
  } else if (key == "cell.max_ues") {
    c.cell.max_ues = static_cast<uint32_t>(parse_uint(key, v, 1, 65536));
  } else if (key == "cell.max_buffer_bytes") {
    c.cell.max_buffer_bytes = parse_uint(key, v, 1, UINT64_MAX / 2);
  } else if (key == "network.udp_bind") {
    c.udp_bind = v;
  } else if (key == "network.udp_port") {
    c.udp_port = static_cast<uint16_t>(parse_uint(key, v, 0, 65535));
  } else if (key == "network.http_bind") {
    c.http_bind = v;
  } else if (key == "network.http_port") {
    c.http_port = static_cast<uint16_t>(parse_uint(key, v, 0, 65535));
  } else if (key == "network.queue_capacity") {
    c.queue_capacity = static_cast<uint32_t>(parse_uint(key, v, 1, 1u << 24));
  } else {
    fail("unknown key '" + key + "'");
  }
}

void apply_yaml(Config& c, const YAML::Node& root) {
  if (!root || root.IsNull()) return;
  if (!root.IsMap()) fail("top level must be a mapping");
  static const std::set<std::string> kSections = {"cell", "network"};
  for (const auto& kv : root) {
    const auto key = kv.first.as<std::string>();
    if (kSections.count(key) != 0) {
      if (!kv.second.IsMap()) fail("'" + key + "' must be a mapping");
      for (const auto& inner : kv.second) {
        set_key(c, key + "." + inner.first.as<std::string>(), inner.second.as<std::string>());
      }
    } else {
      if (!kv.second.IsScalar()) fail("'" + key + "' must be a scalar");
      set_key(c, key, kv.second.as<std::string>());
    }
  }
}

void apply_env(Config& c, const EnvLookup& env) {
  static const std::pair<const char*, const char*> kEnv[] = {
      {"NRS_POLICY", "policy"},
      {"NRS_NUM_PRBS", "cell.num_prbs"},
      {"NRS_OVERHEAD", "cell.overhead"},
      {"NRS_EWMA_TC_SLOTS", "cell.ewma_time_constant_slots"},
      {"NRS_SLOT_US", "cell.slot_duration_us"},
      {"NRS_MAX_UES", "cell.max_ues"},
      {"NRS_MAX_BUFFER_BYTES", "cell.max_buffer_bytes"},
      {"NRS_UDP_BIND", "network.udp_bind"},
      {"NRS_UDP_PORT", "network.udp_port"},
      {"NRS_HTTP_BIND", "network.http_bind"},
      {"NRS_HTTP_PORT", "network.http_port"},
      {"NRS_QUEUE_CAPACITY", "network.queue_capacity"},
  };
  for (const auto& [var, key] : kEnv) {
    if (auto v = env(var)) set_key(c, key, *v);
  }
}

}  // namespace

EnvLookup process_env() {
  return [](const std::string& name) -> std::optional<std::string> {
    const char* v = std::getenv(name.c_str());
    if (v == nullptr) return std::nullopt;
    return std::string(v);
  };
}

void validate(const Config& c) {
  if (!(c.cell.overhead >= 0.0 && c.cell.overhead < 1.0)) fail("cell.overhead must be in [0, 1)");
  if (!(c.cell.ewma_time_constant_slots >= 1.0)) fail("cell.ewma_time_constant_slots must be >= 1");
}

Config load_config_from_string(const std::string& yaml, const EnvLookup& env) {
  Config c;
  try {
    apply_yaml(c, YAML::Load(yaml));
  } catch (const YAML::Exception& e) {
    fail(std::string("YAML error: ") + e.what());
  }
  apply_env(c, env);
  validate(c);
  return c;
}

Config load_config(const std::optional<std::string>& yaml_path, const EnvLookup& env) {
  Config c;
  if (yaml_path) {
    try {
      apply_yaml(c, YAML::LoadFile(*yaml_path));
    } catch (const YAML::Exception& e) {
      fail("cannot load " + *yaml_path + ": " + e.what());
    }
  }
  apply_env(c, env);
  validate(c);
  return c;
}

}  // namespace nrs
