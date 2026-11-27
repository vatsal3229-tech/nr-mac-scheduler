#pragma once

#include <nlohmann/json.hpp>

#include "kpi/kpi.h"

namespace nrs {

nlohmann::json to_json(const KpiReport& k);

}  // namespace nrs
