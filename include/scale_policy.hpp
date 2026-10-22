#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace spiderless {
enum class CreatureRole { Regular, Boss, Guardian };

// Always start with the game's original scale, including its existing boss or
// guardian adjustment. A repeated publication must not compound our multiplier.
inline float ReplacementScale(std::span<const std::uint32_t> replacedDisplays,
                              std::uint32_t display, float original,
                              CreatureRole role) noexcept {
    if (!std::binary_search(replacedDisplays.begin(), replacedDisplays.end(), display) ||
        !std::isfinite(original) || original <= 0.0f) return original;
    const auto factor = role == CreatureRole::Regular ? 2.0f : 4.0f;
    const auto scaled = original * factor;
    return std::isfinite(scaled) ? scaled : original;
}
}
