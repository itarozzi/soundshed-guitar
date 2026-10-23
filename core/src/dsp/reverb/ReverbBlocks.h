#pragma once

/**
 * ReverbBlocks.h — the pieces Soundshed's algorithmic reverbs share: delay reads without a
 * division, the rotating-phasor LFO, a decay-time-to-gain law, the in-loop chirp dispersion of
 * the spring tanks, a Schroeder allpass on a fractional line, a three-band loop damping filter
 * and the 16-point Hadamard mix.
 *
 * Header-only and allocation-free once prepared, like the effects that include it.
 */

#include "dsp/FiniteCheck.h"
#include "dsp/effects/DelayLineSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace guitarfx::reverb
{
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958648;

/// `readPos` wrapped into [0, size) for a position less than 2·size, without the integer
/// division a `%` costs on every read.
[[nodiscard]] inline std::size_t WrapReadIndex(std::size_t readPos, std::size_t size) noexcept
{
    return readPos >= size ? readPos - size : readPos;
}

/// The sample written `delaySamples` ago into a circular `buffer` whose next write is at
/// `writePos`. Delays past the buffer read its oldest sample.
[[nodiscard]] inline float ReadDelay(const std::vector<float>& buffer, std::size_t writePos,
                                     std::size_t delaySamples) noexcept
{
    if (buffer.empty())
    {
        return 0.0f;
    }

    const std::size_t size = buffer.size();
    const std::size_t back = std::min(delaySamples, size - 1);
    return buffer[WrapReadIndex(writePos + size - back, size)];
}

/// Linear-interpolated read; the caller keeps `delaySamples` at or below buffer.size() - 2.
[[nodiscard]] inline float ReadDelayLinear(const std::vector<float>& buffer, std::size_t writePos,
                                           float delaySamples) noexcept
{
    if (buffer.empty())
    {
        return 0.0f;
    }

    // The two taps are adjacent, so the second is the first stepped back by one — no need to
    // redo the clamp and the wrap for it.
    const std::size_t size = buffer.size();
    const auto d0 = static_cast<std::size_t>(delaySamples);
    const float frac = delaySamples - static_cast<float>(d0);
    const std::size_t back1 = std::min(d0 + 1, size - 1);
    const std::size_t pos1 = WrapReadIndex(writePos + size - back1, size);
    const std::size_t pos0 = WrapReadIndex(pos1 + 1, size);
    const float s1 = buffer[pos1];
    const float s0 = buffer[pos0];
    return s0 + frac * (s1 - s0);
}

/// Loop gain for a recirculation of `loopSeconds` that decays 60 dB in `rt60Seconds`.
[[nodiscard]] inline double Rt60LoopGain(double loopSeconds, double rt60Seconds) noexcept
{
    return std::pow(10.0, -3.0 * loopSeconds / std::max(1.0e-3, rt60Seconds));
}

/// A non-finite sample from upstream becomes silence instead of latching a feedback loop.
[[nodiscard]] inline float ScrubNonFinite(float x) noexcept
{
    return IsFinite(x) ? x : 0.0f;
}

/**
 * A sine LFO held as a rotating unit phasor (sin, cos) rather than a wrapped phase accumulator.
 *
 * There is no phase accumulator and so no wrap: a polynomial sin() evaluated over a wrapped
 * [-π, π] phase steps discontinuously at ±π, which moves every delay it drives at once and ticks
 * audibly once per cycle. A tap offset by δ reads sin(φ+δ) through the angle-addition identity.
 */
class RotatingPhasor
{
  public:
    void SetRate(double hz, double sampleRate) noexcept
    {
        const double increment = kTwoPi * hz / std::max(1.0, sampleRate);
        mRotSin = static_cast<float>(std::sin(increment));
        mRotCos = static_cast<float>(std::cos(increment));
    }

    void Reset() noexcept
    {
        mSin = 0.0f;
        mCos = 1.0f;
    }

    void Advance() noexcept
    {
        const float s = mSin * mRotCos + mCos * mRotSin;
        const float c = mCos * mRotCos - mSin * mRotSin;
        // One Newton step back onto the unit circle. Without it the magnitude creeps over the
        // hours a session runs, because at the lowest rates cos(increment) rounds to 1.0f.
        const float correction = 1.5f - 0.5f * (s * s + c * c);
        mSin = s * correction;
        mCos = c * correction;
    }

    [[nodiscard]] float Sin() const noexcept
    {
        return mSin;
    }

    [[nodiscard]] float Cos() const noexcept
    {
        return mCos;
    }

    /// sin(φ + δ), given sin δ and cos δ.
    [[nodiscard]] float SinOffset(float sinDelta, float cosDelta) const noexcept
    {
        return mSin * cosDelta + mCos * sinDelta;
    }

  private:
    float mSin = 0.0f;
    float mCos = 1.0f;
    float mRotSin = 0.0f;
    float mRotCos = 1.0f;
};

/**
 * In-loop dispersion for spring tanks: for each of `Lines` loops, `Stages` first-order allpasses
 * (a + z^-K)/(1 + a z^-K), each stretched over K samples, in transposed form with one K-sample
 * state line per stage. Group delay climbs from about 0.3 ms at DC to 5.3 ms a pass at fs/2K, so
 * the higher frequencies fall further behind on every trip round the loop and each echo comes
 * back a longer chirp than the last, as a real spring's do.
 *
 * Every chain is a serial dependency, so the lines are stepped a stage at a time together, which
 * lets them overlap rather than wait on each other.
 */
template <std::size_t Lines, std::size_t Stages = 16, std::size_t MaxStretch = 16> class ChirpDispersion
{
  public:
    static constexpr float kCoeff = 0.6f;
    static constexpr double kStretchAt48k = 4.0;

    void Prepare(double sampleRate) noexcept
    {
        mStretch = std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(kStretchAt48k * sampleRate / 48000.0)),
                                           1, MaxStretch);
        Reset();
    }

    void Reset() noexcept
    {
        for (auto& line : mState)
        {
            line.fill(0.0f);
        }

        mPos = 0;
    }

    /// Disperses one sample of each line in place, then advances the shared stretch position.
    void Process(std::array<float, Lines>& lines) noexcept
    {
        for (std::size_t stage = 0; stage < Stages; ++stage)
        {
            const std::size_t slot = stage * MaxStretch + mPos;

            for (std::size_t line = 0; line < Lines; ++line)
            {
                lines[line] = Stage(mState[line][slot], lines[line]);
            }
        }

        if (++mPos >= mStretch)
        {
            mPos = 0;
        }
    }

    /// The same as Process for two banks that share one stretch position, as a spring tank's
    /// left and right loops do; the stage order and arithmetic match two calls exactly.
    void ProcessPair(std::array<float, Lines / 2>& left, std::array<float, Lines / 2>& right) noexcept
    {
        static_assert(Lines % 2 == 0, "a pair splits the lines in half");
        constexpr std::size_t half = Lines / 2;

        for (std::size_t stage = 0; stage < Stages; ++stage)
        {
            const std::size_t slot = stage * MaxStretch + mPos;

            for (std::size_t line = 0; line < half; ++line)
            {
                left[line] = Stage(mState[line][slot], left[line]);
                right[line] = Stage(mState[half + line][slot], right[line]);
            }
        }

        if (++mPos >= mStretch)
        {
            mPos = 0;
        }
    }

  private:
    static float Stage(float& slot, float x) noexcept
    {
        const float delayed = slot;
        const float w = x - kCoeff * delayed;
        slot = std::fabs(w) < 1.0e-9f ? 0.0f : w;
        return kCoeff * w + delayed;
    }

    std::array<std::array<float, Stages * MaxStretch>, Lines> mState{};
    std::size_t mStretch = 4;
    std::size_t mPos = 0;
};

