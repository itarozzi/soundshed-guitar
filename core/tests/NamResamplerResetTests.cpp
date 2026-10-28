/**
 * @file NamResamplerResetTests.cpp
 * @brief What a NAM resampler reset has to do: leave nothing of the audio before it, and be
 *        where the resampler's debug switches are read.
 *
 * When the host stops audio, PluginProcessorAdapter::releaseResources() resets every effect, and
 * a prepareToPlay() with the same settings then keeps the NAM amp's resampler rather than
 * rebuilding it (OptimizedNAMAmpEffect::ResetModelsIfNeeded). So NamOversamplingProcessor::Reset()
 * is all that clears it, through ResamplingContainer::ClearBuffers(), and that missed the
 * fractional-ratio minimum-phase anti-alias filter's state: the amp played out the tail of the
 * audio from before the stop. Every filter here is linear, so silence after a complete reset
 * comes out exactly silent.
 *
 * The resampler's debug switches are environment variables, and it read them with std::getenv
 * on every block, which on Windows takes a lock on the audio thread. Reset() reads them now.
 *
 * Both fixes are in core/cmake/GuitarfxAudioDSPTools.cmake.
 *
 * The amp's own side of that skip has to notice a host rate change too, even when the model's rate
 * and block size come out unchanged.
 */

#include "dsp/effects/NAMOversampling.h"
#include "dsp/effects/OptimizedNAMAmpEffect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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

/// Sets an environment variable, or removes it when value is null.
void SetTestEnvironment(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else

    if (value != nullptr)
    {
        setenv(name, value, 1);
    }
    else
    {
        unsetenv(name);
    }

#endif
}

enum class SwitchTiming
{
    Unset,
    BeforePrepare,
    AfterPrepare,
    AfterPrepareThenReset,
};

struct SwitchCase
{
    const char* label;
    double hostRate;
    const char* baseName; // Set before Prepare() in every render of the case, or null.
    const char* name;
    const char* value;
    bool takesEffectAtReset; // A flag; the filter overrides only apply when the filters are designed.
};

/// Renders broadband noise through a 48 kHz model's resampler with 2x and minimum phase, the
/// realtime IIR half-band path, with the case's switch set at the given point.
std::vector<NAM_SAMPLE> RenderWithSwitch(const SwitchCase& test, SwitchTiming timing)
{
    constexpr int kBlock = 256;
    constexpr int kBlocks = 8;

    if (test.baseName != nullptr)
    {
        SetTestEnvironment(test.baseName, "1");
    }

    if (timing == SwitchTiming::BeforePrepare)
    {
        SetTestEnvironment(test.name, test.value);
    }

    PassThroughNamDSP model;
    guitarfx::NamOversamplingProcessor processor;
    processor.Prepare(model, test.hostRate, 48000.0, kBlock, 2, guitarfx::NamAntiAliasPhaseFromIndex(0));

    if (timing == SwitchTiming::AfterPrepare || timing == SwitchTiming::AfterPrepareThenReset)
    {
        SetTestEnvironment(test.name, test.value);
    }

    if (timing == SwitchTiming::AfterPrepareThenReset)
    {
        processor.Reset(model);
    }

    std::vector<NAM_SAMPLE> input(static_cast<std::size_t>(kBlock));
    std::vector<NAM_SAMPLE> output(static_cast<std::size_t>(kBlock));
    std::vector<NAM_SAMPLE> rendered;
    rendered.reserve(static_cast<std::size_t>(kBlock * kBlocks));
    std::uint32_t noiseState = 0x12345678u;

    for (int block = 0; block < kBlocks; ++block)
    {
        for (NAM_SAMPLE& sample : input)
        {
            noiseState = noiseState * 1664525u + 1013904223u;
            sample = static_cast<NAM_SAMPLE>(0.25 * (static_cast<double>(noiseState >> 8) / 8388608.0 - 1.0));
        }

        processor.Process(model, input.data(), output.data(), kBlock);
        rendered.insert(rendered.end(), output.begin(), output.end());
    }

    SetTestEnvironment(test.name, nullptr);

    if (test.baseName != nullptr)
    {
        SetTestEnvironment(test.baseName, nullptr);
    }

    return rendered;
}

double LargestDifference(const std::vector<NAM_SAMPLE>& a, const std::vector<NAM_SAMPLE>& b)
{
    double largest = 0.0;

    // No NaN can reach here, so /fp:fast's compares are safe.
    for (std::size_t index = 0; index < a.size() && index < b.size(); ++index)
    {
        largest = std::max(largest, std::abs(static_cast<double>(a[index]) - static_cast<double>(b[index])));
    }

    return largest;
}

