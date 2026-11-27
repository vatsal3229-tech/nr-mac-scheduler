#include <gtest/gtest.h>

#include <map>
#include <stdexcept>

#include "config/config.h"

namespace nrs {
namespace {

EnvLookup env_of(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](const std::string& k) -> std::optional<std::string> {
    auto it = vars.find(k);
    if (it == vars.end()) return std::nullopt;
    return it->second;
  };
}

TEST(Config, Defaults) {
  Config c = load_config_from_string("", env_of({}));
  EXPECT_EQ(c.policy, Policy::kProportionalFair);
  EXPECT_EQ(c.cell.num_prbs, 52);
  EXPECT_DOUBLE_EQ(c.cell.overhead, 0.14);
  EXPECT_DOUBLE_EQ(c.cell.ewma_time_constant_slots, 100.0);
  EXPECT_EQ(c.udp_port, 38472);
  EXPECT_EQ(c.http_port, 8080);
}

TEST(Config, YamlOverridesDefaults) {
  Config c = load_config_from_string(
      "policy: round_robin\ncell:\n  num_prbs: 106\n  ewma_time_constant_slots: 50\n"
      "network:\n  http_port: 9000\n",
      env_of({}));
  EXPECT_EQ(c.policy, Policy::kRoundRobin);
  EXPECT_EQ(c.cell.num_prbs, 106);
  EXPECT_DOUBLE_EQ(c.cell.ewma_time_constant_slots, 50.0);
  EXPECT_EQ(c.http_port, 9000);
}

TEST(Config, EnvOverridesYaml) {
  Config c = load_config_from_string(
      "policy: round_robin\ncell:\n  num_prbs: 106\n",
      env_of({{"NRS_POLICY", "max_throughput"}, {"NRS_NUM_PRBS", "24"}, {"NRS_UDP_PORT", "5000"}}));
  EXPECT_EQ(c.policy, Policy::kMaxThroughput);
  EXPECT_EQ(c.cell.num_prbs, 24);
  EXPECT_EQ(c.udp_port, 5000);
}

TEST(Config, UnknownKeysFailLoudly) {
  // A ConfigMap typo ("num_prb") must not silently fall back to the default.
  EXPECT_THROW(load_config_from_string("cell:\n  num_prb: 52\n", env_of({})),
               std::invalid_argument);
  EXPECT_THROW(load_config_from_string("polcy: pf\n", env_of({})), std::invalid_argument);
}

TEST(Config, InvalidValuesRejected) {
  const auto bad = [](const std::string& yaml) {
    EXPECT_THROW(load_config_from_string(yaml, env_of({})), std::invalid_argument) << yaml;
  };
  bad("policy: fastest\n");
  bad("cell:\n  num_prbs: 0\n");
  bad("cell:\n  num_prbs: 276\n");
  bad("cell:\n  num_prbs: -5\n");
  bad("cell:\n  num_prbs: abc\n");
  bad("cell:\n  overhead: 1.0\n");
  bad("cell:\n  ewma_time_constant_slots: 0.5\n");
  bad("network:\n  udp_port: 70000\n");
  bad("cell: 5\n");
  bad("[1, 2]\n");
  EXPECT_THROW(load_config_from_string("", env_of({{"NRS_NUM_PRBS", "lots"}})),
               std::invalid_argument);
}

}  // namespace
}  // namespace nrs