/**
 * Schroeder allpass on a fractional delay line, so its length can glide or be modulated
 * without zipper noise. The line is fed the allpass *output*: feeding back the delay read
 * instead gives a resonant comb, not a flat diffuser.
 */
class FractionalAllpass
{
  public:
    /// Allocates. Call from Prepare, never from Process.
    void Prepare(double maxDelaySamples)
    {
        mLine.Resize(static_cast<std::size_t>(std::ceil(maxDelaySamples)) + 4);
    }

    void Reset() noexcept
    {
        mLine.Clear();
    }

    [[nodiscard]] float Process(float input, double delaySamples, float gain) noexcept
    {
        const float delayed = mLine.ReadLinear(delaySamples);
        const float output = delayed - gain * input;
        mLine.Write(delay_line::FlushDenormal(input + gain * output));
        return output;
    }

    [[nodiscard]] double MaxDelay() const noexcept
    {
        return mLine.MaxDelay();
    }

  private:
    delay_line::FractionalDelayLine mLine;
};

/**
 * Frequency-dependent loss for one recirculating line: the signal split three ways by two
 * complementary one-pole crossovers, each band scaled by its own loop gain. The bands sum back
 * to the input exactly, so with three equal gains it is a plain gain and adds no colour.
 */
class BandDamping
{
  public:
    struct Gains
    {
        float low = 1.0f;
        float mid = 1.0f;
        float high = 1.0f;
    };

