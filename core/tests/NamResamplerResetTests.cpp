/**
 * @file NamResamplerResetTests.cpp
 * @brief A NAM resampler reset with unchanged settings leaves nothing of the audio before it.
 *
 * When the host stops audio, PluginProcessorAdapter::releaseResources() resets every effect, and
 * a prepareToPlay() with the same settings then keeps the NAM amp's resampler rather than
 * rebuilding it (OptimizedNAMAmpEffect::ResetModelsIfNeeded). So NamOversamplingProcessor::Reset()
 * is all that clears it, through ResamplingContainer::ClearBuffers(), and that missed the
 * fractional-ratio minimum-phase anti-alias filter's state: the amp played out the tail of the
 * audio from before the stop. The fix is in core/cmake/GuitarfxAudioDSPTools.cmake.
 *
 * Every filter here is linear, so silence after a complete reset comes out exactly silent.
 */

#include "dsp/effects/NAMOversampling.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;

class PassThroughNamDSP final : public ::nam::DSP
{
  public:
    PassThroughNamDSP() : ::nam::DSP(1, 1, 48000.0)
    {
    }

    void Reset(double, int) override
    {
    }

    void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int numFrames) override
    {
        std::copy_n(input[0], numFrames, output[0]);
    }
};

bool TestNamResamplerResetClearsFilterState()
{
    struct Case
    {
        const char* label;
        double hostRate;
        double modelRate;
        int factor;
        int phaseIndex;
    };

    constexpr Case kCases[] = {
        {"44.1 kHz model at 48 kHz, oversampling off, minimum phase", 48000.0, 44100.0, 1, 0},
        {"44.1 kHz model at 48 kHz, 2x, minimum phase", 48000.0, 44100.0, 2, 0},
        {"44.1 kHz model at 48 kHz, 2x, linear short", 48000.0, 44100.0, 2, 1},
        {"48 kHz model at 48 kHz, 2x, minimum phase", 48000.0, 48000.0, 2, 0},
        {"48 kHz model at 96 kHz, 2x, minimum phase", 96000.0, 48000.0, 2, 0},
        {"48 kHz model at 48 kHz, 2x, linear short", 48000.0, 48000.0, 2, 1},
    };
    constexpr int kBlock = 256;
    constexpr int kSignalBlocks = 8;
    constexpr int kSilentBlocks = 8;
    bool allPassed = true;

    for (const Case& test : kCases)
    {
        PassThroughNamDSP model;
        guitarfx::NamOversamplingProcessor processor;
        processor.Prepare(model, test.hostRate, test.modelRate, kBlock, test.factor,
                          guitarfx::NamAntiAliasPhaseFromIndex(test.phaseIndex));

        std::vector<NAM_SAMPLE> input(static_cast<std::size_t>(kBlock));
        std::vector<NAM_SAMPLE> output(static_cast<std::size_t>(kBlock));

        for (int block = 0; block < kSignalBlocks; ++block)
        {
            for (int index = 0; index < kBlock; ++index)
            {
                const int frame = block * kBlock + index;
                input[static_cast<std::size_t>(index)] =
                    static_cast<NAM_SAMPLE>(0.5 * std::sin(2.0 * kPi * 997.0 * frame / test.hostRate));
            }

            processor.Process(model, input.data(), output.data(), kBlock);
        }

        processor.Reset(model);
        std::fill(input.begin(), input.end(), static_cast<NAM_SAMPLE>(0.0));
        double largest = 0.0;

        for (int block = 0; block < kSilentBlocks; ++block)
        {
            processor.Process(model, input.data(), output.data(), kBlock);

            // No NaN can reach here, so /fp:fast's compares are safe.
            for (const NAM_SAMPLE sample : output)
            {
                largest = std::max(largest, std::abs(static_cast<double>(sample)));
            }
        }

        std::cout << test.label << ": largest sample after the reset " << largest << "\n";

        if (largest > 0.0)
        {
            std::cerr << test.label << ": the reset left audio from before it in the filters\n";
            allPassed = false;
        }
    }

    return allPassed;
}
} // namespace

int main()
{
    if (!TestNamResamplerResetClearsFilterState())
    {
        std::cerr << "NAM resampler reset test failed\n";
        return 1;
    }

    return 0;
}
