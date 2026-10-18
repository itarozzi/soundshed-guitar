/**
 * IrZeroLatencyTests.cpp — the IR Cabinet and Convolution Reverb have no latency, and the Low
 * Latency switch they used to have is gone.
 *
 * Until 2026-10 Low Latency off ran a uniform convolver a partition late, 256 to 2048 samples,
 * and delayed the dry to match. Now both effects always run the zero-latency convolver.
 *
 * - An impulse through each effect at Mix 0.5, with an IR that is a single spike 300 samples in:
 *   the dry spike lands at sample 0 and the wet one 300 samples after it, at several host block
 *   sizes, and neither effect reports any latency.
 * - The IR Cabinet with Speaker Drive on: the dry stays clean, since only the wet is driven.
 * - A preset or binding saved with lowLatency 0: ignored. Nothing is rebuilt, the output is the
 *   same to the bit, and the effect no longer lists the parameter.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
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
        fs::temp_directory_path() / ("guitarfx_ir_zero_latency_tests_" + std::to_string(std::random_device{}()));
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

/// 2000 samples, silent but for one spike: long enough for the FFT stages behind the FIR head.
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

/// A node with the spike IR, set up the way a preset's is: params, then the IR, then Prepare.
std::unique_ptr<EffectProcessor> MakeEffect(const std::string& type, const std::map<std::string, double>& params,
                                            int maxBlock)
{
    auto effect = EffectRegistry::Instance().Create(type);

    for (const auto& [key, value] : params)
    {
        effect->SetParam(key, value);
    }

    if (!effect->LoadResource(SpikeIR()))
    {
        return nullptr;
    }

    effect->Prepare(kSampleRate, maxBlock);
    return effect;
}

std::string DisplayName(const std::string& type)
{
    return type == EffectGuids::kCabIr ? "IR Cabinet" : "Convolution Reverb";
}

bool TestImpulseLinesUp(const std::string& type)
{
    std::cout << DisplayName(type) << ": an impulse at Mix 0.5 has its dry at sample 0, its wet on the IR" << std::endl;
    bool ok = true;

    struct Case
    {
        int maxBlock;
        int hostBlock;
    };

    for (const auto& c : {Case{64, 64}, Case{256, 256}, Case{512, 37}, Case{2048, 64}, Case{2048, 2048}})
    {
        const std::string name =
            DisplayName(type) + ", host block " + std::to_string(c.hostBlock) + " of " + std::to_string(c.maxBlock);
        auto effect = MakeEffect(type, {{"mix", 0.5}}, c.maxBlock);

        if (!Expect(effect != nullptr, name + ": the spike IR should load"))
        {
            ok = false;
            continue;
        }

        ok &= Expect(effect->GetLatencySamples() == 0,
                     name + ": reports " + std::to_string(effect->GetLatencySamples()) + " samples of latency");

        std::vector<float> input(kSpikeAt + 4096, 0.0f);
        input[0] = 1.0f;
        std::vector<float> outL;
        std::vector<float> outR;
        Render(*effect, input, outL, outR, c.hostBlock);

        for (const auto* out : {&outL, &outR})
        {
            const std::string side = out == &outL ? " (left)" : " (right)";
            float strayPeak = 0.0f;
            std::size_t strayAt = 0;

            for (std::size_t i = 0; i < out->size(); ++i)
            {
                if (i != 0 && i != kSpikeAt && std::abs((*out)[i]) > strayPeak)
                {
                    strayPeak = std::abs((*out)[i]);
                    strayAt = i;
                }
            }

            ok &= Expect(std::abs((*out)[0] - 0.5f) < 1e-4f,
                         name + side + ": the dry spike should be 0.5 at sample 0, found " + std::to_string((*out)[0]));
            ok &= Expect(std::abs((*out)[kSpikeAt] - 0.5f) < 0.05f,
                         name + side + ": the wet spike should be about 0.5 at sample " + std::to_string(kSpikeAt) +
                             ", found " + std::to_string((*out)[kSpikeAt]));
            ok &= Expect(strayPeak < 1e-3f, name + side + ": nothing else should sound, found " +
                                                std::to_string(strayPeak) + " at sample " + std::to_string(strayAt));
        }
    }

    return ok;
}

/// Speaker Drive bends only what reaches the IR. The dry signal took a copy of the input on its
/// way through the dry delay; now it is the cab's own input buffer, which the drive must not touch.
bool TestSpeakerDriveLeavesTheDryClean()
{
    std::cout << "IR Cabinet: Speaker Drive leaves the dry signal clean" << std::endl;
    bool ok = true;
    auto effect = MakeEffect(EffectGuids::kCabIr, {{"mix", 0.5}, {"speakerDrive", 1.0}}, 256);

    if (!Expect(effect != nullptr, "the spike IR should load"))
    {
        return false;
    }

    // Before the IR's spike the output is the dry alone: the input at half level, exactly.
    std::vector<float> input(kSpikeAt + 1024, 0.0f);

    for (std::size_t i = 0; i < kSpikeAt; ++i)
    {
        input[i] = 0.9f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
    }

    std::vector<float> outL;
    std::vector<float> outR;
    Render(*effect, input, outL, outR, 64);
    float worst = 0.0f;

    for (std::size_t i = 0; i < kSpikeAt; ++i)
    {
        worst = std::max({worst, std::abs(outL[i] - 0.5f * input[i]), std::abs(outR[i] - 0.5f * input[i])});
    }

    ok &=
        Expect(worst < 1e-5f, "the dry should be the clean input at half level, off by up to " + std::to_string(worst));

    // And the drive did reach the wet: the IR's echo of the input is not the clean input.
    float wetDiff = 0.0f;

    for (std::size_t i = 0; i < kSpikeAt; ++i)
    {
        wetDiff = std::max(wetDiff, std::abs(outL[i + kSpikeAt] - 0.5f * input[i]));
    }

    ok &= Expect(wetDiff > 1e-3f, "the wet should be driven, but it is the clean input");
    return ok;
}

/// Presets and MIDI bindings saved before the switch was retired still carry lowLatency, some at 0.
bool TestSavedLowLatencyIsIgnored(const std::string& type)
{
    std::cout << DisplayName(type) << ": a saved lowLatency of 0 is ignored" << std::endl;
    const std::string name = DisplayName(type);
    bool ok = true;

    const auto info = EffectRegistry::Instance().GetTypeInfo(type);
    const bool listed = info && std::any_of(info->parameters.begin(), info->parameters.end(),
                                            [](const ParameterDef& p) { return p.id == "lowLatency"; });
    ok &= Expect(info.has_value() && !listed, name + ": the effect should not list a lowLatency parameter");

    auto plain = MakeEffect(type, {{"mix", 0.5}}, 512);
    auto saved = MakeEffect(type, {{"mix", 0.5}, {"lowLatency", 0.0}}, 512);

    if (!Expect(plain != nullptr && saved != nullptr, name + ": the spike IR should load"))
    {
        return false;
    }

    // Set again with the IR loaded, as automation or a later preset load would.
    saved->SetParam("lowLatency", 0.0);
    ok &= Expect(saved->TakeDeferredRebuild() == nullptr, name + ": lowLatency should ask for no rebuild");
    ok &= Expect(saved->GetLatencySamples() == 0, name + ": lowLatency 0 should add no latency");

    std::mt19937 rng(11);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    std::vector<float> input(8192);

    for (auto& s : input)
    {
        s = noise(rng);
    }

    std::vector<float> plainL;
    std::vector<float> plainR;
    std::vector<float> savedL;
    std::vector<float> savedR;
    Render(*plain, input, plainL, plainR, 128);
    Render(*saved, input, savedL, savedR, 128);
    ok &= Expect(plainL == savedL && plainR == savedR, name + ": the output should be the same to the bit");
    return ok;
}

std::vector<float> Tone(double rmsDbfs, std::size_t length)
{
    const double amplitude = std::pow(10.0, rmsDbfs / 20.0) * std::sqrt(2.0);
    std::vector<float> tone(length);

    for (std::size_t n = 0; n < length; ++n)
    {
        tone[n] = static_cast<float>(
            amplitude * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(n) / kSampleRate));
    }

    return tone;
}

/// A bypassed cab passes nothing to Speaker Drive, so the speaker cools. Its compression used to
/// freeze over the bypass instead, and came back still holding down the loud passage before it:
/// -6 dB on quiet playing for about a second.
bool TestSpeakerDriveCoolsWhileBypassed()
{
    std::cout << "IR Cabinet: Speaker Drive comes back from a bypass cooled" << std::endl;
    auto cab = MakeEffect(EffectGuids::kCabIr, {{"speakerDrive", 1.0}}, 256);
    auto fresh = MakeEffect(EffectGuids::kCabIr, {{"speakerDrive", 1.0}}, 256);

    if (!Expect(cab != nullptr && fresh != nullptr, "the spike IR should load"))
    {
        return false;
    }

    std::vector<float> outL;
    std::vector<float> outR;
    Render(*cab, Tone(0.0, 48000), outL, outR, 64); // a loud second, at the compression's limit
    cab->SetParam("enabled", 0.0);
    Render(*cab, Tone(0.0, 640), outL, outR, 64);
    cab->SetParam("enabled", 1.0);

    const auto quiet = Tone(-30.0, 9600);
    std::vector<float> afterL;
    std::vector<float> freshL;
    Render(*cab, quiet, afterL, outR, 64);
    Render(*fresh, quiet, freshL, outR, 64);

    // From 10 ms after the quiet playing reaches the spike, past the history from before.
    double after = 0.0;
    double expected = 0.0;

    for (std::size_t n = kSpikeAt + 480; n < quiet.size(); ++n)
    {
        after += static_cast<double>(afterL[n]) * afterL[n];
        expected += static_cast<double>(freshL[n]) * freshL[n];
    }

    const double differenceDb = 10.0 * std::log10(after / expected);
    return Expect(std::abs(differenceDb) < 0.1, "quiet playing after the bypass should be as loud as on a fresh " +
                                                    std::string("cab, but was ") + std::to_string(differenceDb) +
                                                    " dB off");
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool ok = true;

    for (const std::string type : {EffectGuids::kReverbIr, EffectGuids::kCabIr})
    {
        ok &= TestImpulseLinesUp(type);
        ok &= TestSavedLowLatencyIsIgnored(type);
    }

    ok &= TestSpeakerDriveLeavesTheDryClean();
    ok &= TestSpeakerDriveCoolsWhileBypassed();

    std::error_code ignored;
    fs::remove_all(TempDir(), ignored);

    std::cout << (ok ? "All IR zero-latency tests passed" : "IR zero-latency tests FAILED") << std::endl;
    return ok ? 0 : 1;
}
