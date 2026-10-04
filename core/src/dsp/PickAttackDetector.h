#pragma once

#include "dsp/BiquadDesign.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace guitarfx
{
/**
 * Spots pick attacks in a guitar or bass signal, one sample at a time, with no look-ahead.
 *
 * A pick throws broadband energy that a ringing string does not, so the detector watches the
 * signal's power above a high-pass and asks two things of it: that it rises well above its
 * recent quietest level, and that it also beats its recent loudest. The second test is what
 * separates an attack from the steady pulse of a low note (every cycle of a low string has a
 * corner that a high-pass turns into a small spike, each no louder than the last) and from two
 * strings beating against each other, whose high band swells and fades by a few dB without ever
 * setting a new peak.
 *
 * The history is kept as the loudest and quietest level in each millisecond, over the last
 * kHistorySeconds, skipping the most recent kSkipSeconds so a rising attack is never compared
 * with itself. After a detection the history is set to the attack's level, so the same attack
 * cannot fire twice, and kRefractorySeconds must pass before another can.
 */
class PickAttackDetector
{
  public:
    static constexpr double kHighPassHz = 1500.0;
    static constexpr double kFastSeconds = 0.001;
    static constexpr double kRefractorySeconds = 0.035;
    static constexpr double kSkipSeconds = 0.003;
    static constexpr double kHistorySeconds = 0.045;
    /// Rise over the recent quietest level (power ratio; 8 is 9 dB).
    static constexpr double kOverFloor = 8.0;
    /// Rise over the recent loudest level (2.5 is 4 dB).
    static constexpr double kOverCeiling = 2.5;
    /// A rise this sudden (power ratio over the last one to two milliseconds; 30 is 15 dB) only
    /// has to beat the recent loudest level by kOverCeilingSudden: a quiet pick straight after a
    /// louder note that was damped. Strings beating swell far more slowly than that.
    static constexpr double kSuddenRise = 30.0;
    static constexpr double kOverCeilingSudden = 1.6;
    /// Nothing quieter than this counts (power; about -70 dBFS in the high band).
    static constexpr double kSilence = 1.0e-7;

    void Prepare(double sampleRate)
    {
        mHighPass = biquad::HighPass(kHighPassHz, biquad::kButterworthQ, sampleRate);
        mFastCoefficient = std::exp(-1.0 / (kFastSeconds * sampleRate));
        mCellLength = std::max(1, static_cast<int>(std::lround(sampleRate * 0.001)));
        mSkipCells = std::max(1, static_cast<int>(std::lround(kSkipSeconds * 1000.0)));
        mHistoryCells = std::clamp(static_cast<int>(std::lround(kHistorySeconds * 1000.0)), mSkipCells + 1, kMaxCells);
        mRefractory = static_cast<int>(std::lround(kRefractorySeconds * sampleRate));
        Reset();
    }

    void Reset()
    {
        mState.Reset();
        mLevel = 0.0;
        mCellMax.fill(0.0);
        mCellMin.fill(0.0);
        mRunningMax = 0.0;
        mRunningMin = 0.0;
        mCellPosition = 0;
        mCell = 0;
        mCeiling = 0.0;
        mFloor = 0.0;
        mSinceAttack = mRefractory;
    }

    /// Takes one sample; true on the sample an attack is detected.
    bool Process(float sample) noexcept
    {
        const double high = mState.Process(mHighPass, static_cast<double>(sample));
        mLevel = mFastCoefficient * mLevel + (1.0 - mFastCoefficient) * high * high;

        mRunningMax = std::max(mRunningMax, mLevel);
        mRunningMin = mCellPosition == 0 ? mLevel : std::min(mRunningMin, mLevel);

        if (++mCellPosition == mCellLength)
        {
            mCellMax[static_cast<std::size_t>(mCell)] = mRunningMax;
            mCellMin[static_cast<std::size_t>(mCell)] = mRunningMin;
            mCell = (mCell + 1) % mHistoryCells;
            mCellPosition = 0;
            mRunningMax = 0.0;
            UpdateReference();
        }

        if (mSinceAttack < mRefractory)
        {
            ++mSinceAttack;
            return false;
        }

        if (mLevel < kSilence)
        {
            return false;
        }

        const double previousCellMin = mCellMin[static_cast<std::size_t>((mCell - 1 + mHistoryCells) % mHistoryCells)];
        const bool sudden = mLevel > kSuddenRise * std::min(mRunningMin, previousCellMin);
        const bool newPeak = mLevel > (sudden ? kOverCeilingSudden : kOverCeiling) * mCeiling;

        if (mLevel > kOverFloor * mFloor && newPeak)
        {
            mSinceAttack = 0;
            mCellMax.fill(mLevel);
            mCellMin.fill(mLevel);
            mCeiling = mLevel;
            mFloor = mLevel;
            return true;
        }

        return false;
    }

  private:
    static constexpr int kMaxCells = 64;

    /// The loudest and quietest level in the cells older than the skip, which only change when
    /// a cell is finished. The newest finished cell is mCell - 1.
    void UpdateReference() noexcept
    {
        mCeiling = 0.0;
        mFloor = 1.0e30;

        for (int back = mSkipCells; back < mHistoryCells; ++back)
        {
            const auto index = static_cast<std::size_t>((mCell - back + 2 * mHistoryCells) % mHistoryCells);
            mCeiling = std::max(mCeiling, mCellMax[index]);
            mFloor = std::min(mFloor, mCellMin[index]);
        }
    }

    BiquadCoefficients mHighPass;
    biquad::State mState;
    double mFastCoefficient = 0.0;
    double mLevel = 0.0;
    std::array<double, kMaxCells> mCellMax{};
    std::array<double, kMaxCells> mCellMin{};
    double mRunningMax = 0.0;
    double mRunningMin = 0.0;
    double mCeiling = 0.0;
    double mFloor = 0.0;
    int mCellLength = 48;
    int mCellPosition = 0;
    int mCell = 0;
    int mSkipCells = 3;
    int mHistoryCells = 45;
    int mRefractory = 1680;
    int mSinceAttack = 0;
};
} // namespace guitarfx
