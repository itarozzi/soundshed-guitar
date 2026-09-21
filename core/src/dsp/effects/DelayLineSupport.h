#pragma once

/**
 * Shared primitives for the character delays (`delay_tape`, `delay_analog`).
 *
 * These live here rather than in either effect because both need the same modulated
 * delay line, the same glide ramp and the same waveshapers, and because a new source
 * file has to come in under the 800-line budget (`node tools/check-cpp-file-sizes.js`).
 *
 * Everything in here is audio-thread safe once prepared: no allocation, no locks, no
 * `std::sin` and no integer division per sample.
 */

#include "dsp/BiquadFrequency.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace guitarfx::delay_line
{
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958648;

/// Hermite interpolation reads one sample either side of the two it lands between, so the
/// shortest delay it can serve is 2 and the longest is the capacity less 3.
inline constexpr double kMinHermiteDelay = 2.0;
inline constexpr std::size_t kHermiteTailGuard = 3;

/// Feedback loops park tiny values that never quite reach zero; on x86 those become
/// denormals and cost hundreds of cycles each. Anything under this is silence.
[[nodiscard]] inline float FlushDenormal(float value) noexcept
{
    return (std::fabs(value) < 1.0e-25f) ? 0.0f : value;
}

[[nodiscard]] inline std::size_t NextPowerOfTwo(std::size_t value) noexcept
{
    std::size_t result = 1;

    while (result < value)
    {
        result <<= 1;
    }

    return result;
}

/// sin(2*pi*phase) to within about 0.1%, with no call to `std::sin` and no phase drift.
///
/// The parabola 4x(1-|x|) is the classic approximation to sin(pi*x) on [-1,1]; the second
/// line is the usual error-correction term. Both are C1 continuous, which is what matters
/// here — a kink in an LFO driving a delay time is a click.
[[nodiscard]] inline float FastSin01(float phase) noexcept
{
    float folded = phase - std::floor(phase);
    folded = (folded < 0.5f) ? (2.0f * folded) : (2.0f * folded - 2.0f);

    float value = 4.0f * folded * (1.0f - std::fabs(folded));
    value = 0.225f * (value * std::fabs(value) - value) + value;
    return value;
}

/**
 * A sine LFO built on a phase accumulator.
 *
 * Deliberately not a recursive oscillator: those drift in amplitude and need periodic
 * renormalising, and nothing here needs the two or three cycles that would save.
 */
class LfoPhasor
{
  public:
    void Prepare(double sampleRate) noexcept
    {
        mSampleRate = (sampleRate > 0.0) ? sampleRate : 48000.0;
        SetRate(mRateHz);
    }

    void SetRate(double hz) noexcept
    {
        mRateHz = std::max(0.0, hz);
        mIncrement = static_cast<float>(mRateHz / mSampleRate);
    }

    void Reset(float phase = 0.0f) noexcept
    {
        mPhase = phase - std::floor(phase);
    }

    [[nodiscard]] float Next() noexcept
    {
        const float value = FastSin01(mPhase);
        mPhase += mIncrement;

        if (mPhase >= 1.0f)
        {
            mPhase -= 1.0f;
        }

        return value;
    }

  private:
    double mSampleRate = 48000.0;
    double mRateHz = 1.0;
    float mIncrement = 0.0f;
    float mPhase = 0.0f;
};

/// xorshift32. Used for tape hiss, scrape flutter and BBD noise — none of which care about
/// statistical quality, all of which care about not allocating or locking.
class Rng
{
  public:
    void Seed(std::uint32_t seed) noexcept
    {
        mState = (seed == 0u) ? 0x9e3779b9u : seed;
    }

    /// Uniform in [-1, 1).
    [[nodiscard]] float NextBipolar() noexcept
    {
        mState ^= mState << 13;
        mState ^= mState >> 17;
        mState ^= mState << 5;
        return static_cast<float>(static_cast<std::int32_t>(mState)) * (1.0f / 2147483648.0f);
    }

  private:
    std::uint32_t mState = 0x9e3779b9u;
};

/**
 * Circular delay line with a power-of-two buffer.
 *
 * Call order per sample is **read then write**: `Read*(d)` returns the sample written `d`
 * samples ago, and `Write()` stores the current one. Reading after writing would shift
 * every delay by one and read a stale slot at the minimum.
 */
class FractionalDelayLine
{
  public:
    /// Allocates. Call from Prepare, never from Process.
    void Resize(std::size_t minSamples)
    {
        const std::size_t capacity = NextPowerOfTwo(std::max<std::size_t>(16, minSamples + kHermiteTailGuard + 1));
        mBuffer.assign(capacity, 0.0f);
        mMask = capacity - 1;
        mWrite = 0;
    }

    void Clear() noexcept
    {
        std::fill(mBuffer.begin(), mBuffer.end(), 0.0f);
        mWrite = 0;
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mBuffer.empty();
    }

    /// The longest delay this line can serve, in samples.
    [[nodiscard]] double MaxDelay() const noexcept
    {
        return mBuffer.empty() ? 0.0 : static_cast<double>(mBuffer.size() - kHermiteTailGuard - 1);
    }

    /// Clamps to what the line can actually read, so a caller never has to.
    [[nodiscard]] double ClampDelay(double delaySamples) const noexcept
    {
        return std::clamp(delaySamples, kMinHermiteDelay, MaxDelay());
    }

    void Write(float sample) noexcept
    {
        mBuffer[mWrite] = sample;
        mWrite = (mWrite + 1) & mMask;
    }

    /// Four-point third-order Hermite (Catmull-Rom).
    ///
    /// Worth the extra three multiplies over linear: linear interpolation attenuates high
    /// frequencies by an amount that depends on the fractional part, so a delay time being
    /// modulated by wow and flutter gets a shimmer riding on it that is not in the model.
    [[nodiscard]] float ReadHermite(double delaySamples) const noexcept
    {
        const double clamped = ClampDelay(delaySamples);
        const auto whole = static_cast<std::size_t>(clamped);
        const auto fraction = static_cast<float>(clamped - static_cast<double>(whole));

        const std::size_t base = (mWrite - whole) & mMask;
        const float newer = mBuffer[(base + 1) & mMask];
        const float here = mBuffer[base];
        const float older = mBuffer[(base - 1) & mMask];
        const float oldest = mBuffer[(base - 2) & mMask];

        const float c0 = here;
        const float c1 = 0.5f * (older - newer);
        const float c2 = newer - 2.5f * here + 2.0f * older - 0.5f * oldest;
        const float c3 = 0.5f * (oldest - newer) + 1.5f * (here - older);

        return ((c3 * fraction + c2) * fraction + c1) * fraction + c0;
    }

    [[nodiscard]] float ReadLinear(double delaySamples) const noexcept
    {
        const double clamped = ClampDelay(delaySamples);
        const auto whole = static_cast<std::size_t>(clamped);
        const auto fraction = static_cast<float>(clamped - static_cast<double>(whole));

        const std::size_t base = (mWrite - whole) & mMask;
        return mBuffer[base] * (1.0f - fraction) + mBuffer[(base - 1) & mMask] * fraction;
    }

  private:
    std::vector<float> mBuffer;
    std::size_t mMask = 0;
    std::size_t mWrite = 0;
};

/**
 * One-pole ramp toward a target, in whatever unit the caller keeps.
 *
 * This is what stops a delay-time change clicking, and on the tape effect it is also the
 * effect: a read pointer that takes time to reach its new position bends the pitch of
 * everything already on the tape.
 */
class GlideRamp
{
  public:
    void Prepare(double sampleRate) noexcept
    {
        mSampleRate = (sampleRate > 0.0) ? sampleRate : 48000.0;
        SetTimeConstantMs(mTimeConstantMs);
    }

    /// A time constant of zero (or less) snaps, which is what a tempo-synced delay wants.
    void SetTimeConstantMs(double milliseconds) noexcept
    {
        mTimeConstantMs = std::max(0.0, milliseconds);

        if (mTimeConstantMs <= 0.0)
        {
            mCoefficient = 1.0;
            return;
        }

        mCoefficient = 1.0 - std::exp(-1000.0 / (mSampleRate * mTimeConstantMs));
    }

    /// Caps how far the value may move per sample. For a delay time in samples this is the
    /// playback speed error: a step of s plays the tape at (1 - s) times its speed. An
    /// exponential ramp alone starts fastest, and a big enough jump in time makes it move
    /// the read position faster than real time — the tape plays backwards through the
    /// glide. Measured: 300 to 600 ms with a 400 ms glide read down to 28 Hz from 440 Hz.
    /// Zero means no cap.
    void SetMaxStep(double samplesPerSample) noexcept
    {
        mMaxStep = std::max(0.0, samplesPerSample);
    }

    void SetTarget(double target) noexcept
    {
        mTarget = target;
    }

    void Snap(double value) noexcept
    {
        mTarget = value;
        mValue = value;
    }

    [[nodiscard]] bool Settled() const noexcept
    {
        return std::fabs(mTarget - mValue) < 1.0e-6;
    }

    [[nodiscard]] double Next() noexcept
    {
        double step = mCoefficient * (mTarget - mValue);

        if (mMaxStep > 0.0)
        {
            step = std::clamp(step, -mMaxStep, mMaxStep);
        }

        mValue += step;
        return mValue;
    }

    [[nodiscard]] double Value() const noexcept
    {
        return mValue;
    }

    [[nodiscard]] double Target() const noexcept
    {
        return mTarget;
    }

  private:
    double mSampleRate = 48000.0;
    double mTimeConstantMs = 0.0;
    double mCoefficient = 1.0;
    double mMaxStep = 0.0;
    double mValue = 0.0;
    double mTarget = 0.0;
};

/// A glided delay may change by at most this many samples per sample, so playback speed
/// stays between half and one and a half times normal: at most an octave down and a fifth
/// up, and never backwards. A tape capstan and a BBD clock are both bounded like this.
inline constexpr double kMaxGlideStep = 0.5;

/// |H(e^jw)| of a one-pole section with pole `pole` and numerator gain `gain`.
[[nodiscard]] inline double OnePoleMagnitude(double gain, double pole, double w) noexcept
{
    const double re = 1.0 - pole * std::cos(w);
    const double im = pole * std::sin(w);
    return std::fabs(gain) / std::sqrt(re * re + im * im);
}

/// One-pole low-pass, same formulation as DelayEffect so the two sound alike where they
/// are meant to.
class OnePoleLp
{
  public:
    void SetCutoff(double hz, double sampleRate) noexcept
    {
        if (sampleRate <= 0.0)
        {
            return;
        }

        const double clamped = std::clamp(hz, 10.0, sampleRate * 0.49);
        mCoefficient = static_cast<float>(1.0 - std::exp(-kTwoPi * clamped / sampleRate));
    }

    void Reset() noexcept
    {
        mState = 0.0f;
    }

    [[nodiscard]] float Process(float input) noexcept
    {
        mState = FlushDenormal(mCoefficient * input + (1.0f - mCoefficient) * mState);
        return mState;
    }

    /// Gain at angular frequency `w` (radians per sample).
    [[nodiscard]] double Magnitude(double w) const noexcept
    {
        return OnePoleMagnitude(mCoefficient, 1.0 - mCoefficient, w);
    }

  private:
    float mCoefficient = 1.0f;
    float mState = 0.0f;
};

/// One-pole high-pass.
class OnePoleHp
{
  public:
    void SetCutoff(double hz, double sampleRate) noexcept
    {
        if (sampleRate <= 0.0)
        {
            return;
        }

        const double clamped = std::clamp(hz, 1.0, sampleRate * 0.49);
        mCoefficient = static_cast<float>(std::exp(-kTwoPi * clamped / sampleRate));
    }

    void Reset() noexcept
    {
        mState = 0.0f;
        mPreviousInput = 0.0f;
    }

    [[nodiscard]] float Process(float input) noexcept
    {
        mState = FlushDenormal(mCoefficient * (mState + input - mPreviousInput));
        mPreviousInput = input;
        return mState;
    }

    /// Gain at angular frequency `w`: c(1 - z^-1) / (1 - c z^-1).
    [[nodiscard]] double Magnitude(double w) const noexcept
    {
        const double zeroMagnitude = 2.0 * std::fabs(std::sin(0.5 * w));
        return zeroMagnitude * OnePoleMagnitude(mCoefficient, mCoefficient, w);
    }

  private:
    float mCoefficient = 1.0f;
    float mState = 0.0f;
    float mPreviousInput = 0.0f;
};

/// RBJ biquad, transposed direct form II. Only the two shapes these effects need are
/// built: a resonant low-pass (the BBD reconstruction filter) and a peaking bell (the
/// tape head bump).
class Biquad
{
  public:
    void SetLowpass(double hz, double sampleRate, double q) noexcept
    {
        if (sampleRate <= 0.0)
        {
            return;
        }

        const double w0 = kTwoPi * ClampBiquadFrequency(hz, sampleRate) / sampleRate;
        const double cosW0 = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * std::max(0.05, q));

        const double b0 = (1.0 - cosW0) * 0.5;
        const double b1 = 1.0 - cosW0;
        const double b2 = b0;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cosW0;
        const double a2 = 1.0 - alpha;

        Normalize(b0, b1, b2, a0, a1, a2);
    }

    void SetPeaking(double hz, double sampleRate, double gainDb, double q) noexcept
    {
        if (sampleRate <= 0.0)
        {
            return;
        }

        const double amplitude = std::pow(10.0, gainDb / 40.0);
        const double w0 = kTwoPi * ClampBiquadFrequency(hz, sampleRate) / sampleRate;
        const double cosW0 = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * std::max(0.05, q));

        const double b0 = 1.0 + alpha * amplitude;
        const double b1 = -2.0 * cosW0;
        const double b2 = 1.0 - alpha * amplitude;
        const double a0 = 1.0 + alpha / amplitude;
        const double a1 = b1;
        const double a2 = 1.0 - alpha / amplitude;

        Normalize(b0, b1, b2, a0, a1, a2);
    }

    void SetBypass() noexcept
    {
        mB0 = 1.0f;
        mB1 = 0.0f;
        mB2 = 0.0f;
        mA1 = 0.0f;
        mA2 = 0.0f;
    }

    void Reset() noexcept
    {
        mZ1 = 0.0f;
        mZ2 = 0.0f;
    }

    [[nodiscard]] float Process(float input) noexcept
    {
        const float output = mB0 * input + mZ1;
        mZ1 = FlushDenormal(mB1 * input - mA1 * output + mZ2);
        mZ2 = FlushDenormal(mB2 * input - mA2 * output);
        return output;
    }

    /// Gain at angular frequency `w` (radians per sample).
    [[nodiscard]] double Magnitude(double w) const noexcept
    {
        const double c1 = std::cos(w);
        const double s1 = std::sin(w);
        const double c2 = std::cos(2.0 * w);
        const double s2 = std::sin(2.0 * w);
        const double numRe = mB0 + mB1 * c1 + mB2 * c2;
        const double numIm = -(mB1 * s1 + mB2 * s2);
        const double denRe = 1.0 + mA1 * c1 + mA2 * c2;
        const double denIm = -(mA1 * s1 + mA2 * s2);
        return std::sqrt((numRe * numRe + numIm * numIm) / (denRe * denRe + denIm * denIm));
    }

  private:
    void Normalize(double b0, double b1, double b2, double a0, double a1, double a2) noexcept
    {
        const double inverse = 1.0 / a0;
        mB0 = static_cast<float>(b0 * inverse);
        mB1 = static_cast<float>(b1 * inverse);
        mB2 = static_cast<float>(b2 * inverse);
        mA1 = static_cast<float>(a1 * inverse);
        mA2 = static_cast<float>(a2 * inverse);
    }

    float mB0 = 1.0f;
    float mB1 = 0.0f;
    float mB2 = 0.0f;
    float mA1 = 0.0f;
    float mA2 = 0.0f;
    float mZ1 = 0.0f;
    float mZ2 = 0.0f;
};

