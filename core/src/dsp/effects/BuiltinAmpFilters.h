#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/BiquadFrequency.h"
#include "dsp/effects/BuiltinAmpVoicing.h"
#include <algorithm>
#include <array>
#include <cmath>

/**
 * The Heavy American's linear filters: the one-pole coupling between preamp stages and the fades
 * that switch those stages in and out, and the gliding biquads that make up its pre-filters, tone
 * stack, voicing and speaker controls.
 */
namespace guitarfx::builtin_amp
{
/// The high-pass/low-pass pair that couples one preamp stage into the next.
struct StageFilter
{
    // Sets where the corners are heading; Advance glides there so a
    // Character sweep never steps the interstage coupling mid-note.
    void SetCorners(double sampleRate, double highPassHz, double lowPassHz)
    {
        const double hp = std::min(highPassHz, sampleRate * 0.45);
        const double lp = std::min(lowPassHz, sampleRate * 0.45);
        hpTarget = static_cast<float>(std::exp(-2.0 * kPi * hp / sampleRate));
        lpTarget = static_cast<float>(1.0 - std::exp(-2.0 * kPi * lp / sampleRate));
    }

    void Advance(float step)
    {
        hpCoefficient += step * (hpTarget - hpCoefficient);
        lpStep += step * (lpTarget - lpStep);
    }

    void Reset()
    {
        hpCoefficient = hpTarget;
        lpStep = lpTarget;
        ClearState();
    }

    /// Silence on both channels, keeping the corners where they are.
    void ClearState()
    {
        previousInput.fill(0.0f);
        previousHighPass.fill(0.0f);
        previousLowPass.fill(0.0f);
    }

    void CopyChannel(int from, int to)
    {
        previousInput[to] = previousInput[from];
        previousHighPass[to] = previousHighPass[from];
        previousLowPass[to] = previousLowPass[from];
    }

    float Process(float sample, int channel)
    {
        const float highPassed = hpCoefficient * (previousHighPass[channel] + sample - previousInput[channel]);
        previousInput[channel] = sample;
        previousHighPass[channel] = highPassed;
        previousLowPass[channel] += lpStep * (highPassed - previousLowPass[channel]);
        return previousLowPass[channel];
    }

    float hpCoefficient = 0.0f;
    float lpStep = 1.0f;
    float hpTarget = 0.0f;
    float lpTarget = 1.0f;
    std::array<float, 2> previousInput = {};
    std::array<float, 2> previousHighPass = {};
    std::array<float, 2> previousLowPass = {};
};

/**
 * Preamp Stages, faded: a stage is brought in or taken out over 20 ms instead of switched, which
 * clicked on a ringing note. Part-way, a stage is crossfaded with its own input (SwitchIn). One
 * coming back from fully out starts from silence, not from whatever its coupling filter held when
 * it last played, and the fade-in covers that start. Stepped once per host sample.
 */
struct StageFades
{
    void SetSampleRate(double sampleRate)
    {
        step = sampleRate > 0.0 ? static_cast<float>(1.0 / (0.02 * sampleRate)) : 1.0f;
    }

    /// Straight to `stageCount`, as after a reset.
    void Settle(int stageCount)
    {
        for (int stage = 0; stage < kMaxStages; ++stage)
        {
            mix[stage] = stage < stageCount ? 1.0f : 0.0f;
        }
    }

    /// One step towards `stageCount`. Says whether any stage moved.
    bool Advance(int stageCount, std::array<StageFilter, kMaxStages>& filters)
    {
        bool moved = false;

        for (int stage = 1; stage < kMaxStages; ++stage)
        {
            const float target = stage < stageCount ? 1.0f : 0.0f;

            if (mix[stage] == target)
            {
                continue;
            }

            if (mix[stage] == 0.0f)
            {
                filters[stage].ClearState();
            }

            mix[stage] = target > mix[stage] ? std::min(mix[stage] + step, 1.0f) : std::max(mix[stage] - step, 0.0f);
            moved = true;
        }

        return moved;
    }

    /// The level table's makeup for the stages that are in, or while some fade, for how far
    /// each one is.
    [[nodiscard]] float Makeup(float gain, float voice) const
    {
        std::array<float, kMaxStages> stageIn = {};
        int stages = 0;
        bool fading = false;

        for (int stage = 0; stage < kMaxStages; ++stage)
        {
            stageIn[stage] = Shape(mix[stage]);
            stages += mix[stage] == 1.0f ? 1 : 0;
            fading = fading || (mix[stage] > 0.0f && mix[stage] < 1.0f);
        }

        return fading ? LevelMakeup(gain, voice, stageIn) : LevelMakeup(gain, voice, stages);
    }

