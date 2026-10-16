/**
 * IrDryPathDelayTests.cpp — the IR Cabinet's and Convolution Reverb's dry signal is as late as the
 * latency they report.
 *
 * With Low Latency off both convolve a uniform partition late, 256 to 2048 samples, and report it.
 * Their dry signal used to pass straight through, so a host compensating for the reported latency
 * pulled the dry guitar early, and below 100% Mix the dry combed against the late wet. Now
 * DryPathDelay delays the dry by whatever the effect reports.
 *
 * - DryPathDelay itself: exact at 0, exact at a delay across block boundaries, a new delay reached
 *   by a crossfade with no jump, and a delay taken at once after Reset.
 * - An impulse through each effect at Mix 0.5, with an IR that is a single spike 300 samples in:
 *   the dry spike lands at the reported latency and the wet one 300 samples after it, with Low
 *   Latency on (latency 0) and off (the partition), at several host block sizes.
 * - Low Latency switched while playing, at Mix 0 (the dry alone): the dry moves to the new latency
 *   without a click, and ends exactly that late.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dsp/DryPathDelay.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/RealtimeConvolver.h"
#include "dsp/effects/BuiltinEffects.h"

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kSpikeAt = 300; // where the test IR's only non-zero sample sits

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

/// This run's own folder, so two runs at once cannot delete each other's IRs.
const fs::path& TempDir()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("guitarfx_ir_dry_path_delay_tests_" + std::to_string(std::random_device{}()));
    return dir;
}

fs::path WriteMonoWav(const std::string& name, const std::vector<float>& samples)
{
    fs::create_directories(TempDir());
    const auto path = TempDir() / name;
    const auto dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(float));
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t format = 3; // IEEE float
    const std::uint16_t channels = 1;
    const auto rate = static_cast<std::uint32_t>(kSampleRate);
    const std::uint32_t byteRate = rate * sizeof(float);
    const std::uint16_t blockAlign = sizeof(float);
    const std::uint16_t bitsPerSample = 32;

    std::ofstream file(path, std::ios::binary);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    file.write(reinterpret_cast<const char*>(&format), 2);
    file.write(reinterpret_cast<const char*>(&channels), 2);
    file.write(reinterpret_cast<const char*>(&rate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
    file.write(reinterpret_cast<const char*>(samples.data()), dataSize);
    return path;
}

/// 2000 samples, silent but for one spike: long enough for the FFT engines in both modes.
const fs::path& SpikeIR()
{
    static const fs::path path = [] {
        std::vector<float> ir(2000, 0.0f);
        ir[kSpikeAt] = 1.0f;
        return WriteMonoWav("spike.wav", ir);
    }();
    return path;
}

/// Runs `input` through the effect, the same on both channels, in blocks of `hostBlock`.
void Render(EffectProcessor& effect, const std::vector<float>& input, std::vector<float>& outL,
            std::vector<float>& outR, int hostBlock)
{
    outL.assign(input.size(), 0.0f);
    outR.assign(input.size(), 0.0f);
    std::vector<float> inL(input);
    std::vector<float> inR(input);

    for (std::size_t start = 0; start < input.size(); start += static_cast<std::size_t>(hostBlock))
    {
        const int n =
            static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(hostBlock), input.size() - start));
        float* inputs[2] = {inL.data() + start, inR.data() + start};
        float* outputs[2] = {outL.data() + start, outR.data() + start};
        effect.Process(inputs, outputs, n);
    }
}

/// The message thread's part, as PluginController::ApplyDeferredNodeRebuilds does it but with no
/// DSP lock, since nothing else runs here.
bool ApplyDeferredRebuild(EffectProcessor& effect)
{
    auto work = effect.TakeDeferredRebuild();

    if (!work)
    {
        return false;
    }

    work->Build();
    effect.CommitDeferredRebuild(*work);
    return true;
}

/// A node with the spike IR, set up the way a preset's is: params, then the IR, then Prepare.
std::unique_ptr<EffectProcessor> MakeEffect(const std::string& type, double mix, bool lowLatency, int maxBlock)
{
    auto effect = EffectRegistry::Instance().Create(type);
    effect->SetParam("mix", mix);
    effect->SetParam("lowLatency", lowLatency ? 1.0 : 0.0);

    if (!effect->LoadResource(SpikeIR()))
    {
        return nullptr;
    }

    effect->Prepare(kSampleRate, maxBlock);
    return effect;
}

// --- DryPathDelay ----------------------------------------------------------------------------

std::vector<float> Ramp(std::size_t length)
{
    std::vector<float> ramp(length);

    for (std::size_t i = 0; i < length; ++i)
    {
        ramp[i] = static_cast<float>(i + 1);
    }

    return ramp;
}

/// Runs a whole signal through the helper in blocks that keep changing size, at `delayFor(start)`.
template <typename DelayFor>
std::vector<float> RunHelper(DryPathDelay& delay, const std::vector<float>& input, int maxBlock, DelayFor delayFor)
{
    std::vector<float> output(input.size(), 0.0f);
    std::size_t start = 0;
    int blockIndex = 0;

    while (start < input.size())
    {
        const int block = std::max(1, maxBlock - (blockIndex++ % 3) * (maxBlock / 3));
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), input.size() - start));
        delay.Process(input.data() + start, input.data() + start, n, delayFor(start));
        std::copy_n(delay.Output(0), n, output.data() + start);
        start += static_cast<std::size_t>(n);
    }

    return output;
}

bool TestHelperExactAtZeroAndAtADelay()
{
    std::cout << "DryPathDelay: exact at 0 and at a delay, across block boundaries" << std::endl;
    const auto input = Ramp(5000);
    bool ok = true;

    for (const int delaySamples : {0, 1, 255, 256, 2048})
    {
        DryPathDelay delay;
        delay.Prepare(2048, 96, 1440);
        const auto output = RunHelper(delay, input, 96, [&](std::size_t) { return delaySamples; });
        bool exact = true;

        for (std::size_t i = 0; i < input.size(); ++i)
        {
            const float expected =
                i >= static_cast<std::size_t>(delaySamples) ? input[i - static_cast<std::size_t>(delaySamples)] : 0.0f;
            exact = exact && output[i] == expected;
        }

        ok &= Expect(exact, "the dry signal should be exactly " + std::to_string(delaySamples) + " samples late");
    }

    DryPathDelay clamped;
    clamped.Prepare(256, 64, 100);
    const auto output = RunHelper(clamped, input, 64, [](std::size_t) { return 5000; });
    ok &= Expect(output[256] == input[0] && output[255] == 0.0f,
                 "a delay beyond what Prepare sized for should stop at that size");
    return ok;
}

bool TestHelperCrossfadesToANewDelay()
{
    std::cout << "DryPathDelay: a new delay is reached by a crossfade, then exactly" << std::endl;
    constexpr std::size_t kLength = 24000;
    constexpr std::size_t kChangeAt = 6000;
    constexpr int kFade = 1440;
    std::vector<float> sine(kLength);

    for (std::size_t i = 0; i < kLength; ++i)
    {
        sine[i] = static_cast<float>(std::sin(2.0 * kPi * 110.0 * static_cast<double>(i) / kSampleRate));
    }

    const double naturalStep = 2.0 * kPi * 110.0 / kSampleRate; // the sine's own largest step
    bool ok = true;

    for (const auto& [from, to] : {std::pair{0, 512}, std::pair{512, 0}, std::pair{256, 2048}})
    {
        DryPathDelay delay;
        delay.Prepare(2048, 128, kFade);
        const auto output =
            RunHelper(delay, sine, 128, [&](std::size_t start) { return start < kChangeAt ? from : to; });
        double largestStep = 0.0;

        for (std::size_t i = 4096; i < kLength; ++i)
        {
            largestStep = std::max(largestStep, std::abs(static_cast<double>(output[i]) - output[i - 1]));
        }

        bool exactAfter = true;

        // The change lands at the first block starting at or after kChangeAt; give it a block.
        for (std::size_t i = kChangeAt + 128 + kFade; i < kLength; ++i)
        {
            exactAfter = exactAfter && output[i] == sine[i - static_cast<std::size_t>(to)];
        }

        const std::string name = std::to_string(from) + " -> " + std::to_string(to);
        ok &= Expect(largestStep < naturalStep * 1.5,
                     name + ": the dry signal should not jump (largest step " + std::to_string(largestStep) + ")");
        ok &= Expect(exactAfter, name + ": once faded, the dry signal should be exactly the new delay late");
    }

    DryPathDelay delay;
    delay.Prepare(2048, 64, kFade);
    RunHelper(delay, sine, 64, [](std::size_t) { return 0; });
    delay.Reset();
    const auto afterReset = RunHelper(delay, sine, 64, [](std::size_t) { return 300; });
    bool snapped = true;

    for (std::size_t i = 0; i < 3000; ++i)
    {
        snapped = snapped && afterReset[i] == (i < 300 ? 0.0f : sine[i - 300]);
    }

    ok &= Expect(snapped, "after Reset the history should be silent and the next delay taken at once");
    return ok;
}

// --- The effects -----------------------------------------------------------------------------

std::string DisplayName(const std::string& type)
{
    return type == EffectGuids::kCabIr ? "IR Cabinet" : "Convolution Reverb";
}

bool TestImpulseLandsAtReportedLatency(const std::string& type)
{
    std::cout << DisplayName(type) << ": an impulse at Mix 0.5 lands where the reported latency says" << std::endl;
    bool ok = true;

    struct Case
    {
        bool lowLatency;
        int maxBlock;
        int hostBlock;
    };

    for (const auto& c : {Case{true, 256, 256}, Case{true, 2048, 64}, Case{false, 64, 64}, Case{false, 512, 512},
                          Case{false, 2048, 2048}, Case{false, 2048, 64}})
    {
        const std::string name = DisplayName(type) + (c.lowLatency ? " Low Latency on" : " Low Latency off") +
                                 ", host block " + std::to_string(c.hostBlock) + " of " + std::to_string(c.maxBlock);
        auto effect = MakeEffect(type, 0.5, c.lowLatency, c.maxBlock);

        if (!Expect(effect != nullptr, name + ": the spike IR should load"))
        {
            ok = false;
            continue;
        }

        const int latency = effect->GetLatencySamples();
        const int expectedLatency = c.lowLatency ? 0 : RealtimeConvolver::GetMaxLatency(c.maxBlock);
        ok &= Expect(latency == expectedLatency, name + ": reports " + std::to_string(latency) + " samples, expected " +
                                                     std::to_string(expectedLatency));

        std::vector<float> input(static_cast<std::size_t>(latency) + kSpikeAt + 4096, 0.0f);
        input[0] = 1.0f;
        std::vector<float> outL;
        std::vector<float> outR;
        Render(*effect, input, outL, outR, c.hostBlock);

        const auto dryAt = static_cast<std::size_t>(latency);
        const std::size_t wetAt = dryAt + kSpikeAt;

        for (const auto* out : {&outL, &outR})
        {
            const std::string side = out == &outL ? " (left)" : " (right)";
            float strayPeak = 0.0f;
            std::size_t strayAt = 0;

            for (std::size_t i = 0; i < out->size(); ++i)
            {
                if (i != dryAt && i != wetAt && std::abs((*out)[i]) > strayPeak)
                {
                    strayPeak = std::abs((*out)[i]);
                    strayAt = i;
                }
            }

            ok &= Expect(std::abs((*out)[dryAt] - 0.5f) < 1e-4f,
                         name + side + ": the dry spike should be 0.5 at the reported latency, sample " +
                             std::to_string(dryAt) + ", found " + std::to_string((*out)[dryAt]));
            ok &= Expect(std::abs((*out)[wetAt] - 0.5f) < 0.05f,
                         name + side + ": the wet spike should be about 0.5, " + std::to_string(kSpikeAt) +
                             " samples after it, found " + std::to_string((*out)[wetAt]));
            ok &= Expect(strayPeak < 1e-3f, name + side + ": nothing else should sound, found " +
                                                std::to_string(strayPeak) + " at sample " + std::to_string(strayAt));
        }
    }

    return ok;
}

bool TestLowLatencySwitchMovesTheDryWithoutAClick(const std::string& type)
{
    std::cout << DisplayName(type) << ": switching Low Latency while playing moves the dry without a click"
              << std::endl;
    const std::string name = DisplayName(type);
    constexpr int kMaxBlock = 512;
    constexpr int kHostBlock = 128;
    const int partition = RealtimeConvolver::GetMaxLatency(kMaxBlock);
    bool ok = true;

    // At Mix 0 the output is the dry signal alone.
    auto effect = MakeEffect(type, 0.0, true, kMaxBlock);

    if (!Expect(effect != nullptr, name + ": the spike IR should load"))
    {
        return false;
    }

    constexpr std::size_t kSegment = 9600; // 200 ms, far longer than the 30 ms crossfade
    std::vector<float> sine(kSegment * 3);

    for (std::size_t i = 0; i < sine.size(); ++i)
    {
        sine[i] = static_cast<float>(std::sin(2.0 * kPi * 110.0 * static_cast<double>(i) / kSampleRate));
    }

    const double naturalStep = 2.0 * kPi * 110.0 / kSampleRate;
    std::vector<float> output(sine.size(), 0.0f);
    std::vector<float> scratchR(static_cast<std::size_t>(kHostBlock), 0.0f);
    std::vector<int> latencyAt(sine.size(), 0);

    for (std::size_t start = 0; start < sine.size(); start += kHostBlock)
    {
        if (start == kSegment || start == 2 * kSegment)
        {
            // Off, then on again: the message thread builds it, as for a click in the node panel.
            effect->SetParam("lowLatency", start == kSegment ? 0.0 : 1.0);
            ok &= Expect(ApplyDeferredRebuild(*effect), name + ": a Low Latency change should be rebuilt");
        }

        std::vector<float> inR(sine.begin() + static_cast<std::ptrdiff_t>(start),
                               sine.begin() + static_cast<std::ptrdiff_t>(start + kHostBlock));
        float* inputs[2] = {sine.data() + start, inR.data()};
        float* outputs[2] = {output.data() + start, scratchR.data()};
        effect->Process(inputs, outputs, kHostBlock);
        std::fill_n(latencyAt.begin() + static_cast<std::ptrdiff_t>(start), kHostBlock, effect->GetLatencySamples());
    }

    ok &= Expect(latencyAt[kSegment - 1] == 0 && latencyAt[kSegment] == partition && latencyAt[2 * kSegment] == 0,
                 name + ": the reported latency should go 0, then " + std::to_string(partition) + ", then 0");

    double largestStep = 0.0;

    for (std::size_t i = 1; i < output.size(); ++i)
    {
        largestStep = std::max(largestStep, std::abs(static_cast<double>(output[i]) - output[i - 1]));
    }

    ok &= Expect(largestStep < naturalStep * 1.5,
                 name + ": the dry signal should not jump (largest step " + std::to_string(largestStep) + ")");

    // Each segment's last 100 ms, well past the crossfade: exactly the latency reported, at Mix 0.
    for (std::size_t segment = 0; segment < 3; ++segment)
    {
        const std::size_t from = segment * kSegment + kSegment / 2;
        const auto delay = static_cast<std::size_t>(latencyAt[from]);
        bool exact = true;

        for (std::size_t i = from; i < (segment + 1) * kSegment; ++i)
        {
            exact = exact && output[i] == sine[i - delay];
        }

        ok &= Expect(exact, name + ": segment " + std::to_string(segment) + " should be the input exactly " +
                                std::to_string(delay) + " samples late");
    }

    return ok;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool ok = true;
    ok &= TestHelperExactAtZeroAndAtADelay();
    ok &= TestHelperCrossfadesToANewDelay();

    for (const std::string type : {EffectGuids::kReverbIr, EffectGuids::kCabIr})
    {
        ok &= TestImpulseLandsAtReportedLatency(type);
        ok &= TestLowLatencySwitchMovesTheDryWithoutAClick(type);
    }

    std::error_code ignored;
    fs::remove_all(TempDir(), ignored);

    std::cout << (ok ? "All IR dry path delay tests passed" : "IR dry path delay tests FAILED") << std::endl;
    return ok ? 0 : 1;
}
