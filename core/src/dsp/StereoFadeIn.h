#pragma once

#include <algorithm>

namespace guitarfx
{
/// Brings in a right channel whose state cannot be trusted yet: one that sat idle while its path
/// ran mono, or a NAM model just built. For the hold the right side copies the left, while whatever
/// makes it warms up on its own input; then it crosses over to its own output along a linear ramp.
/// Without it the right side starts from stale or empty state, which is heard as a click on that
/// side. Audio thread; no allocation.
class StereoFadeIn
{
  public:
    /// The hold outlasts the longest memory a node has: a NAM model forgets everything older than
    /// its receptive field, about 85 ms for a standard model and 170 ms for the largest.
    static constexpr double kHoldSeconds = 0.17;
    static constexpr double kRampSeconds = 0.03;

    void Start(double sampleRate) noexcept
    {
        mHold = std::max(0, static_cast<int>(sampleRate * kHoldSeconds));
        mRamp = std::max(1, static_cast<int>(sampleRate * kRampSeconds));
        mPosition = 0;
    }

    void Stop() noexcept
    {
        mPosition = -1;
    }

    [[nodiscard]] bool IsActive() const noexcept
    {
        return mPosition >= 0;
    }

    /// Rewrites `right` in place from `left` for as much of the fade as falls in this block.
    template <typename Sample> void Apply(const Sample* left, Sample* right, int numSamples) noexcept
    {
        if (mPosition < 0 || !left || !right)
        {
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            if (mPosition < mHold)
            {
                right[i] = left[i];
            }
            else
            {
                const int intoRamp = mPosition - mHold;

                if (intoRamp >= mRamp)
                {
                    mPosition = -1;
                    return;
                }

                const auto own = static_cast<Sample>(intoRamp) / static_cast<Sample>(mRamp);
                right[i] = left[i] + own * (right[i] - left[i]);
            }

            ++mPosition;
        }
    }

  private:
    int mHold = 0;
    int mRamp = 1;
    int mPosition = -1;
};
} // namespace guitarfx
