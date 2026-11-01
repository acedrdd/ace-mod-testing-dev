#pragma once

#include <optional>
#include <string_view>

#include <pl/Config.hpp>

namespace acecull {

struct AceConfig {
  int version = 1;
  bool cullEnabled = true;  // entity culling on/off
  bool heavy = false;       // heavy mode: culls at heavyPercent of the distance
  int heavyPercent = 60;    // heavy strength: % of the distance used in heavy mode
  int distance = 64;        // cull distance in blocks
  int belowBlocks = 0;      // also cull entities this far below the camera (0 = off)
  bool ramEnabled = true;   // RAM optimizer on/off
  bool ramAdaptive = true;  // purge only when memory is low or has grown a lot
  int ramInterval = 30;     // seconds between memory purges
};

inline constexpr int kMinDistance = 4;
inline constexpr int kMaxDistance = 160;
inline constexpr int kMinHeavyPercent = 10;
inline constexpr int kMaxHeavyPercent = 100;
inline constexpr int kMinBelow = 2;   // smallest active value (0 turns it off)
inline constexpr int kMaxBelow = 64;
inline constexpr int kMinRamInterval = 2;
inline constexpr int kMaxRamInterval = 120;

} // namespace acecull

namespace pl::config {

template <> struct Schema<acecull::AceConfig> {
  static constexpr std::string_view title = "Ace Cull";
  static constexpr std::string_view description =
      "Entity culling and RAM optimizer for Minecraft Bedrock 1.26.60.";

  static constexpr FieldSchema field(std::string_view name) {
    if (name == "version") {
      return {"Version", "Config schema version managed by the mod.",
              std::nullopt, std::nullopt, true};
    }
    if (name == "cullEnabled") {
      return {"Entity Cull", "Skip drawing entities beyond the cull distance.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "heavy") {
      return {"Heavy Culling",
              "Cull closer (see Heavy Strength) for a bigger FPS gain.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "distance") {
      return {"Cull Distance", "Entities farther than this (blocks) are skipped.",
              acecull::kMinDistance, acecull::kMaxDistance, false};
    }
    if (name == "heavyPercent") {
      return {"Heavy Strength (%)",
              "In Heavy mode, cull at this % of the distance (lower = more culling).",
              acecull::kMinHeavyPercent, acecull::kMaxHeavyPercent, false};
    }
    if (name == "ramAdaptive") {
      return {"Adaptive RAM",
              "Only purge memory when the phone is low on RAM or the game grew a lot.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "belowBlocks") {
      return {"Cull Below (blocks)",
              "Skip entities this many blocks below you (caves, underground). 0 = off.",
              0, acecull::kMaxBelow, false};
    }
    if (name == "ramEnabled") {
      return {"RAM Optimizer", "Periodically return unused memory to Android.",
              std::nullopt, std::nullopt, false};
    }
    if (name == "ramInterval") {
      return {"RAM Interval", "Seconds between memory purges.",
              acecull::kMinRamInterval, acecull::kMaxRamInterval, false};
    }
    return {};
  }
};

} // namespace pl::config
