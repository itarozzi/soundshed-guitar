#include "dsp/Loudness.h"

#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace guitarfx::loudness
{
std::optional<double> GatedLoudness(const double* blockMeanSquares, std::size_t count) noexcept
{
    if (blockMeanSquares == nullptr || count == 0)
    {
        return std::nullopt;
    }

    // Compared as mean squares rather than as loudness, so no block costs a logarithm.
    const double absoluteGate = LufsToMeanSquare(kAbsoluteGateLufs);
    double absoluteSum = 0.0;
    std::size_t absoluteCount = 0;

    for (std::size_t i = 0; i < count; ++i)
    {
        const double meanSquare = blockMeanSquares[i];

        if (IsFinite(meanSquare) && meanSquare > absoluteGate)
        {
            absoluteSum += meanSquare;
            ++absoluteCount;
        }
    }

    if (absoluteCount == 0)
    {
        return std::nullopt;
    }

    // 10 LU below the loudness of their mean is a tenth of the mean.
    const double relativeGate =
        (absoluteSum / static_cast<double>(absoluteCount)) * std::pow(10.0, kRelativeGateLu / 10.0);
    double gatedSum = 0.0;
    std::size_t gatedCount = 0;

    for (std::size_t i = 0; i < count; ++i)
    {
        const double meanSquare = blockMeanSquares[i];

        if (IsFinite(meanSquare) && meanSquare > absoluteGate && meanSquare > relativeGate)
        {
            gatedSum += meanSquare;
            ++gatedCount;
        }
    }

    if (gatedCount == 0)
    {
        return std::nullopt;
    }

    return MeanSquareToLufs(gatedSum / static_cast<double>(gatedCount));
}

IntegratedLoudnessMeter::IntegratedLoudnessMeter(double sampleRate)
    : mSampleRate(sampleRate > 0.0 ? sampleRate : 48000.0),
      mBlockFrames(std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(mSampleRate * kBlockSeconds)))),
      mHopFrames(std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(mSampleRate * kHopSeconds)))),
      mGranuleFrames(std::gcd(mBlockFrames, mHopFrames))
{
    mLeftFilter.Prepare(mSampleRate);
    mRightFilter.Prepare(mSampleRate);
}

void IntegratedLoudnessMeter::Push(const float* left, const float* right, std::size_t frames)
{
    if (left == nullptr)
    {
        return;
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        const float l = left[i];
        const float r = right != nullptr ? right[i] : l;
        const double weightedLeft = mLeftFilter.Process(static_cast<double>(l));
        // A mono signal is the same signal twice: one filter, its energy counted for both sides.
        const double weightedRight = right != nullptr ? mRightFilter.Process(static_cast<double>(r)) : weightedLeft;

        mPendingEnergy += (weightedLeft * weightedLeft) + (weightedRight * weightedRight);
        mSumSquares +=
            static_cast<double>(l) * static_cast<double>(l) + static_cast<double>(r) * static_cast<double>(r);
        mPeak = std::max({mPeak, std::abs(l), std::abs(r)});

        if (++mPendingFrames == mGranuleFrames)
        {
            mGranuleEnergy.push_back(mPendingEnergy);
            mPendingEnergy = 0.0;
            mPendingFrames = 0;
        }
    }

    mFrames += frames;
}

LevelMeasurement IntegratedLoudnessMeter::Result() const
{
    LevelMeasurement result;
    result.frames = mFrames;
    result.sampleRate = mSampleRate;

    if (mFrames == 0)
    {
        return result;
    }

    // A block starts every hop and spans block/granule granules. Only whole blocks count: a
    // block ending past the last whole granule would end past the signal.
    const std::size_t granulesPerBlock = mBlockFrames / mGranuleFrames;
    const std::size_t granulesPerHop = mHopFrames / mGranuleFrames;
    std::vector<double> prefix(mGranuleEnergy.size() + 1, 0.0);

    for (std::size_t i = 0; i < mGranuleEnergy.size(); ++i)
    {
        prefix[i + 1] = prefix[i] + mGranuleEnergy[i];
    }

    std::vector<double> blocks;

    for (std::size_t start = 0; start + granulesPerBlock <= mGranuleEnergy.size(); start += granulesPerHop)
    {
        blocks.push_back((prefix[start + granulesPerBlock] - prefix[start]) / static_cast<double>(mBlockFrames));
    }

    result.integratedLufs = GatedLoudness(blocks.data(), blocks.size());

    if (mPeak > 0.0f && IsFinite(mSumSquares))
    {
        result.peakDbfs = 20.0 * std::log10(static_cast<double>(mPeak));
        const double meanSquare = mSumSquares / (2.0 * static_cast<double>(mFrames));

        if (meanSquare > 0.0)
        {
            result.rmsDbfs = 10.0 * std::log10(meanSquare);
        }
    }

    return result;
}

LevelMeasurement MeasureIntegratedLoudness(const float* left, const float* right, std::size_t frames, double sampleRate)
{
    IntegratedLoudnessMeter meter(sampleRate);
    meter.Push(left, right, frames);
    return meter.Result();
}
} // namespace guitarfx::loudness