    /// The fade runs linearly in time, but a crossfade that starts or stops with a step in its
    /// slope is a faint tick of its own on a clean note, so it is eased. Exactly 0 and 1 at the
    /// ends.
    [[nodiscard]] static float Shape(float position)
    {
        return position * position * (3.0f - 2.0f * position);
    }

    /// A stage `position` of the way through its fade. Fully in is exactly the stage's own output.
    [[nodiscard]] static float SwitchIn(float bypassed, float engaged, float position)
    {
        return position == 1.0f ? engaged : bypassed + Shape(position) * (engaged - bypassed);
    }

    /// How far each stage is in; the first always is.
    std::array<float, kMaxStages> mix = {1.0f, 1.0f, 0.0f, 0.0f};
    float step = 1.0f;
};

/**
 * A biquad whose coefficients glide towards the last design asked of it, so a control move never
 * steps the filter, with state for two channels. Advance(1.0) lands on the target.
 */
struct GlidingBiquad
{
    void Advance(double step)
    {
        current.b0 += step * (target.b0 - current.b0);
        current.b1 += step * (target.b1 - current.b1);
        current.b2 += step * (target.b2 - current.b2);
        current.a1 += step * (target.a1 - current.a1);
        current.a2 += step * (target.a2 - current.a2);
    }

    void Reset()
    {
        s1.fill(0.0);
        s2.fill(0.0);
    }

    void CopyChannel(int from, int to)
    {
        s1[to] = s1[from];
        s2[to] = s2[from];
    }

    double Process(double input, int channel)
    {
        const double output = current.b0 * input + s1[channel];
        s1[channel] = current.b1 * input - current.a1 * output + s2[channel];
        s2[channel] = current.b2 * input - current.a2 * output;
        return output;
    }

    BiquadCoefficients target;
    BiquadCoefficients current;
    std::array<double, 2> s1 = {};
    std::array<double, 2> s2 = {};
};

// The amp's own copies of the RBJ cookbook designs. BiquadDesign.h's round differently (they
// multiply by 1/a0, and return the exact identity at 0 dB), so moving the amp onto those would
// change its output in the last bits. The shelves take the cookbook's slope S rather than a Q.

[[nodiscard]] inline BiquadCoefficients DesignHighPass(double freq, double Q, double sampleRate)
{
    const double w0 = 2.0 * kPi * ClampBiquadFrequency(freq, sampleRate) / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * Q);

    const double a0 = 1.0 + alpha;
    BiquadCoefficients c;
    c.b0 = (1.0 + cosw0) / 2.0 / a0;
    c.b1 = -(1.0 + cosw0) / a0;
    c.b2 = (1.0 + cosw0) / 2.0 / a0;
    c.a1 = (-2.0 * cosw0) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

[[nodiscard]] inline BiquadCoefficients DesignLowShelf(double freq, double slope, double gainDb, double sampleRate)
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * ClampBiquadFrequency(freq, sampleRate) / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double sqrtA = std::sqrt(A);
    const double alpha = sinw0 / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);

    const double a0 = (A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
    BiquadCoefficients c;
    c.b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
    c.b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
    c.b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    c.a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
    c.a2 = ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    return c;
}

[[nodiscard]] inline BiquadCoefficients DesignHighShelf(double freq, double slope, double gainDb, double sampleRate)
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * ClampBiquadFrequency(freq, sampleRate) / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double sqrtA = std::sqrt(A);
    const double alpha = sinw0 / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);

    const double a0 = (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
    BiquadCoefficients c;
    c.b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha) / a0;
    c.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0) / a0;
    c.b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    c.a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0) / a0;
    c.a2 = ((A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha) / a0;
    return c;
}

[[nodiscard]] inline BiquadCoefficients DesignPeaking(double freq, double Q, double gainDb, double sampleRate)
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * ClampBiquadFrequency(freq, sampleRate) / sampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * Q);

    const double a0 = 1.0 + alpha / A;
    BiquadCoefficients c;
    c.b0 = (1.0 + alpha * A) / a0;
    c.b1 = (-2.0 * cosw0) / a0;
    c.b2 = (1.0 - alpha * A) / a0;
    c.a1 = (-2.0 * cosw0) / a0;
    c.a2 = (1.0 - alpha / A) / a0;
    return c;
}
} // namespace guitarfx::builtin_amp