/// Two cascaded biquads at the Butterworth Q pair, giving a 4-pole 24 dB/octave roll-off.
/// The BBD reconstruction filter has to be this steep: a one-pole leaves enough energy
/// above the clock's Nyquist that the "gets darker as Time goes up" behaviour reads as a
/// tone control rather than a bandwidth collapse.
class Lowpass4
{
  public:
    void SetCutoff(double hz, double sampleRate) noexcept
    {
        mStageA.SetLowpass(hz, sampleRate, 0.54119610);
        mStageB.SetLowpass(hz, sampleRate, 1.30656296);
    }

    void Reset() noexcept
    {
        mStageA.Reset();
        mStageB.Reset();
    }

    [[nodiscard]] float Process(float input) noexcept
    {
        return mStageB.Process(mStageA.Process(input));
    }

  private:
    Biquad mStageA;
    Biquad mStageB;
};

/// tanh as a Pade (7,6) rational: measured worst error 1.1e-4 over [-4, 4].
///
/// The input is clamped to that range first, for two reasons. Past it the rational drifts
/// above 1 and then diverges, which in a feedback loop is the difference between saturating
/// and exploding; and true tanh is already 0.9993 at 4, so clamping costs nothing audible.
/// A cheaper Pade (3,2) was tried first and measured 2.4% error, which is fine for a
/// saturator but makes the shape hard to reason about against a reference.
[[nodiscard]] inline float FastTanh(float x) noexcept
{
    const float clamped = std::clamp(x, -4.0f, 4.0f);
    const float squared = clamped * clamped;
    const float numerator = clamped * (135135.0f + squared * (17325.0f + squared * (378.0f + squared)));
    const float denominator = 135135.0f + squared * (62370.0f + squared * (3150.0f + 28.0f * squared));
    return numerator / denominator;
}

