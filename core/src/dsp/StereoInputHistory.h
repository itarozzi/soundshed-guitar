#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace guitarfx
{
/**
 * The recent stereo input of a time-domain pitch shifter: a power-of-two ring the shifter's taps
 * read at fractional delays. Nothing allocates after Prepare().
 */
class StereoInputHistory
{
  public:
    /// Holds at least `minSamples`, rounded up to a power of two.
    void Prepare(std::size_t minSamples)
    {
        std::size_t capacity = 1;

        while (capacity < minSamples)
        {
            capacity <<= 1;
        }

        mMask = capacity - 1;
        mLeft.assign(capacity, 0.0f);
        mRight.assign(capacity, 0.0f);
        mWritten = 0;
    }

    void Reset() noexcept
    {
        std::fill(mLeft.begin(), mLeft.end(), 0.0f);
        std::fill(mRight.begin(), mRight.end(), 0.0f);
        mWritten = 0;
    }

    [[nodiscard]] std::size_t Capacity() const noexcept
    {
        return mLeft.size();
    }

    /// Samples written since the last Reset().
    [[nodiscard]] std::int64_t Written() const noexcept
    {
        return mWritten;
    }

    void Write(float left, float right) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(mWritten) & mMask;
        mLeft[index] = left;
        mRight[index] = right;
        ++mWritten;
    }

    /// The input from `delaySamples` ago: a dry path aligned with the nominal latency.
    void ReadDelayed(int delaySamples, float& left, float& right) const noexcept
    {
        const auto delay = static_cast<std::int64_t>(std::clamp(delaySamples, 0, static_cast<int>(mMask)));
        const std::size_t index = static_cast<std::size_t>(mWritten - 1 - delay) & mMask;
        left = mLeft[index];
        right = mRight[index];
    }

    /// The input `delay` samples ago, between samples by Catmull-Rom. Reads one sample either side
    /// of the two it lands between, so `delay` must be at least 2 and leave 2 samples of history.
    void Read(double delay, float& left, float& right) const noexcept
    {
        const auto whole = static_cast<std::int64_t>(delay);
        const auto t = static_cast<float>(delay - static_cast<double>(whole));
        // Newest to oldest around the read point: delays whole-1, whole, whole+1, whole+2.
        const std::int64_t newest = mWritten - 1 - whole;
        const std::size_t i0 = static_cast<std::size_t>(newest + 1) & mMask;
        const std::size_t i1 = static_cast<std::size_t>(newest) & mMask;
        const std::size_t i2 = static_cast<std::size_t>(newest - 1) & mMask;
        const std::size_t i3 = static_cast<std::size_t>(newest - 2) & mMask;
        left = Hermite(mLeft[i0], mLeft[i1], mLeft[i2], mLeft[i3], t);
        right = Hermite(mRight[i0], mRight[i1], mRight[i2], mRight[i3], t);
    }

  private:
    /// Catmull-Rom between y0 (t = 0) and y1 (t = 1).
    [[nodiscard]] static float Hermite(float ym1, float y0, float y1, float y2, float t) noexcept
    {
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + y0;
    }

    std::vector<float> mLeft;
    std::vector<float> mRight;
    std::size_t mMask = 0;
    std::int64_t mWritten = 0;
};
} // namespace guitarfx
