#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace guitarfx
{
/**
 * Delays an effect's dry signal by its wet path's latency, so a Mix below 100% lines the two up
 * and the latency the effect reports is what both paths do.
 *
 * The IR Cabinet and Convolution Reverb convolve with no latency when Low Latency is on, and a
 * partition late, 256 to 2048 samples, when it is off (RealtimeConvolver::GetLatency). Their dry
 * signal used to pass straight through either way, so with Low Latency off a host compensating
 * for the reported latency pulled the dry guitar early, and below 100% Mix it combed against the
 * late wet one. The effect hands this its latency every block, so the dry path follows it through
 * whatever changes it: an IR load, a Low Latency rebuild, a new block size.
 *
 * The history is sized at Prepare for the most delay asked of it, so nothing here allocates on the
 * audio thread. A new delay is reached by a linear crossfade from the old tap to the new one
 * rather than a jump, which would click; the first block after Prepare or Reset has no history to
 * fade from and takes its delay at once. At a delay of 0 the output is the input, exactly.
 */
class DryPathDelay
{
  public:
    /// `maxDelaySamples` bounds what Process can be asked for; more is clamped to it.
    void Prepare(int maxDelaySamples, int maxBlockSize, int crossfadeSamples)
    {
        mMaxDelay = std::max(0, maxDelaySamples);
        mMaxBlockSize = std::max(1, maxBlockSize);
        mCrossfadeSamples = std::max(1, crossfadeSamples);

        const std::size_t needed = static_cast<std::size_t>(mMaxDelay) + static_cast<std::size_t>(mMaxBlockSize) + 1;
        std::size_t size = 1;

        while (size < needed)
        {
            size <<= 1;
        }

        for (int channel = 0; channel < 2; ++channel)
        {
            mHistory[channel].assign(size, 0.0f);
            mOutput[channel].assign(static_cast<std::size_t>(mMaxBlockSize), 0.0f);
        }

        mHistoryMask = size - 1;
        Reset();
    }

    /// Clears the history. The next Process takes its delay at once, with nothing to fade from.
    void Reset()
    {
        for (auto& channel : mHistory)
        {
            std::fill(channel.begin(), channel.end(), 0.0f);
        }

        mWritePosition = 0;
        mFadeRemaining = 0;
        mSnapToNextDelay = true;
    }

    /// Records this block's input and works out the dry signal `delaySamples` behind it, read back
    /// with Output(). `numSamples` must not exceed the block size given to Prepare; the inputs may
    /// be overwritten once this returns.
    void Process(const float* inputL, const float* inputR, int numSamples, int delaySamples)
    {
        if (mHistory[0].empty() || numSamples <= 0)
        {
            return; // not prepared
        }

        numSamples = std::min(numSamples, mMaxBlockSize);
        const int target = std::clamp(delaySamples, 0, mMaxDelay);

        if (mSnapToNextDelay)
        {
            mDelay = target;
            mSnapToNextDelay = false;
        }
        else if (target != mDelay)
        {
            // A change mid-fade restarts from the delay that fade was heading for.
            mPreviousDelay = mDelay;
            mDelay = target;
            mFadeRemaining = mCrossfadeSamples;
        }

        const std::array<const float*, 2> input = {inputL, inputR};
        const std::size_t blockStart = mWritePosition;

        for (int channel = 0; channel < 2; ++channel)
        {
            auto& history = mHistory[channel];

            for (int i = 0; i < numSamples; ++i)
            {
                history[(blockStart + static_cast<std::size_t>(i)) & mHistoryMask] = input[channel][i];
            }
        }

        mWritePosition = (blockStart + static_cast<std::size_t>(numSamples)) & mHistoryMask;

        if (mFadeRemaining <= 0)
        {
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto& history = mHistory[channel];
                float* output = mOutput[channel].data();

                for (int i = 0; i < numSamples; ++i)
                {
                    output[i] = history[(blockStart + static_cast<std::size_t>(i) - static_cast<std::size_t>(mDelay)) &
                                        mHistoryMask];
                }
            }

            return;
        }

        const int fadeStart = mFadeRemaining;

        for (int channel = 0; channel < 2; ++channel)
        {
            const auto& history = mHistory[channel];
            float* output = mOutput[channel].data();
            int remaining = fadeStart;

            for (int i = 0; i < numSamples; ++i)
            {
                const std::size_t position = blockStart + static_cast<std::size_t>(i);
                const float now = history[(position - static_cast<std::size_t>(mDelay)) & mHistoryMask];

                if (remaining <= 0)
                {
                    output[i] = now;
                    continue;
                }

                const float before = history[(position - static_cast<std::size_t>(mPreviousDelay)) & mHistoryMask];
                const float weight = static_cast<float>(remaining) / static_cast<float>(mCrossfadeSamples);
                output[i] = now + weight * (before - now);
                --remaining;
            }
        }

        mFadeRemaining = std::max(0, fadeStart - numSamples);
    }

    /// The dry signal on channel 0 (left) or 1 (right) for the block Process last saw.
    [[nodiscard]] const float* Output(int channel) const
    {
        return mOutput[static_cast<std::size_t>(channel)].data();
    }

    /// The delay the dry signal is heading for, or at once it has faded there.
    [[nodiscard]] int GetDelay() const
    {
        return mDelay;
    }

  private:
    int mMaxDelay = 0;
    int mMaxBlockSize = 0;
    int mCrossfadeSamples = 1;
    int mDelay = 0;
    int mPreviousDelay = 0;
    int mFadeRemaining = 0;
    bool mSnapToNextDelay = true;

    std::array<std::vector<float>, 2> mHistory;
    std::size_t mHistoryMask = 0;
    std::size_t mWritePosition = 0;
    std::array<std::vector<float>, 2> mOutput;
};
} // namespace guitarfx