/// Symmetric soft clip at unity gain through the origin, so `drive` changes the knee
/// without changing quiet-signal level.
[[nodiscard]] inline float SoftSaturate(float x, float drive) noexcept
{
    if (drive <= 0.0f)
    {
        return x;
    }

    const float gain = 1.0f + drive;
    return FastTanh(x * gain) / gain;
}

/// A device's headroom: linear up to `knee`, then a tanh-shaped approach to `ceiling` that
/// never reaches it, C1 at the knee so it clicks nowhere.
///
/// This is not a tone control. It leaves ordinary levels alone and exists so that a loop
/// with Feedback above unity has a bound whatever Saturation is set to. It replaces a fixed
/// minimum drive into `SoftSaturate`, which was measured costing 2.4 dB at -6 dBFS with
/// Saturation at zero: tanh has no linear region, so a tanh rail colours everything.
[[nodiscard]] inline float RailClip(float x, float knee, float ceiling) noexcept
{
    const float magnitude = std::fabs(x);

    if (magnitude <= knee)
    {
        return x;
    }

    const float span = ceiling - knee;
    const float shaped = knee + span * FastTanh((magnitude - knee) / span);
    return std::copysign(shaped, x);
}

/// Asymmetric soft clip: tape's magnetic curve is not symmetric about zero, and the second
/// harmonic that asymmetry produces is most of why a driven tape echo sounds warm rather
/// than merely clipped. `bias` shifts the operating point; the DC it introduces is removed
/// by subtracting the shaped bias back out.
[[nodiscard]] inline float AsymSaturate(float x, float drive, float bias) noexcept
{
    if (drive <= 0.0f)
    {
        return x;
    }

    const float gain = 1.0f + drive;
    return (FastTanh(x * gain + bias) - FastTanh(bias)) / gain;
}

