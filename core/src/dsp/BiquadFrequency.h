#pragma once

#include <algorithm>

namespace guitarfx
{
/// The frequency a fixed-frequency biquad can safely be designed at, for this sample rate.
///
/// The RBJ cookbook formulas assume 0 < f0 < fs/2. Past Nyquist sin(w0) turns negative, the
/// bandwidth term alpha with it, and the poles leave the unit circle: the filter's output
/// reaches infinity within a few hundred samples. A tone control built at a fixed frequency
/// crosses that line whenever a host runs below twice that frequency. The NAM amp's 6.3 kHz
/// presence bell does at 8 kHz, and the simple cab's low-pass, which reaches 8 kHz, does at
/// 11.025 kHz. BiquadNyquistTests covers each of them.
///
/// The cap is 0.49 of the sample rate, as in ParametricEQEffect: far enough from w0 = pi that
/// sin and cos stay well conditioned. Moving a treble or presence filter down to it changes
/// little, because a rate that low cannot carry the frequency the filter was meant for anyway.
/// The floor keeps w0 positive if a caller ever passes zero or a negative frequency.
[[nodiscard]] inline double ClampBiquadFrequency(double frequencyHz, double sampleRate) noexcept
{
    const double ceiling = sampleRate * 0.49;
    return std::clamp(frequencyHz, std::min(1.0, ceiling), ceiling);
}
} // namespace guitarfx
