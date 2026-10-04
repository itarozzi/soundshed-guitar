#pragma once

#include "dsp/BiquadDesign.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace guitarfx
{
/**
 * The mono history a waveform-matching pitch shifter searches for its splices.
 *
 * It keeps the input twice: at the full rate, and low-passed and decimated to an analysis rate,
 * where a search over a whole window of candidates costs a fraction as much. Each copy is
 * mirrored (every sample written twice, one ring length apart), so the window ending at any
 * position is one contiguous run of memory and a correlation over it is a plain loop that
 * vectorises.
 *
 * Positions are absolute sample counts: the first sample written is position 0. A position stays
 * readable for the ring's capacity, less a window.
 */
class WaveformHistory
{
  public:
    /// `capacity` full-rate samples (a power of two). Coarse windows are `coarseSeconds` long at
    /// the analysis rate, fine ones `fineSeconds` at the full rate.
    void Prepare(double sampleRate, std::size_t capacity, double analysisRateHz, double coarseSeconds,
                 double fineSeconds)
    {
        mDecimation = std::max(1, static_cast<int>(std::lround(sampleRate / analysisRateHz)));
        mCapacity = capacity;
        mMask = capacity - 1;
        mCoarseLength = std::max(8, static_cast<int>(std::lround(coarseSeconds * sampleRate)) / mDecimation);
        mFineLength = std::max(8, static_cast<int>(std::lround(fineSeconds * sampleRate)));
        mFine.assign(2 * capacity, 0.0f);
        mCoarseCapacity = capacity / static_cast<std::size_t>(mDecimation) + 1;
        mCoarse.assign(2 * mCoarseCapacity, 0.0f);
        mAntiAlias = biquad::LowPass(0.4 * sampleRate / mDecimation, biquad::kButterworthQ, sampleRate);
        Reset();
    }

    void Reset()
    {
        std::fill(mFine.begin(), mFine.end(), 0.0f);
        std::fill(mCoarse.begin(), mCoarse.end(), 0.0f);
        mAntiAliasState.Reset();
        mWritten = 0;
        mCoarseWritten = 0;
        mPhase = 0;
    }

    void Write(float sample) noexcept
    {
        const std::size_t index = static_cast<std::size_t>(mWritten) & mMask;
        mFine[index] = sample;
        mFine[index + mCapacity] = sample;
        ++mWritten;

        const auto filtered = static_cast<float>(mAntiAliasState.Process(mAntiAlias, sample));

        if (++mPhase == mDecimation)
        {
            const std::size_t coarse = static_cast<std::size_t>(mCoarseWritten) % mCoarseCapacity;
            mCoarse[coarse] = filtered;
            mCoarse[coarse + mCoarseCapacity] = filtered;
            ++mCoarseWritten;
            mPhase = 0;
        }
    }

    [[nodiscard]] int Decimation() const noexcept
    {
        return mDecimation;
    }

    [[nodiscard]] int CoarseLength() const noexcept
    {
        return mCoarseLength;
    }

    [[nodiscard]] int FineLength() const noexcept
    {
        return mFineLength;
    }

    /// The position of the newest sample written.
    [[nodiscard]] std::int64_t Newest() const noexcept
    {
        return static_cast<std::int64_t>(mWritten) - 1;
    }

    /// The newest position a coarse window may end at: its decimated sample is complete.
    [[nodiscard]] std::int64_t NewestCoarse() const noexcept
    {
        return static_cast<std::int64_t>(mCoarseWritten) * mDecimation - 1 - mDecimation;
    }

    /// CoarseLength() decimated samples, ending at the one that covers `position`.
    [[nodiscard]] const float* Coarse(std::int64_t position) const noexcept
    {
        const std::int64_t index = std::max<std::int64_t>(0, position / mDecimation);
        const std::size_t end = static_cast<std::size_t>(index) % mCoarseCapacity + mCoarseCapacity;
        return mCoarse.data() + (end + 1 - static_cast<std::size_t>(mCoarseLength));
    }

    /// FineLength() full-rate samples ending at `position`.
    [[nodiscard]] const float* Fine(std::int64_t position) const noexcept
    {
        const std::size_t end = (static_cast<std::size_t>(std::max<std::int64_t>(0, position)) & mMask) + mCapacity;
        return mFine.data() + (end + 1 - static_cast<std::size_t>(mFineLength));
    }

    [[nodiscard]] static double Energy(const float* a, int length) noexcept
    {
        float sum[8] = {};
        int i = 0;

        for (; i + 8 <= length; i += 8)
        {
            for (int k = 0; k < 8; ++k)
            {
                sum[k] += a[i + k] * a[i + k];
            }
        }

        double total = 0.0;

        for (; i < length; ++i)
        {
            total += static_cast<double>(a[i]) * a[i];
        }

        for (const float s : sum)
        {
            total += s;
        }

        return total;
    }

    /// Normalised cross-correlation of `a` (whose energy is given) with `b`; 0 for silence. Eight
    /// running sums, so the loop vectorises even where float sums may not be reassociated.
    [[nodiscard]] static double Correlate(const float* a, double aEnergy, const float* b, int length) noexcept
    {
        float ab[8] = {};
        float bb[8] = {};
        int i = 0;

        for (; i + 8 <= length; i += 8)
        {
            for (int k = 0; k < 8; ++k)
            {
                ab[k] += a[i + k] * b[i + k];
                bb[k] += b[i + k] * b[i + k];
            }
        }

        double cross = 0.0;
        double energy = 0.0;

        for (; i < length; ++i)
        {
            cross += static_cast<double>(a[i]) * b[i];
            energy += static_cast<double>(b[i]) * b[i];
        }

        for (int k = 0; k < 8; ++k)
        {
            cross += ab[k];
            energy += bb[k];
        }

        const double product = aEnergy * energy;
        return product > 1.0e-18 ? cross / std::sqrt(product) : 0.0;
    }

  private:
    int mDecimation = 2;
    std::size_t mCapacity = 0;
    std::size_t mMask = 0;
    std::size_t mCoarseCapacity = 0;
    int mCoarseLength = 300;
    int mFineLength = 1200;
    std::vector<float> mFine;
    std::vector<float> mCoarse;
    BiquadCoefficients mAntiAlias;
    biquad::State mAntiAliasState;
    std::uint64_t mWritten = 0;
    std::uint64_t mCoarseWritten = 0;
    int mPhase = 0;
};
} // namespace guitarfx
