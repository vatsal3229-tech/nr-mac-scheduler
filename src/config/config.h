#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/types.h"
#include "sched/scheduler.h"

namespace nrs {

struct Config {
  Policy policy = Policy::kProportionalFair;
  CellConfig cell;
  std::string udp_bind = "0.0.0.0";
  uint16_t udp_port = 38472;
  std::string http_bind = "0.0.0.0";
  uint16_t http_port = 8080;
  uint32_t queue_capacity = 65536;
};

// Injected so tests do not depend on the process environment.
using EnvLookup = std::function<std::optional<std::string>(const std::string&)>;
EnvLookup process_env();

// Precedence: built-in defaults < YAML file < NRS_* environment variables.
// In Kubernetes the YAML comes from a ConfigMap and env vars allow per-pod
// overrides. Unknown keys and out-of-range values throw std::invalid_argument:
// a typo in a ConfigMap must fail loudly, not silently fall back to a default.
Config load_config(const std::optional<std::string>& yaml_path, const EnvLookup& env);
Config load_config_from_string(const std::string& yaml, const EnvLookup& env);

void validate(const Config& c);

}  // namespace nrs