    void SetCrossovers(double lowHz, double highHz, double sampleRate) noexcept
    {
        mLowCoeff = OnePoleCoeff(lowHz, sampleRate);
        mHighCoeff = OnePoleCoeff(highHz, sampleRate);
    }

    void Reset() noexcept
    {
        mLowState = 0.0f;
        mHighState = 0.0f;
    }

    [[nodiscard]] float Process(float x, const Gains& gains) noexcept
    {
        mLowState = delay_line::FlushDenormal(mLowState + mLowCoeff * (x - mLowState));
        const float rest = x - mLowState;
        mHighState = delay_line::FlushDenormal(mHighState + mHighCoeff * (rest - mHighState));
        const float high = rest - mHighState;
        return gains.low * mLowState + gains.mid * mHighState + gains.high * high;
    }

    [[nodiscard]] static float OnePoleCoeff(double hz, double sampleRate) noexcept
    {
        const double clamped = std::clamp(hz, 10.0, std::max(20.0, sampleRate * 0.45));
        return static_cast<float>(1.0 - std::exp(-kTwoPi * clamped / std::max(1.0, sampleRate)));
    }

  private:
    float mLowCoeff = 0.0f;
    float mHighCoeff = 1.0f;
    float mLowState = 0.0f;
    float mHighState = 0.0f;
};

/// In-place fast Walsh-Hadamard transform of 16 values, scaled by 1/4 so it is orthonormal and
/// lossless: the feedback matrix of a 16-line delay network.
inline void Hadamard16(std::array<float, 16>& v) noexcept
{
    for (std::size_t span = 1; span < 16; span <<= 1)
    {
        for (std::size_t base = 0; base < 16; base += span << 1)
        {
            for (std::size_t i = base; i < base + span; ++i)
            {
                const float a = v[i];
                const float b = v[i + span];
                v[i] = a + b;
                v[i + span] = a - b;
            }
        }
    }

    for (auto& x : v)
    {
        x *= 0.25f;
    }
}

/// A one-pole smoother toward a target, for control values ramped per sample.
class Smoothed
{
  public:
    void SetTime(double seconds, double sampleRate) noexcept
    {
        mCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0e-4, seconds) * std::max(1.0, sampleRate))));
    }

    void Snap(float value) noexcept
    {
        mValue = value;
        mTarget = value;
    }

    void SetTarget(float value) noexcept
    {
        mTarget = value;
    }

    [[nodiscard]] float Next() noexcept
    {
        mValue += (mTarget - mValue) * mCoeff;
        return mValue;
    }

    [[nodiscard]] float Value() const noexcept
    {
        return mValue;
    }

    [[nodiscard]] float Target() const noexcept
    {
        return mTarget;
    }

  private:
    float mCoeff = 1.0f;
    float mValue = 0.0f;
    float mTarget = 0.0f;
};
} // namespace guitarfx::reverb
