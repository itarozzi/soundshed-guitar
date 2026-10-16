#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace guitarfx
{
// Two cascaded 2x half-band stages. Each 65-tap Blackman FIR has an exact
// half-sample-band centre and an integer 32-sample round-trip delay at its
// input rate. The decimator takes the even phase, so the total latency is
// exactly 48 host samples at 4x or 32 host samples at 2x.
//
// Run polyphase, a block at a time. Every even tap but the centre is zero, so
// the even phase is a pure 16-sample delay and the odd phase a 32-tap FIR. Each
// direction keeps a line: the last 32 samples, then the block's own, so every
// sample a block's outputs need sits in a row. The FIR runs across outputs,
// each tap one multiply-add for a row of them, a vector's worth at once, and
// each output still adds its taps in order, so it is the dot product it was a
// sample at a time. One sample at a time, each output a dot product ending in
// a horizontal sum, read straight after its history was written, this was 45%
// of the amp's cost (docs/heavy-american-performance-review-2026-10-16.md).
class BuiltinAmpHalfband2x
{
  public:
    static constexpr int kTaps = 65;
    static constexpr int kCentre = (kTaps - 1) / 2;
    /// The most samples one call takes in (upsampling) or gives out (downsampling).
    static constexpr int kMaxBlock = 128;

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
        mLine.fill(0.0f);
        mEvenLine.fill(0.0f);
        mSums.fill(0.0f);
    }

    /// n samples in (null is silence), 2n out: each one's even phase, then its odd. n is at most
    /// kMaxBlock.
    void UpsampleBlock(const float* input, int n, float* output)
    {
        float* block = mLine.data() + kHistory;

        if (input)
        {
            std::copy(input, input + n, block);
        }
        else
        {
            std::fill(block, block + n, 0.0f);
        }

        OddPhase(mUpTaps, block, n);
        const float* delayed = block - kEvenDelay;

        for (int i = 0; i < n; ++i)
        {
            output[2 * i] = delayed[i];
            output[2 * i + 1] = mSums[i];
        }

        KeepHistory(mLine, kHistory, n);
    }

    /// 2n samples in, each one's even phase then its odd; n out, at most kMaxBlock.
    void DownsampleBlock(const float* input, int n, float* output)
    {
        float* odd = mLine.data() + kHistory;
        float* even = mEvenLine.data() + kEvenDelay;

        for (int i = 0; i < n; ++i)
        {
            even[i] = input[2 * i];
            odd[i] = input[2 * i + 1];
        }

        // The odd taps meet the odd-phase samples before each output's own,
        // and the centre tap the even sample 16 back.
        OddPhase(mDownTaps, odd - 1, n);
        const float* delayed = mEvenLine.data();
        const int whole = WholeGroups(n);

        for (int i = 0; i < whole; ++i)
        {
            mSums[i] = 0.5f * delayed[i] + mSums[i];
        }

        std::copy(mSums.data(), mSums.data() + n, output);
        KeepHistory(mLine, kHistory, n);
        KeepHistory(mEvenLine, kEvenDelay, n);
    }

  private:
    static constexpr int kOddTaps = (kTaps - 1) / 2;
    static constexpr int kEvenDelay = kCentre / 2;
    /// The samples kept from one block for the next: as many as the odd phase reaches back.
    static constexpr int kHistory = kOddTaps;

    /**
     * Outputs are worked out in whole groups, the ones past a block's end from stale samples, and
     * unused, so none falls to a loop's scalar remainder: one that fused its multiply-adds where
     * the vector lanes didn't, or the other way round, would make the output depend on the host's
     * block size. A group is a whole number of vectors at any width.
     */
    static constexpr int kGroup = 16;
    static_assert(kMaxBlock % kGroup == 0, "the lines hold whole groups");
    static_assert(kOddTaps % 8 == 0, "the odd phase runs eight taps a pass");

    [[nodiscard]] static int WholeGroups(int n)
    {
        return (n + kGroup - 1) / kGroup * kGroup;
    }

    using Taps = std::array<float, kOddTaps>;

    /// mSums[i] = the sum over lag of taps[lag] * newest[i - lag], lag 0 first, for the n
    /// outputs and the rest of their last group.
    ///
    /// Eight taps a pass over the outputs: one tap a pass loaded and stored every sum 32 times,
    /// and took 40% longer. A pass over a group of outputs held in registers was slower still,
    /// each sum a chain of 32 dependent multiply-adds.
    void OddPhase(const Taps& taps, const float* newest, int n)
    {
        const int whole = WholeGroups(n);
        float* sums = mSums.data();
        std::fill(sums, sums + whole, 0.0f);

        for (int lag = 0; lag < kOddTaps; lag += 8)
        {
            const float t0 = taps[lag], t1 = taps[lag + 1], t2 = taps[lag + 2], t3 = taps[lag + 3];
            const float t4 = taps[lag + 4], t5 = taps[lag + 5], t6 = taps[lag + 6], t7 = taps[lag + 7];
            const float* from = newest - lag;

            for (int i = 0; i < whole; ++i)
            {
                float sum = sums[i];
                sum += t0 * from[i];
                sum += t1 * from[i - 1];
                sum += t2 * from[i - 2];
                sum += t3 * from[i - 3];
                sum += t4 * from[i - 4];
                sum += t5 * from[i - 5];
                sum += t6 * from[i - 6];
                sum += t7 * from[i - 7];
                sums[i] = sum;
            }
        }
    }

    /// Moves the last `history` samples of the block just run to the front of the line.
    template <std::size_t Size> static void KeepHistory(std::array<float, Size>& line, int history, int n)
    {
        std::copy(line.begin() + n, line.begin() + n + history, line.begin());
    }

    Taps mUpTaps = {};
    Taps mDownTaps = {};
    alignas(32) std::array<float, kHistory + kMaxBlock> mLine = {};
    alignas(32) std::array<float, kEvenDelay + kMaxBlock> mEvenLine = {};
    alignas(32) std::array<float, kMaxBlock> mSums = {};
};
} // namespace guitarfx
