#pragma once

// Helpers the LooperEngine translation units share. Not part of the engine's interface.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace guitarfx::looper::detail
{
/// A span still being recorded has no end yet.
inline constexpr std::int64_t kOpenEnd = std::numeric_limits<std::int64_t>::max();

/// `value` modulo a positive `modulus`, never negative.
inline std::int64_t Wrap(std::int64_t value, std::int64_t modulus) noexcept
{
    const std::int64_t r = value % modulus;
    return r < 0 ? r + modulus : r;
}

inline std::int64_t ToSamples(double seconds, double sampleRate) noexcept
{
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(seconds * sampleRate)));
}

/// One step of a gain glide: `value` moved towards `target` by at most `maxDelta`.
inline float MoveTowards(float value, float target, float maxDelta) noexcept
{
    const float delta = target - value;
    return delta > maxDelta ? value + maxDelta : (delta < -maxDelta ? value - maxDelta : target);
}

inline float PeakOf(float a, float b) noexcept
{
    return std::max(std::abs(a), std::abs(b));
}
} // namespace guitarfx::looper::detail
