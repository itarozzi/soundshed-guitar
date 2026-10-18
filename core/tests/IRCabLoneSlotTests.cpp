/**
 * IRCabLoneSlotTests.cpp — an IR Cabinet with one IR, in either slot.
 *
 * A lone IR plays in full, as if IR Blend were turned all the way to its slot, whatever IR Blend,
 * L/R Split or IR B Offset say: those only mean something between two IRs. That was always true of
 * IR A alone. A cab with only IR B passed the guitar through dry until 2026-10-17, because Process()
 * waited for slot A's convolvers.
 *
 * - IR B alone sounds bit-identical to IR A alone with the same file, at IR Blend 0, 0.5 and 1,
 *   with L/R Split on, with an IR B Offset, at Mix 0.5, with Trim Leading Silence, and with a
 *   stereo IR. Neither changes with IR Blend or L/R Split.
 * - A lone slot keeps its own Level, Pan, Invert and Mic settings, and ignores the other slot's.
 * - A lone IR sounds exactly like both loaded with IR Blend all the way to it.
 * - Reloading a lone B crossfades exactly as reloading a lone A does. Adding or clearing the other
 *   slot crossfades from what was playing at the level it played at, with no dip.
 * - The outgoing side of a load's crossfade keeps the history it was playing with. Until 2026-10-18
 *   it was rebuilt empty, so an IR change while playing dropped nearly to silence for ~1 ms.
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
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr std::size_t kLength = kBlockSize * 100;
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

/// This run's own folder, so two runs at once cannot delete each other's IRs.
const fs::path& TempDir()
{
    static const fs::path dir =
        fs::temp_directory_path() / ("guitarfx_ir_cab_lone_slot_tests_" + std::to_string(std::random_device{}()));
    return dir;
}

/// A 32-bit float WAV at 48 kHz: mono, or stereo when `right` is given.
fs::path WriteWav(const std::string& name, const std::vector<float>& left, const std::vector<float>* right = nullptr)
{
    fs::create_directories(TempDir());
    const auto path = TempDir() / name;
    std::vector<float> interleaved;

    for (std::size_t n = 0; n < left.size(); ++n)
    {
        interleaved.push_back(left[n]);

        if (right)
        {
            interleaved.push_back((*right)[n]);
        }
    }

    const std::uint16_t channels = right ? 2 : 1;
    const auto dataSize = static_cast<std::uint32_t>(interleaved.size() * sizeof(float));
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t format = 3; // IEEE float
    const auto rate = static_cast<std::uint32_t>(kSampleRate);
    const auto blockAlign = static_cast<std::uint16_t>(sizeof(float) * channels);
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
    file.write(reinterpret_cast<const char*>(interleaved.data()), dataSize);
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

/// A cab-length IR (under Standard quality's truncation): decaying noise, distinct per seed, after
/// `leading` samples of silence.
std::vector<float> DecayingNoiseIR(unsigned seed, std::size_t leading = 0)
{
    auto ir = Noise(1600, seed);

    for (std::size_t n = 0; n < ir.size(); ++n)
    {
        ir[n] = n < leading ? 0.0f : ir[n] * static_cast<float>(std::exp(-static_cast<double>(n - leading) / 300.0));
    }

    return ir;
}

struct IRFiles
{
    fs::path a = WriteWav("a.wav", DecayingNoiseIR(1));
    fs::path b = WriteWav("b.wav", DecayingNoiseIR(2));
    fs::path c = WriteWav("c.wav", DecayingNoiseIR(3));
    fs::path late = WriteWav("late.wav", DecayingNoiseIR(4, 120)); // 2.5 ms in front, for the trim
    fs::path stereo = [] {
        const auto left = DecayingNoiseIR(5);
        const auto right = DecayingNoiseIR(6);
        return WriteWav("stereo.wav", left, &right);
    }();
};

/// What each slot holds: a path, or empty for none.
struct Slots
{
    fs::path a;
    fs::path b;
};

void Load(EffectProcessor& cab, const Slots& slots)
{
    std::vector<ResourceRef> refs;
    std::vector<fs::path> paths;

    for (const auto& [path, slot] : {std::pair{slots.a, 0}, std::pair{slots.b, 1}})
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
std::unique_ptr<EffectProcessor> MakeCab(const Slots& slots, const Params& params = {})
{
    auto cab = EffectRegistry::Instance().Create("cab_ir");

    for (const auto& [key, value] : params)
    {
        cab->SetParam(key, value);
    }

    Load(*cab, slots);
    cab->Prepare(kSampleRate, kBlockSize);
    return cab;
}

/// Both output channels, left then right, for a stereo input with a different noise on each side.
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

float WorstDifference(const std::vector<float>& x, const std::vector<float>& y)
{
    float worst = x.size() == y.size() ? 0.0f : 1.0f;

    for (std::size_t n = 0; n < std::min(x.size(), y.size()); ++n)
    {
        worst = std::max(worst, std::abs(x[n] - y[n]));
    }

    return worst;
}

/// Clears both cabs' history, renders them and expects the same output, bit for bit.
bool SoundsTheSame(EffectProcessor& cab, EffectProcessor& reference, const std::string& what)
{
    cab.Reset();
    reference.Reset();
    const auto x = Render(cab);
    const auto y = Render(reference);
    const float worst = WorstDifference(x, y);
    return Expect(worst == 0.0f, what + " (worst difference " + std::to_string(worst) + ")");
}

/// IR B alone against IR A alone with the same file, with settings that only matter between two
/// IRs, and with settings that apply to whatever the cab plays.
bool TestLoneBSoundsLikeLoneA(const IRFiles& irs)
{
    std::cout << "IR B alone sounds like IR A alone" << std::endl;

    struct Case
    {
        std::string name;
        fs::path ir;
        Params shared;  // given to both cabs and the reference
        Params twoOnly; // settings for two IRs, which a lone IR ignores
    };

    const std::vector<Case> cases = {
        {"defaults", irs.a, {}, {}},
        {"IR Blend 0", irs.a, {}, {{"irBlend", 0.0}}},
        {"IR Blend 0.5", irs.a, {}, {{"irBlend", 0.5}}},
        {"IR Blend 1", irs.a, {}, {{"irBlend", 1.0}}},
        {"L/R Split", irs.a, {}, {{"lrSplit", 1.0}, {"irBlend", 0.5}}},
        {"L/R Split at IR Blend 1", irs.a, {}, {{"lrSplit", 1.0}, {"irBlend", 1.0}}},
        {"IR B Offset +2 ms", irs.a, {}, {{"slotBOffset", 2.0}, {"irBlend", 0.5}}},
        {"IR B Offset -3 ms", irs.a, {}, {{"slotBOffset", -3.0}}},
        {"Mix 0.5", irs.a, {{"mix", 0.5}}, {{"irBlend", 0.5}}},
        {"Trim Leading Silence", irs.late, {{"trimLeadingSilence", 1.0}}, {{"irBlend", 0.5}}},
        {"a stereo IR", irs.stereo, {}, {{"irBlend", 0.3}, {"lrSplit", 1.0}}},
        {"Output, Low Cut, Air, Speaker Drive",
         irs.a,
         {{"outputGain", -4.0}, {"lowCutHz", 120.0}, {"air", 0.5}, {"speakerDrive", 0.4}},
         {{"irBlend", 0.5}}},
    };

    bool ok = true;

    for (const auto& c : cases)
    {
        Params withTwoOnly = c.shared;
        withTwoOnly.insert(withTwoOnly.end(), c.twoOnly.begin(), c.twoOnly.end());

        auto reference = MakeCab({c.ir, {}}, c.shared);
        auto loneA = MakeCab({c.ir, {}}, withTwoOnly);
        auto loneB = MakeCab({{}, c.ir}, withTwoOnly);

        ok &= Expect(loneB->HasResource(), c.name + ": IR B alone is a loaded resource");
        ok &= SoundsTheSame(*loneB, *reference, c.name + ": IR B alone sounds like IR A alone");
        ok &= SoundsTheSame(*loneA, *reference, c.name + ": IR A alone sounds as it does at the defaults");
    }

    // Not a vacuous pass: the cab is heard, not the dry input.
    auto loneB = MakeCab({{}, irs.a});
    auto input = Noise(kLength, 11);
    const auto inputR = Noise(kLength, 12);
    input.insert(input.end(), inputR.begin(), inputR.end());
    ok &= Expect(WorstDifference(Render(*loneB), input) > 0.1f, "IR B alone does not pass the input through dry");
    return ok;
}

/// A lone slot plays with its own Level, Pan, Invert and Mic settings, never the other slot's.
bool TestLoneSlotKeepsItsOwnSettings(const IRFiles& irs)
{
    std::cout << "A lone IR keeps its own slot's settings" << std::endl;

    const Params slotA = {{"slotAGain", -6.0},   {"slotAPan", 0.5},   {"slotAPolarity", 1.0},
                          {"micEmulation", 1.0}, {"micRadialA", 0.6}, {"micProximityA", 0.4}};
    const Params slotB = {{"slotBGain", -6.0},   {"slotBPan", 0.5},   {"slotBPolarity", 1.0},
                          {"micEmulation", 1.0}, {"micRadialB", 0.6}, {"micProximityB", 0.4}};
    const Params otherA = {{"slotAGain", 12.0}, {"slotAPan", -1.0}, {"slotAPolarity", 1.0}, {"micRadialA", 1.0}};
    const Params otherB = {{"slotBGain", 12.0}, {"slotBPan", -1.0}, {"slotBPolarity", 1.0}, {"micRadialB", 1.0}};

    const auto join = [](Params x, const Params& y) {
        x.insert(x.end(), y.begin(), y.end());
        return x;
    };

    bool ok = true;
    auto reference = MakeCab({irs.a, {}}, slotA);
    auto loneB = MakeCab({{}, irs.a}, slotB);
    ok &= SoundsTheSame(*loneB, *reference, "IR B alone with B's settings sounds like IR A alone with A's");

    auto loneBWithA = MakeCab({{}, irs.a}, join(slotB, otherA));
    ok &= SoundsTheSame(*loneBWithA, *reference, "IR B alone ignores IR A's settings");

    auto loneAWithB = MakeCab({irs.a, {}}, join(slotA, otherB));
    ok &= SoundsTheSame(*loneAWithB, *reference, "IR A alone ignores IR B's settings");
    return ok;
}

/// A lone IR is IR Blend all the way to its slot.
bool TestLoneSlotIsBlendAllTheWay(const IRFiles& irs)
{
    std::cout << "A lone IR sounds like both at IR Blend all the way to it" << std::endl;

    bool ok = true;
    auto loneB = MakeCab({{}, irs.b});
    auto bothAtB = MakeCab({irs.a, irs.b}, {{"irBlend", 1.0}});
    ok &= SoundsTheSame(*loneB, *bothAtB, "IR B alone sounds like A and B at IR Blend 1");

    auto loneA = MakeCab({irs.a, {}}, {{"irBlend", 0.5}});
    auto bothAtA = MakeCab({irs.a, irs.b}, {{"irBlend", 0.0}});
    ok &= SoundsTheSame(*loneA, *bothAtA, "IR A alone sounds like A and B at IR Blend 0");
    return ok;
}

/// Reloading a lone B is reloading a lone A, crossfade and all: no Reset, the load mid-stream.
bool TestLoneBReloadCrossfadesAsLoneA(const IRFiles& irs)
{
    std::cout << "Reloading IR B alone crossfades as reloading IR A alone does" << std::endl;

    bool ok = true;

    for (const double blend : {0.0, 0.5, 1.0})
    {
        const std::string what = "IR Blend " + std::to_string(blend).substr(0, 3);
        auto loneA = MakeCab({irs.a, {}}, {{"irBlend", blend}});
        auto loneB = MakeCab({{}, irs.a}, {{"irBlend", blend}});
        auto x = Render(*loneA, 21);
        auto y = Render(*loneB, 21);
        Load(*loneA, {irs.c, {}});
        Load(*loneB, {{}, irs.c});
        const auto xAfter = Render(*loneA, 31);
        const auto yAfter = Render(*loneB, 31);
        x.insert(x.end(), xAfter.begin(), xAfter.end());
        y.insert(y.end(), yAfter.begin(), yAfter.end());
        const float worst = WorstDifference(x, y);
        ok &= Expect(worst == 0.0f, what + ": B alone, then another B, sounds like A alone, then another A " +
                                        "(worst difference " + std::to_string(worst) + ")");
    }

    return ok;
}

/// Loads `after` into a cab playing `before`, and expects the 30 ms crossfade between what each
/// plays alone: the outgoing side at the level it played at, so adding or clearing a slot cannot
/// dip or jump. The outgoing side carries on with the history it had at the load; the incoming
/// side, newly built, starts with none.
bool ExpectCrossfade(const Slots& before, const Slots& after, double blend, const std::string& what)
{
    const Params params = {{"irBlend", blend}};
    auto cab = MakeCab(before, params);
    Render(*cab, 41);
    Load(*cab, after);
    const auto crossfaded = Render(*cab, 51);
    auto outgoing = MakeCab(before, params);
    auto incoming = MakeCab(after, params);
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

    return Expect(worst < 1e-5, what + ": crossfades from what played at its own level (worst difference " +
                                    std::to_string(worst) + ")");
}

bool TestAddingOrClearingASlotCrossfades(const IRFiles& irs)
{
    std::cout << "Adding or clearing the other IR crossfades from what was playing" << std::endl;

    bool ok = true;

    for (const double blend : {0.0, 0.5})
    {
        const std::string at = " at IR Blend " + std::to_string(blend).substr(0, 3);
        ok &= ExpectCrossfade({{}, irs.b}, {irs.a, irs.b}, blend, "B alone, then A and B" + at);
        ok &= ExpectCrossfade({irs.a, irs.b}, {{}, irs.b}, blend, "A and B, then B alone" + at);
        ok &= ExpectCrossfade({irs.a, {}}, {irs.a, irs.b}, blend, "A alone, then A and B" + at);
        ok &= ExpectCrossfade({irs.a, irs.b}, {irs.a, {}}, blend, "A and B, then A alone" + at);
        ok &= ExpectCrossfade({irs.a, {}}, {{}, irs.b}, blend, "A alone, then B alone" + at);
        ok &= ExpectCrossfade({{}, irs.b}, {irs.c, {}}, blend, "B alone, then A alone" + at);
    }

    return ok;
}

/// Reloading the IR a cab is playing, mid-stream, should barely be heard: the outgoing side carries
/// on as it was while the incoming one fills. Measured in 1 ms windows against a cab never reloaded.
/// With the outgoing side rebuilt empty, the first window came out more than 10 dB down.
bool TestReloadWhilePlayingHasNoDip(const IRFiles& irs)
{
    std::cout << "Reloading the IR while playing has no dip" << std::endl;

    bool ok = true;
    const auto reloads = {std::pair<std::string, bool>{"LoadResources", false}, {"LoadResource", true}};

    for (const auto& [path, single] : reloads)
    {
        auto cab = MakeCab({irs.a, {}});
        auto steady = MakeCab({irs.a, {}});
        Render(*cab, 61);
        Render(*steady, 61);

        if (single)
        {
            cab->LoadResource(irs.a);
        }
        else
        {
            Load(*cab, {irs.a, {}});
        }

        const auto reloaded = Render(*cab, 71);
        const auto reference = Render(*steady, 71);
        constexpr std::size_t kWindow = 48;
        double worstDb = 0.0;

        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            for (std::size_t start = 0; start + kWindow <= kCrossfadeSamples + 480; start += kWindow)
            {
                double got = 0.0;
                double expected = 0.0;

                for (std::size_t n = start; n < start + kWindow; ++n)
                {
                    const std::size_t i = channel * kLength + n;
                    got += static_cast<double>(reloaded[i]) * reloaded[i];
                    expected += static_cast<double>(reference[i]) * reference[i];
                }

                worstDb = std::min(worstDb, 10.0 * std::log10(got / expected));
            }
        }

        ok &= Expect(worstDb > -1.0, path + ": the quietest 1 ms of the fade should be within 1 dB of the cab " +
                                         "never reloaded, but was " + std::to_string(worstDb) + " dB");
    }

    return ok;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool ok = true;
    {
        const IRFiles irs;
        ok &= TestLoneBSoundsLikeLoneA(irs);
        ok &= TestLoneSlotKeepsItsOwnSettings(irs);
        ok &= TestLoneSlotIsBlendAllTheWay(irs);
        ok &= TestLoneBReloadCrossfadesAsLoneA(irs);
        ok &= TestAddingOrClearingASlotCrossfades(irs);
        ok &= TestReloadWhilePlayingHasNoDip(irs);
    }

    std::error_code ignored;
    fs::remove_all(TempDir(), ignored);

    if (!ok)
    {
        return 1;
    }

    std::cout << "IRCabLoneSlotTests passed" << std::endl;
    return 0;
}