bool TestNamResamplerReadsSwitchesAtReset()
{
    constexpr SwitchCase kCases[] = {
        {"NAM_MINPHASE_IIR_CLEAN_FUSED, 48 kHz host", 48000.0, nullptr, "NAM_MINPHASE_IIR_CLEAN_FUSED", "1", true},
        {"NAM_MINPHASE_DOWN_IIR_ORDER with the fused decimator, 48 kHz host", 48000.0, "NAM_MINPHASE_IIR_CLEAN_FUSED",
         "NAM_MINPHASE_DOWN_IIR_ORDER", "4", false},
        {"NAM_MINPHASE_OUTPUT_IIR_ORDER, 96 kHz host", 96000.0, nullptr, "NAM_MINPHASE_OUTPUT_IIR_ORDER", "8", false},
        {"NAM_MINPHASE_OUTPUT_IIR_CUTOFF_BIAS, 96 kHz host", 96000.0, nullptr, "NAM_MINPHASE_OUTPUT_IIR_CUTOFF_BIAS",
         "0", false},
    };
    bool allPassed = true;

    for (const SwitchCase& test : kCases)
    {
        const auto unset = RenderWithSwitch(test, SwitchTiming::Unset);
        const auto beforePrepare = RenderWithSwitch(test, SwitchTiming::BeforePrepare);
        const auto afterPrepare = RenderWithSwitch(test, SwitchTiming::AfterPrepare);

        const double switchEffect = LargestDifference(unset, beforePrepare);
        const double midStreamEffect = LargestDifference(unset, afterPrepare);
        std::cout << test.label << ": set before prepare changes the output by " << switchEffect
                  << ", set mid-stream by " << midStreamEffect << "\n";

        if (!(switchEffect > 1.0e-4))
        {
            std::cerr << test.label << ": the switch set before the resampler was built did nothing\n";
            allPassed = false;
        }

        if (midStreamEffect != 0.0)
        {
            std::cerr << test.label << ": the switch was read mid-stream, not at a reset\n";
            allPassed = false;
        }

        if (test.takesEffectAtReset)
        {
            const auto beforeReset = RenderWithSwitch(test, SwitchTiming::AfterPrepareThenReset);
            const double resetDifference = LargestDifference(beforePrepare, beforeReset);
            std::cout << test.label << ": set before a reset differs from set before prepare by " << resetDifference
                      << "\n";

            if (resetDifference != 0.0)
            {
                std::cerr << test.label << ": the switch set before a reset did not take effect at it\n";
                allPassed = false;
            }
        }
    }

    return allPassed;
}

/// A host rate change kept the amp's old setup whenever the model's own rate and block size came
/// out the same, as they do at 1x for 48 kHz/480 to 96 kHz/960: the model ran without the
/// resampler 96 kHz needs, on blocks larger than it was prepared for, with a stale latency. Re-
/// prepared, the amp must match one prepared fresh at the new settings, sample for sample.
bool TestNamAmpReprepareAtANewHostRate()
{
    const auto model = std::filesystem::path(GUITARFX_TEST_RESOURCES_DIR) /
                       "assets/amps/Guitar/Emil Rohbe/VOX AC15 NAM PACK/VOX AC15 Capture 3 - Emil Rohbe.nam";
    constexpr double kRate = 96000.0;
    constexpr int kBlock = 960;
    guitarfx::OptimizedNAMAmpEffect moved;
    guitarfx::OptimizedNAMAmpEffect fresh;

    if (!moved.LoadResource(model) || !fresh.LoadResource(model))
    {
        std::cerr << "could not load " << model.string() << "\n";
        return false;
    }

    moved.Prepare(48000.0, 480);
    moved.Prepare(kRate, kBlock);
    fresh.Prepare(kRate, kBlock);

    bool passed = true;

    if (moved.GetLatencySamples() != fresh.GetLatencySamples())
    {
        std::cerr << "re-prepared at 96 kHz the amp reports " << moved.GetLatencySamples()
                  << " samples of latency, prepared fresh " << fresh.GetLatencySamples() << "\n";
        passed = false;
    }

    std::vector<float> in(kBlock), movedL(kBlock), movedR(kBlock), freshL(kBlock), freshR(kBlock);
    double largest = 0.0;

    for (int block = 0; block < 10; ++block)
    {
        for (int i = 0; i < kBlock; ++i)
        {
            in[i] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 220.0 * (block * kBlock + i) / kRate));
        }

        float* inputs[2] = {in.data(), in.data()};
        float* movedOut[2] = {movedL.data(), movedR.data()};
        float* freshOut[2] = {freshL.data(), freshR.data()};
        moved.Process(inputs, movedOut, kBlock);
        fresh.Process(inputs, freshOut, kBlock);

        for (int i = 0; i < kBlock; ++i)
        {
            largest = std::max({largest, std::abs(static_cast<double>(movedL[i]) - freshL[i]),
                                std::abs(static_cast<double>(movedR[i]) - freshR[i])});
        }
    }

    std::cout << "NAM amp re-prepared at 96 kHz/960 differs from a fresh one by " << largest << "\n";

    if (largest != 0.0)
    {
        std::cerr << "the re-prepared amp does not match one prepared fresh at 96 kHz\n";
        passed = false;
    }

    return passed;
}
} // namespace

int main()
{
    bool allPassed = true;

    if (!TestNamResamplerResetClearsFilterState())
    {
        std::cerr << "NAM resampler reset test failed\n";
        allPassed = false;
    }

    if (!TestNamResamplerReadsSwitchesAtReset())
    {
        std::cerr << "NAM resampler debug switch test failed\n";
        allPassed = false;
    }

    if (!TestNamAmpReprepareAtANewHostRate())
    {
        std::cerr << "NAM amp host rate change test failed\n";
        allPassed = false;
    }

    return allPassed ? 0 : 1;
}
