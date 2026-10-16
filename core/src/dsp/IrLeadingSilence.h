#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include "dsp/FiniteCheck.h"

namespace guitarfx
{
/**
 * The IR Cabinet's leading-silence trim (`trimLeadingSilence`): how much of what an IR file has
 * in front of its sound both slots lose.
 *
 * A cab IR starts with the time its sound took to reach the mic, plus any silence the capture left
 * in front. That is up to a few milliseconds the player hears as latency, and Quality's truncation
 * counts it against the IR's length. An IR's onset is its first sample within -40 dB of its peak,
 * in either channel of a stereo IR, and the trim stops a few samples short of it so the attack's
 * rise is kept.
 *
 * Two IRs lose the same amount, the earlier one's onset, so IR B keeps its place against IR A and
 * an offset set by ear or by Auto Align means what it did. The trim is counted in host-rate samples
 * and taken from the impulses as they will be convolved, after any resampling, so two IRs at
 * different rates still move by exactly the same time.
 */
namespace ir_leading_silence
{
/// -40 dB: the onset is the first sample at least this fraction of the peak.
constexpr float kOnsetThreshold = 0.01f;

/// Host-rate samples kept in front of the onset, so the trim never cuts into the attack's rise.
constexpr std::size_t kMarginSamples = 4;

/// The first sample within -40 dB of the peak in either channel. `right` is empty for a mono IR.
/// Nothing for an IR with no finite, non-zero sample.
[[nodiscard]] inline std::optional<std::size_t> FindOnsetSample(const std::vector<float>& left,
                                                                const std::vector<float>& right)
{
    // A sample from a file can be anything; a non-finite one counts as silence.
    const auto magnitude = [](float sample) { return IsFinite(sample) ? std::abs(sample) : 0.0f; };
    float peak = 0.0f;

    for (const float sample : left)
    {
        peak = std::max(peak, magnitude(sample));
    }

    for (const float sample : right)
    {
        peak = std::max(peak, magnitude(sample));
    }

    if (peak <= 0.0f)
    {
        return std::nullopt;
    }

    const float threshold = peak * kOnsetThreshold;
    const std::size_t length = std::max(left.size(), right.size());

    for (std::size_t i = 0; i < length; ++i)
    {
        if ((i < left.size() && magnitude(left[i]) >= threshold) ||
            (i < right.size() && magnitude(right[i]) >= threshold))
        {
            return i;
        }
    }

    return std::nullopt; // not reached: the peak itself is over the threshold
}

/// The onset in seconds, at the IR's own rate. Nothing for an empty or silent IR.
[[nodiscard]] inline std::optional<double> FindOnsetSeconds(const std::vector<float>& left,
                                                            const std::vector<float>& right, double sampleRate)
{
    const auto onset = FindOnsetSample(left, right);

    if (!onset || sampleRate <= 0.0)
    {
        return std::nullopt;
    }

    return static_cast<double>(*onset) / sampleRate;
}

/// The leading silence two slots share, in seconds: the earlier of their onsets. A slot with no
/// IR (or a silent one) has no say; with neither, nothing is trimmed.
[[nodiscard]] inline double SharedOnsetSeconds(std::optional<double> onsetA, std::optional<double> onsetB)
{
    if (onsetA && onsetB)
    {
        return std::min(*onsetA, *onsetB);
    }

    return onsetA ? *onsetA : onsetB.value_or(0.0);
}

/// The host-rate samples to trim for a shared onset: up to the onset, less the margin.
[[nodiscard]] inline std::size_t TrimSamples(double onsetSeconds, double hostRate)
{
    if (onsetSeconds <= 0.0 || hostRate <= 0.0)
    {
        return 0;
    }

    // The epsilon keeps a whole-sample onset at the IR's own rate whole after the round trip.
    const auto onset = static_cast<std::size_t>(std::floor(onsetSeconds * hostRate + 1e-6));
    return onset > kMarginSamples ? onset - kMarginSamples : 0;
}

/// Drops `count` samples from the front of `samples`, always leaving at least one.
inline void TrimFront(std::vector<float>& samples, std::size_t count)
{
    count = std::min(count, samples.empty() ? std::size_t{0} : samples.size() - 1);

    if (count > 0)
    {
        samples.erase(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(count));
    }
}
} // namespace ir_leading_silence
} // namespace guitarfx
