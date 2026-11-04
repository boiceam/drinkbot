#pragma once

#include <string>
#include <vector>

#include "core/Config.h"

namespace drinkbot {

// One pour of a recipe at a given total volume (SPEC_v3 §9.5).
struct PourStep {
  std::string liquid;
  int pumpId = 0;   // 0 if the liquid is not loaded on any pump
  float ml = 0;     // rounded to 0.1 ml
  int order = 0;
};

float roundToTenth(float value);

// ml_i = volumeMl * parts_i / sum(parts), sorted by `order` with ties kept
// in list order.
std::vector<PourStep> computePours(const Config& config, const Recipe& recipe, float volumeMl);

struct LiquidTotal {
  int pumpId = 0;
  std::string liquid;
  float ml = 0;
};

// Total per pump, so a liquid listed twice is checked once against its level.
std::vector<LiquidTotal> totalsByPump(const std::vector<PourStep>& steps);

// Liquids in the recipe that no pump has loaded (empty = all available).
std::vector<std::string> missingLiquids(const Config& config, const Recipe& recipe);

}  // namespace drinkbot
