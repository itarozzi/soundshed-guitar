#pragma once

#include <array>
#include <cmath>

namespace guitarfx
{
// Two cascaded 2x half-band stages. Each 65-tap Blackman FIR has an exact
// half-sample-band centre and an integer 32-sample round-trip delay at its
// input rate. The decimator takes the even phase, so the total latency is
// exactly 48 host samples at 4x or 32 host samples at 2x.
class BuiltinAmpHalfband2x
{
  public:
    static constexpr int kTaps = 65;
    static constexpr int kCentre = (kTaps - 1) / 2;

    void Prepare()
    {
        constexpr double pi = 3.14159265358979323846;
        double oddSum = 0.0;

        for (int tap = 0; tap < kTaps; ++tap)
        {
            const int offset = tap - kCentre;

            if (offset == 0)
            {
                mTaps[tap] = 0.5f;
            }
            else if ((offset & 1) == 0)
            {
                mTaps[tap] = 0.0f;
            }
            else
            {
                const double window =
                    0.42 + 0.5 * std::cos(2.0 * pi * offset / (kTaps - 1)) +
                    0.08 * std::cos(4.0 * pi * offset / (kTaps - 1));
                mTaps[tap] = static_cast<float>(std::sin(0.5 * pi * offset) / (pi * offset) * window);
                oddSum += mTaps[tap];
            }
        }

        // The even phase is exactly a delayed input. Match the odd phase's DC
        // gain to it so a constant signal stays constant through interpolation.
        for (int tap = 1; tap < kTaps; tap += 2)
        {
            mTaps[tap] = static_cast<float>(mTaps[tap] * (0.5 / oddSum));
        }

        Reset();
    }

    void Reset()
    {
        mUpHistory.fill(0.0f);
        mDownHistory.fill(0.0f);
        mUpWrite = 0;
        mDownWrite = 0;
    }

    void Upsample(float input, float& even, float& odd)
    {
        mUpHistory[mUpWrite] = input;
        even = mUpHistory[(mUpWrite + kUpHistory - kCentre / 2) % kUpHistory];
        odd = 0.0f;

        for (int tap = 1; tap < kTaps; tap += 2)
        {
            const int lag = tap / 2;
            odd += 2.0f * mTaps[tap] * mUpHistory[(mUpWrite + kUpHistory - lag) % kUpHistory];
        }

        mUpWrite = (mUpWrite + 1) % kUpHistory;
    }

    float Downsample(float even, float odd)
    {
        PushDown(even);
        float output = 0.0f;

        for (int tap = 0; tap < kTaps; ++tap)
        {
            if (mTaps[tap] != 0.0f)
            {
                output += mTaps[tap] * mDownHistory[(mDownWrite + kTaps - 1 - tap) % kTaps];
            }
        }

        PushDown(odd);
        return output;
    }

  private:
    static constexpr int kUpHistory = (kTaps + 1) / 2;

    void PushDown(float sample)
    {
        mDownHistory[mDownWrite] = sample;
        mDownWrite = (mDownWrite + 1) % kTaps;
    }

    std::array<float, kTaps> mTaps = {};
    std::array<float, kUpHistory> mUpHistory = {};
    std::array<float, kTaps> mDownHistory = {};
    int mUpWrite = 0;
    int mDownWrite = 0;
};
} // namespace guitarfx
