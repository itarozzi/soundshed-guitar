#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/FiniteCheck.h"

#include <array>
#include <cstddef>

/**
 * A multi-band split for the multi-band dynamics effects (the Multi-band Gate and Compressor):
 * fourth-order Linkwitz-Riley crossovers in a cascade with all-pass compensation, so the bands
 * sum to a single all-pass of the input, flat in level, whenever every band is at unity.
 *
 * Why not a split by subtraction: it adds back exactly, but its bands leak at about 6 dB/octave,
 * so a 1 kHz note reads only 8 dB down in a band above 6 kHz. LR4 bands fall 24 dB/octave.
 */
namespace guitarfx::lr_crossover
{
/// What the crossover's channels share: per split, one Butterworth section of the Linkwitz-Riley
/// low-pass and high-pass (each runs it twice) and the all-pass the two add up to.
template <int kBands> struct Design
{
    static constexpr int kSplits = kBands - 1;

    std::array<BiquadCoefficients, kSplits> lowPass;
    std::array<BiquadCoefficients, kSplits> highPass;
    std::array<BiquadCoefficients, kSplits> allPass;

    /// `splitHz` low to high.
    void Prepare(const std::array<double, kSplits>& splitHz, double sampleRate)
    {
        for (std::size_t split = 0; split < kSplits; ++split)
        {
            PrepareSplit(static_cast<int>(split), splitHz[split], sampleRate);
        }
    }

    /// Redesigns one split, for a crossover that moved. The filters' state carries over.
    void PrepareSplit(int split, double hz, double sampleRate)
    {
        const auto index = static_cast<std::size_t>(split);
        lowPass[index] = biquad::LowPass(hz, biquad::kButterworthQ, sampleRate);
        highPass[index] = biquad::HighPass(hz, biquad::kButterworthQ, sampleRate);
        allPass[index] = biquad::AllPass(hz, biquad::kButterworthQ, sampleRate);
    }
};

/**
 * One channel of the split. Each crossover takes the lowest band off what the last left; a band
 * that leaves early skips the crossovers above it, so it runs through their all-passes instead.
 */
template <int kBands> class Crossover
{
  public:
    static constexpr int kSplits = kBands - 1;

    /// Low band first.
    void Split(const Design<kBands>& design, double input, std::array<double, kBands>& bands)
    {
        double rest = input;

        for (std::size_t split = 0; split < kSplits; ++split)
        {
            auto& low = mLowPass[split];
            auto& high = mHighPass[split];
            bands[split] = low[1].Process(design.lowPass[split], low[0].Process(design.lowPass[split], rest));
            rest = high[1].Process(design.highPass[split], high[0].Process(design.highPass[split], rest));
        }

        bands[kSplits] = rest;
        std::size_t allPass = 0;

        for (std::size_t band = 0; band + 1 < kSplits; ++band)
        {
            for (std::size_t split = band + 1; split < kSplits; ++split)
            {
                bands[band] = mAllPass[allPass++].Process(design.allPass[split], bands[band]);
            }
        }
    }

    void Reset()
    {
        *this = Crossover{};
    }

    [[nodiscard]] bool IsFinite() const
    {
        for (const auto& sections : mLowPass)
        {
            if (!guitarfx::IsFinite(sections[1].s1) || !guitarfx::IsFinite(sections[1].s2))
            {
                return false;
            }
        }

        if constexpr (kAllPasses > 0)
        {
            if (!guitarfx::IsFinite(mAllPass[0].s1))
            {
                return false;
            }
        }

        return guitarfx::IsFinite(mHighPass[kSplits - 1][1].s1);
    }

  private:
    /// Band b needs the all-passes of every split above it: (kSplits - 1) + … + 1.
    static constexpr std::size_t kAllPasses = kSplits * (kSplits - 1) / 2;

    std::array<std::array<biquad::State, 2>, kSplits> mLowPass{};
    std::array<std::array<biquad::State, 2>, kSplits> mHighPass{};
    std::array<biquad::State, kAllPasses> mAllPass{};
};
} // namespace guitarfx::lr_crossover
