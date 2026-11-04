#include "core/RecipeMath.h"

#include <algorithm>
#include <cmath>

namespace drinkbot {

float roundToTenth(float value) {
  return std::round(value * 10.0f) / 10.0f;
}

std::vector<PourStep> computePours(const Config& config, const Recipe& recipe, float volumeMl) {
  float totalParts = 0;
  for (const auto& ing : recipe.ingredients) {
    totalParts += ing.parts;
  }

  std::vector<PourStep> steps;
  if (totalParts <= 0) {
    return steps;
  }
  for (const auto& ing : recipe.ingredients) {
    PourStep s;
    s.liquid = ing.liquid;
    const PumpConfig* pump = config.findPumpByLiquid(ing.liquid);
    s.pumpId = pump ? pump->pumpId : 0;
    s.ml = roundToTenth(volumeMl * ing.parts / totalParts);
    s.order = ing.order;
    steps.push_back(s);
  }
  std::stable_sort(steps.begin(), steps.end(),
                   [](const PourStep& a, const PourStep& b) { return a.order < b.order; });
  return steps;
}

std::vector<LiquidTotal> totalsByPump(const std::vector<PourStep>& steps) {
  std::vector<LiquidTotal> totals;
  for (const auto& s : steps) {
    auto it = std::find_if(totals.begin(), totals.end(), [&](const LiquidTotal& t) {
      return s.pumpId != 0 ? t.pumpId == s.pumpId : equalsIgnoreCase(t.liquid, s.liquid);
    });
    if (it == totals.end()) {
      totals.push_back({s.pumpId, s.liquid, s.ml});
    } else {
      it->ml += s.ml;
    }
  }
  return totals;
}

std::vector<std::string> missingLiquids(const Config& config, const Recipe& recipe) {
  std::vector<std::string> missing;
  for (const auto& ing : recipe.ingredients) {
    if (!config.findPumpByLiquid(ing.liquid)) {
      bool listed = std::any_of(missing.begin(), missing.end(),
                                [&](const std::string& m) { return equalsIgnoreCase(m, ing.liquid); });
      if (!listed) {
        missing.push_back(ing.liquid);
      }
    }
  }
  return missing;
}

}  // namespace drinkbot
