#pragma once

#include <array>
#include <cmath>

namespace guitarfx
{
// Two cascaded 2x half-band stages. Each 65-tap Blackman FIR has an exact
// half-sample-band centre and an integer 32-sample round-trip delay at its
// input rate. The decimator takes the even phase, so the total latency is
// exactly 48 host samples at 4x or 32 host samples at 2x.
//
// Run polyphase. Every even tap but the centre is zero, so the even phase is a
// pure 16-sample delay and the odd phase a 32-tap dot product. Each odd-phase
// history is a mirrored ring, every sample stored twice and 32 apart, so the
// newest 32 always sit in a row: no wrap or modulo per tap, and the dot product
// vectorises. Indexed tap by tap, with a modulo each, this was 40% of the amp's
// cost (docs/heavy-american-performance-review-2026-10-16.md).
class BuiltinAmpHalfband2x
{
  public:
    static constexpr int kTaps = 65;
    static constexpr int kCentre = (kTaps - 1) / 2;

    void Prepare()
    {
        constexpr double pi = 3.14159265358979323846;
        std::array<float, kTaps> taps = {};
        double oddSum = 0.0;

        for (int tap = 0; tap < kTaps; ++tap)
        {
            const int offset = tap - kCentre;

            if (offset == 0)
            {
                taps[tap] = 0.5f;
            }
            else if ((offset & 1) != 0)
            {
                const double window =
                    0.42 + 0.5 * std::cos(2.0 * pi * offset / (kTaps - 1)) +
                    0.08 * std::cos(4.0 * pi * offset / (kTaps - 1));
                taps[tap] = static_cast<float>(std::sin(0.5 * pi * offset) / (pi * offset) * window);
                oddSum += taps[tap];
            }
        }

        // The even phase is exactly a delayed input. Match the odd phase's DC
        // gain to it so a constant signal stays constant through interpolation.
        for (int lag = 0; lag < kOddTaps; ++lag)
        {
            mDownTaps[lag] = static_cast<float>(taps[2 * lag + 1] * (0.5 / oddSum));
            mUpTaps[lag] = 2.0f * mDownTaps[lag];
        }

        Reset();
    }

    void Reset()
    {
        mUpHistory.fill(0.0f);
        mOddHistory.fill(0.0f);
        mEvenDelay.fill(0.0f);
        mUpAt = 0;
        mOddAt = 0;
        mEvenAt = 0;
    }

    void Upsample(float input, float& even, float& odd)
    {
        Push(mUpHistory, mUpAt, input);
        const float* recent = mUpHistory.data() + mUpAt; // recent[lag]: the input `lag` samples ago
        even = recent[kEvenDelay];
        odd = Dot(mUpTaps, recent);
    }

    float Downsample(float even, float odd)
    {
        // The odd taps meet the odd-phase samples before this one, and the
        // centre tap the even sample 16 calls back.
        const float delayed = mEvenDelay[mEvenAt];
        mEvenDelay[mEvenAt] = even;
        mEvenAt = (mEvenAt + 1) & (kEvenDelay - 1);
        const float output = 0.5f * delayed + Dot(mDownTaps, mOddHistory.data() + mOddAt);
        Push(mOddHistory, mOddAt, odd);
        return output;
    }

  private:
    static constexpr int kOddTaps = (kTaps - 1) / 2;
    static constexpr int kEvenDelay = kCentre / 2;
    static_assert((kOddTaps & (kOddTaps - 1)) == 0 && (kEvenDelay & (kEvenDelay - 1)) == 0,
                  "the rings wrap with a mask");

    using Taps = std::array<float, kOddTaps>;
    using History = std::array<float, 2 * kOddTaps>;

    static void Push(History& history, int& at, float sample)
    {
        at = (at + kOddTaps - 1) & (kOddTaps - 1);
        history[at] = sample;
        history[at + kOddTaps] = sample;
    }

    static float Dot(const Taps& taps, const float* recent)
    {
        float sum = 0.0f;

        for (int lag = 0; lag < kOddTaps; ++lag)
        {
            sum += taps[lag] * recent[lag];
        }

        return sum;
    }

    Taps mUpTaps = {};
    Taps mDownTaps = {};
    History mUpHistory = {};
    History mOddHistory = {};
    std::array<float, kEvenDelay> mEvenDelay = {};
    int mUpAt = 0;
    int mOddAt = 0;
    int mEvenAt = 0;
};
} // namespace guitarfx
