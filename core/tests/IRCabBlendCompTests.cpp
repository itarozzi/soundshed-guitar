/**
 * IRCabBlendCompTests.cpp — the IR Cabinet's Auto Gain Comp with two IRs blended.
 *
 * Auto Gain Comp keeps a blend as loud as its two IRs are on their own, interpolated by IR Blend
 * in power. Until 2026-10-18 it took every pair as unalike, summing in power, and lifted IR Blend
 * 0.5 by 3 dB. Two mics on one speaker are much alike and sum close to amplitude, so a two-mic
 * blend played up to ~2.6 dB loud. It now measures how alike the two IRs are, at every IR B
 * Offset, when the convolvers are built.
 *
 * - Two copies of one IR hold the lone IR's level across IR Blend, and so does a real two-mic pair
 *   (421 and 906 on one 1960 cab, testdata/assets/ir).
 * - Unalike IRs still get the old power comp, and so does one IR against itself moved by IR B
 *   Offset, which makes it unalike.
 * - With Normalize off, the blend interpolates the two IRs' own levels in power.
 * - Invert on one of two copies cancels the pair; the comp lifts it at most 6 dB.
 * - A load that changes how alike the pair is crossfades each side at its own comp.
 *
 * Levels are measured on white noise, which is what the comp is defined for.
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
#include <utility>
#include <vector>

#include "dsp/EffectRegistry.h"
#include "dsp/FiniteCheck.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

#ifndef GUITARFX_TEST_RESOURCES_DIR
    #define GUITARFX_TEST_RESOURCES_DIR "core/tests/testdata"
#endif

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr std::size_t kLength = kBlockSize * 400;
// The IR Cab's crossfade on an IR change: 30 ms.
constexpr std::size_t kCrossfadeSamples = 1440;

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << std::endl;
    }

    return condition;
}

bool ExpectNear(double value, double expected, double tolerance, const std::string& what)
{
    return Expect(IsFinite(value) && std::abs(value - expected) <= tolerance,
                  what + ": expected " + std::to_string(expected) + " +/- " + std::to_string(tolerance) + ", got " +
                      std::to_string(value));
}

/// This run's own folder, so two runs at once cannot delete each other's IRs.
const fs::path& TempDir()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("guitarfx_ir_cab_blend_comp_tests_" + std::to_string(std::random_device{}()));
    return dir;
}

/// A mono 32-bit float WAV at 48 kHz.
fs::path WriteWav(const std::string& name, const std::vector<float>& samples)
{
    fs::create_directories(TempDir());
    const auto path = TempDir() / name;
    const auto dataSize = static_cast<std::uint32_t>(samples.size() * sizeof(float));
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t format = 3; // IEEE float
    const std::uint16_t channels = 1;
    const auto rate = static_cast<std::uint32_t>(kSampleRate);
    const std::uint16_t blockAlign = sizeof(float);
    const std::uint32_t byteRate = rate * blockAlign;
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
std::vector<float> DecayingNoiseIR(unsigned seed, float gain = 1.0f)
{
    auto ir = Noise(1600, seed);

    for (std::size_t n = 0; n < ir.size(); ++n)
    {
        ir[n] *= gain * static_cast<float>(std::exp(-static_cast<double>(n) / 300.0));
    }

    return ir;
}

struct IRFiles
{
    fs::path a = WriteWav("a.wav", DecayingNoiseIR(1));
    fs::path aHalf = WriteWav("a_half.wav", DecayingNoiseIR(1, 0.5f)); // a at -6 dB
    fs::path b = WriteWav("b.wav", DecayingNoiseIR(2));
    fs::path mic421 = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "ir" / "421 1960.wav";
    fs::path mic906 = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "ir" / "906 1960.wav";
};

void Load(EffectProcessor& cab, const fs::path& a, const fs::path& b)
{
    std::vector<ResourceRef> refs;
    std::vector<fs::path> paths;

    for (const auto& [path, slot] : {std::pair{a, 0}, std::pair{b, 1}})
    {
        if (path.empty())
        {
            continue;
        }

        // SignalGraphExecutor stamps each ref with its slot, so B can load with A empty.
        ResourceRef ref;
        ref.resourceType = "ir";
        ref.filePath = path;
        ref.metadata["resourceSlotIndex"] = std::to_string(slot);
        refs.push_back(ref);
        paths.push_back(path);
    }

    cab.LoadResources(refs, paths);
}

using Params = std::vector<std::pair<std::string, double>>;

/// As the executor builds a node: params, then the load, then Prepare.
std::unique_ptr<EffectProcessor> MakeCab(const fs::path& a, const fs::path& b, const Params& params = {})
{
    auto cab = EffectRegistry::Instance().Create("cab_ir");

    for (const auto& [key, value] : params)
    {
        cab->SetParam(key, value);
    }

    Load(*cab, a, b);
    cab->Prepare(kSampleRate, kBlockSize);
    return cab;
}

/// Both output channels, left then right, for white noise in, a different noise on each side.
std::vector<float> Render(EffectProcessor& cab, unsigned seed = 11, std::size_t length = kLength)
{
    const auto inputL = Noise(length, seed);
    const auto inputR = Noise(length, seed + 1);
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

/// The cab's output level in dB, both channels, past the first 100 ms.
double LevelDb(EffectProcessor& cab)
{
    const auto out = Render(cab);
    double sum = 0.0;
    std::size_t count = 0;

    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        for (std::size_t n = 4800; n < kLength; ++n)
        {
            const double s = out[channel * kLength + n];
            sum += s * s;
            ++count;
        }
    }

    return 10.0 * std::log10(sum / static_cast<double>(count));
}

double LoneLevelDb(const fs::path& ir, const Params& params = {})
{
    return LevelDb(*MakeCab(ir, {}, params));
}

double BlendLevelDb(const fs::path& a, const fs::path& b, double blend, const Params& params = {})
{
    Params all = params;
    all.emplace_back("irBlend", blend);
    return LevelDb(*MakeCab(a, b, all));
}

bool TestAlikePairsHoldTheLevel(const IRFiles& irs)
{
    std::cout << "A pair much alike holds the lone level across IR Blend" << std::endl;

    bool ok = true;
    const double lone = LoneLevelDb(irs.a);

    for (const double blend : {0.25, 0.5, 0.75})
    {
        const std::string at = " at IR Blend " + std::to_string(blend).substr(0, 4);
        // The old comp lifted this 3 dB at 0.5.
        ok &= ExpectNear(BlendLevelDb(irs.a, irs.a, blend) - lone, 0.0, 0.05, "two copies of one IR" + at + " (dB)");
    }

    if (!fs::exists(irs.mic421) || !fs::exists(irs.mic906))
    {
        return Expect(false, "the two-mic test IRs should be in testdata: " + irs.mic421.string()) && ok;
    }

    const double lone421 = LoneLevelDb(irs.mic421);
    const double lone906 = LoneLevelDb(irs.mic906);

    for (const double blend : {0.25, 0.5, 0.75})
    {
        const std::string at = " at IR Blend " + std::to_string(blend).substr(0, 4);
        const double expected =
            10.0 * std::log10((1.0 - blend) * std::pow(10.0, lone421 / 10.0) + blend * std::pow(10.0, lone906 / 10.0));
        // The old comp played 0.5 about 2.6 dB loud.
        ok &= ExpectNear(BlendLevelDb(irs.mic421, irs.mic906, blend), expected, 0.3,
                         "421 and 906 on one 1960" + at + " (dB)");
    }

    return ok;
}

bool TestUnalikePairsGetThePowerComp(const IRFiles& irs)
{
    std::cout << "Unalike IRs, or one IR moved by IR B Offset, still get the power comp" << std::endl;

    bool ok = true;
    const double lone = LoneLevelDb(irs.a);
    ok &= ExpectNear(BlendLevelDb(irs.a, irs.b, 0.5) - lone, 0.0, 0.3, "two unalike IRs at IR Blend 0.5 (dB)");

    // 1 ms apart, one IR against itself is unalike: the comp reads the likeness at the offset.
    ok &= ExpectNear(BlendLevelDb(irs.a, irs.a, 0.5, {{"slotBOffset", 1.0}}) - lone, 0.0, 0.3,
                     "one IR against itself 1 ms later at IR Blend 0.5 (dB)");
    ok &= ExpectNear(BlendLevelDb(irs.a, irs.a, 0.5, {{"slotBOffset", -1.0}}) - lone, 0.0, 0.3,
                     "one IR against itself 1 ms earlier at IR Blend 0.5 (dB)");
    return ok;
}

bool TestNormalizeOffInterpolatesTheLevels(const IRFiles& irs)
{
    std::cout << "With Normalize off, IR Blend interpolates the two IRs' own levels in power" << std::endl;

    bool ok = true;
    const Params off = {{"normalizeIR", 0.0}};
    const double loud = LoneLevelDb(irs.a, off);
    ok &= ExpectNear(LoneLevelDb(irs.aHalf, off) - loud, -6.02, 0.05, "the half-level copy alone (dB)");

    for (const double blend : {0.25, 0.5, 0.75})
    {
        const std::string at = " at IR Blend " + std::to_string(blend).substr(0, 4);
        const double expected = 10.0 * std::log10((1.0 - blend) + blend * 0.25);
        ok &= ExpectNear(BlendLevelDb(irs.a, irs.aHalf, blend, off) - loud, expected, 0.05,
                         "an IR and a copy 6 dB down" + at + " (dB)");
    }

    return ok;
}

bool TestInvertedPairIsLiftedAtMostSixDb(const IRFiles& irs)
{
    std::cout << "An inverted pair that cancels is lifted at most 6 dB" << std::endl;

    bool ok = true;
    const double lone = LoneLevelDb(irs.a);
    const Params inverted = {{"slotBPolarity", 1.0}};

    // 0.75 - 0.25 of the IR is left, half its amplitude: exactly what the 6 dB limit lifts back.
    ok &= ExpectNear(BlendLevelDb(irs.a, irs.a, 0.25, inverted) - lone, 0.0, 0.05,
                     "two copies, one inverted, at IR Blend 0.25 (dB)");

    // At 0.5 they cancel entirely, to silence (-inf dB): the comp must not chase that. A NaN
    // fails the comparison.
    const double cancelled = BlendLevelDb(irs.a, irs.a, 0.5, inverted) - lone;
    ok &= Expect(cancelled < -60.0, "two copies, one inverted, at IR Blend 0.5 should cancel, but came out " +
                                        std::to_string(cancelled) + " dB against one alone");
    return ok;
}

bool TestALoadCrossfadesEachSideAtItsOwnComp(const IRFiles& irs)
{
    std::cout << "A load that changes how alike the pair is crossfades each side at its own comp" << std::endl;

    // A pair of copies (comp 0 dB at 0.5) replaced by an unalike pair (comp +3 dB): the slots are
    // the same before and after, so only the measurement of the pair tells the two sides apart.
    const Params params = {{"irBlend", 0.5}};
    auto cab = MakeCab(irs.a, irs.a, params);
    Render(*cab, 41);
    Load(*cab, irs.a, irs.b);
    const auto crossfaded = Render(*cab, 51);
    auto outgoing = MakeCab(irs.a, irs.a, params);
    auto incoming = MakeCab(irs.a, irs.b, params);
    Render(*outgoing, 41); // the history the cab had at the load
    const auto from = Render(*outgoing, 51);
    const auto to = Render(*incoming, 51);
    double worst = 0.0;

    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        for (std::size_t n = 0; n < kLength; ++n)
        {
            const std::size_t i = channel * kLength + n;
            const double oldWeight = n < kCrossfadeSamples ? 1.0 - static_cast<double>(n) / kCrossfadeSamples : 0.0;
            const double expected = from[i] * oldWeight + to[i] * (1.0 - oldWeight);
            worst = std::max(worst, std::abs(crossfaded[i] - expected));
        }
    }

    return Expect(worst < 1e-5, "copies, then an unalike pair: each side at its own comp (worst difference " +
                                    std::to_string(worst) + ")");
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool ok = true;
    {
        const IRFiles irs;
        ok &= TestAlikePairsHoldTheLevel(irs);
        ok &= TestUnalikePairsGetThePowerComp(irs);
        ok &= TestNormalizeOffInterpolatesTheLevels(irs);
        ok &= TestInvertedPairIsLiftedAtMostSixDb(irs);
        ok &= TestALoadCrossfadesEachSideAtItsOwnComp(irs);
    }

    std::error_code ignored;
    fs::remove_all(TempDir(), ignored);

    if (!ok)
    {
        return 1;
    }

    std::cout << "IRCabBlendCompTests passed" << std::endl;
    return 0;
}