/**
 * Envelope follower with separate attack and release, tracking |x|.
 *
 * Shared by the ducking control on both effects and by the analog delay's compander.
 */
class EnvelopeFollower
{
  public:
    void Prepare(double sampleRate) noexcept
    {
        mSampleRate = (sampleRate > 0.0) ? sampleRate : 48000.0;
        SetTimes(mAttackMs, mReleaseMs);
    }

    void SetTimes(double attackMs, double releaseMs) noexcept
    {
        mAttackMs = std::max(0.01, attackMs);
        mReleaseMs = std::max(0.01, releaseMs);
        mAttackCoefficient = static_cast<float>(1.0 - std::exp(-1000.0 / (mSampleRate * mAttackMs)));
        mReleaseCoefficient = static_cast<float>(1.0 - std::exp(-1000.0 / (mSampleRate * mReleaseMs)));
    }

    void Reset() noexcept
    {
        mEnvelope = 0.0f;
    }

    [[nodiscard]] float Process(float input) noexcept
    {
        const float rectified = std::fabs(input);
        const float coefficient = (rectified > mEnvelope) ? mAttackCoefficient : mReleaseCoefficient;
        mEnvelope = FlushDenormal(mEnvelope + coefficient * (rectified - mEnvelope));
        return mEnvelope;
    }

