/**
 * IRCabReloadTests.cpp — loading IRs into an IR Cabinet that already has some.
 *
 * A load that leaves a slot empty must empty it. The cab used to "clear" a slot with
 * RealtimeConvolver::Reset(), which only clears history, so the slot went on playing its old IR:
 * a cab given A and B, then A alone, still blended in the old B (A came out 3 dB down at irBlend
 * 0.5), and with B's impulse gone, Prepare and the Normalize/Low Latency rebuilds skipped B while
 * it kept sounding. A composite's inner executor reaches this, reloading its inner cab with one
 * resource and no slot index.
 *
 * Reloading a prepared cab must sound bit-identical to a fresh cab given the same IRs: A alone,
 * with or without a slot index; A through the single-file LoadResource; a failed A beside B; and
 * after a Prepare at another rate or a Low Latency rebuild. An empty load must leave it passing
 * the input through untouched, as SignalGraphExecutor expects of a node whose slots were cleared.
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

#include "dsp/DeferredRebuild.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;

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
        fs::temp_directory_path() / ("guitarfx_ir_cab_reload_tests_" + std::to_string(std::random_device{}()));
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

std::vector<float> Noise(std::size_t length, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    std::vector<float> samples(length);

    for (auto& sample : samples)
    {
        sample = noise(rng);
    }

    return samples;
}

/// A cab-length IR (under Standard quality's truncation): decaying noise, distinct per seed.
std::vector<float> DecayingNoiseIR(unsigned seed)
{
    auto ir = Noise(1600, seed);

    for (std::size_t n = 0; n < ir.size(); ++n)
    {
        ir[n] *= static_cast<float>(std::exp(-static_cast<double>(n) / 300.0));
    }

    return ir;
}

ResourceRef IRRef(const fs::path& path, int slot)
{
    ResourceRef ir;
    ir.resourceType = "ir";
    ir.filePath = path;

    // SignalGraphExecutor stamps each slot; a composite's inner reload passes one ref without.
    if (slot >= 0)
    {
        ir.metadata["resourceSlotIndex"] = std::to_string(slot);
    }

    return ir;
}

struct Load
{
    std::vector<ResourceRef> refs;
    std::vector<fs::path> paths;
};

Load SlotsAB(const fs::path& irA, const fs::path& irB)
{
    return {{IRRef(irA, 0), IRRef(irB, 1)}, {irA, irB}};
}

/// As the executor builds a node: params, then the load, then Prepare.
std::unique_ptr<EffectProcessor> MakeCab(const Load& load)
{
    auto cab = EffectRegistry::Instance().Create("cab_ir");
    cab->SetParam("irBlend", 0.5);
    cab->LoadResources(load.refs, load.paths);
    cab->Prepare(kSampleRate, kBlockSize);
    return cab;
}

/// Both output channels, left then right, for a stereo input with a different noise on each side.
std::vector<float> Render(EffectProcessor& cab, std::size_t length = kBlockSize * 100)
{
    const auto inputL = Noise(length, 11);
    const auto inputR = Noise(length, 12);
    std::vector<float> out(length * 2);
    std::vector<float> inL(kBlockSize), inR(kBlockSize), outL(kBlockSize), outR(kBlockSize);

    for (std::size_t start = 0; start + kBlockSize <= length; start += kBlockSize)
    {
        std::copy_n(inputL.begin() + static_cast<std::ptrdiff_t>(start), kBlockSize, inL.begin());
        std::copy_n(inputR.begin() + static_cast<std::ptrdiff_t>(start), kBlockSize, inR.begin());
        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        cab.Process(inputs, outputs, kBlockSize);
        std::copy(outL.begin(), outL.end(), out.begin() + static_cast<std::ptrdiff_t>(start));
        std::copy(outR.begin(), outR.end(), out.begin() + static_cast<std::ptrdiff_t>(length + start));
    }

    return out;
}

/// Clears both cabs' history (and the reloaded one's crossfade), then compares what they render.
bool SoundsTheSame(EffectProcessor& reloaded, EffectProcessor& fresh, const std::string& what)
{
    reloaded.Reset();
    fresh.Reset();
    const auto x = Render(reloaded);
    const auto y = Render(fresh);
    float worst = 0.0f;

    for (std::size_t n = 0; n < x.size(); ++n)
    {
        worst = std::max(worst, std::abs(x[n] - y[n]));
    }

    bool ok =
        Expect(worst == 0.0f, what + ": sounds like a fresh cab (worst difference " + std::to_string(worst) + ")");
    ok &= Expect(reloaded.GetLatencySamples() == fresh.GetLatencySamples(), what + ": reports a fresh cab's latency");
    ok &= Expect(reloaded.HasResource() == fresh.HasResource(), what + ": HasResource() as a fresh cab");
    return ok;
}

/// Flips Low Latency and runs the deferred rebuild it asks for, as the message thread would.
void RebuildWithLowLatencyFlipped(EffectProcessor& cab)
{
    cab.SetParam("lowLatency", cab.GetParam("lowLatency") > 0.5 ? 0.0 : 1.0);

    if (auto work = cab.TakeDeferredRebuild())
    {
        work->Build();
        cab.CommitDeferredRebuild(*work);
    }
}

struct IRFiles
{
    fs::path a = WriteMonoWav("a.wav", DecayingNoiseIR(1));
    fs::path b = WriteMonoWav("b.wav", DecayingNoiseIR(2));
};

bool TestReloadWithOnlyADropsB(const IRFiles& irs)
{
    bool ok = true;

    for (const int slot : {0, -1})
    {
        const std::string what = slot == 0 ? "A+B, then A" : "A+B, then A without a slot index";
        auto reloaded = MakeCab(SlotsAB(irs.a, irs.b));
        Render(*reloaded); // playing both before the reload
        reloaded->LoadResources({IRRef(irs.a, slot)}, {irs.a});
        auto fresh = MakeCab({{IRRef(irs.a, slot)}, {irs.a}});
        ok &= SoundsTheSame(*reloaded, *fresh, what);

        // Prepare rebuilds from the impulses, which no longer include B.
        reloaded->Prepare(44100.0, kBlockSize);
        fresh->Prepare(44100.0, kBlockSize);
        ok &= SoundsTheSame(*reloaded, *fresh, what + ", then Prepare at 44.1 kHz");

        RebuildWithLowLatencyFlipped(*reloaded);
        RebuildWithLowLatencyFlipped(*fresh);
        ok &= SoundsTheSame(*reloaded, *fresh, what + ", then a Low Latency rebuild");
    }

    return ok;
}

bool TestSingleFileLoadDropsB(const IRFiles& irs)
{
    auto reloaded = MakeCab(SlotsAB(irs.a, irs.b));
    Render(*reloaded);
    reloaded->LoadResource(irs.a);

    auto fresh = EffectRegistry::Instance().Create("cab_ir");
    fresh->SetParam("irBlend", 0.5);
    fresh->LoadResource(irs.a);
    fresh->Prepare(kSampleRate, kBlockSize);
    return SoundsTheSame(*reloaded, *fresh, "A+B, then LoadResource(A)");
}

bool TestFailedLoadOfADropsA(const IRFiles& irs)
{
    const auto missing = TempDir() / "missing.wav";
    const Load failedA = SlotsAB(missing, irs.b);
    auto reloaded = MakeCab(SlotsAB(irs.a, irs.b));
    Render(*reloaded);
    reloaded->LoadResources(failedA.refs, failedA.paths);
    auto fresh = MakeCab(failedA);
    return SoundsTheSame(*reloaded, *fresh, "A+B, then a missing A with B");
}

bool TestEmptyLoadPassesDry(const IRFiles& irs)
{
    auto cab = MakeCab(SlotsAB(irs.a, irs.b));
    Render(*cab);
    cab->LoadResources({}, {});

    bool ok = Expect(!cab->HasResource(), "an empty load leaves no resource");
    ok &= Expect(cab->GetLatencySamples() == 0, "an empty load leaves no latency");

    // No Reset: passing dry is a plain copy from the next block on.
    const std::size_t length = kBlockSize * 20;
    const auto rendered = Render(*cab, length);
    auto input = Noise(length, 11);
    const auto inputR = Noise(length, 12);
    input.insert(input.end(), inputR.begin(), inputR.end());
    ok &= Expect(rendered == input, "an empty load passes the input through untouched");
    return ok;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool ok = true;
    {
        const IRFiles irs;
        ok &= TestReloadWithOnlyADropsB(irs);
        ok &= TestSingleFileLoadDropsB(irs);
        ok &= TestFailedLoadOfADropsA(irs);
        ok &= TestEmptyLoadPassesDry(irs);
    }

    std::error_code ignored;
    fs::remove_all(TempDir(), ignored);

    if (!ok)
    {
        return 1;
    }

    std::cout << "IRCabReloadTests passed" << std::endl;
    return 0;
}