    [[nodiscard]] float Value() const noexcept
    {
        return mEnvelope;
    }

  private:
    double mSampleRate = 48000.0;
    double mAttackMs = 1.0;
    double mReleaseMs = 100.0;
    float mAttackCoefficient = 1.0f;
    float mReleaseCoefficient = 1.0f;
    float mEnvelope = 0.0f;
};

/**
 * How open a noise source should be: fully open while someone is playing, shut once the
 * input has been quiet for a while, ramping between.
 *
 * For tape hiss and a BBD's noise floor, so that a delay nobody is playing into is silent
 * rather than hissing at -95 dBFS forever. It follows the *input*, never the line: on the
 * line, noise fed back through Feedback would hold its own gate open. The floor matters as
 * much as the level — an envelope decaying exponentially never quite reaches zero, and
 * without a floor hiss was still measurable (3e-5) four seconds after playing stopped.
 */
class IdleGate
{
  public:
    void Prepare(double sampleRate) noexcept
    {
        mFollower.Prepare(sampleRate);
        mFollower.SetTimes(kAttackMs, kReleaseMs);
    }

    void Reset() noexcept
    {
        mFollower.Reset();
    }

    /// 0 (shut) to 1 (open) for this input sample.
    [[nodiscard]] float Process(float input) noexcept
    {
        const float envelope = mFollower.Process(input);
        return std::clamp((envelope - kFloor) / (kLevel - kFloor), 0.0f, 1.0f);
    }

  private:
    static constexpr double kAttackMs = 5.0;
    static constexpr double kReleaseMs = 300.0;
    /// Open from -40 dBFS, shut below -66 dBFS.
    static constexpr float kLevel = 0.01f;
    static constexpr float kFloor = 5.0e-4f;

    EnvelopeFollower mFollower;
};
} // namespace guitarfx::delay_line
