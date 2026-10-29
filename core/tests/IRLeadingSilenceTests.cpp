/**
 * IRLeadingSilenceTests.cpp — the IR Cabinet's Trim Leading Silence.
 *
 * An IR's onset is its first sample within -40 dB of its peak, in either channel. With the trim on,
 * the cab drops everything before it but a few samples, so the IR's own pre-delay stops adding
 * latency. Its response to an impulse is then the untrimmed cab's response moved earlier by exactly
 * that many samples, and nothing else about it changes.
 *
 * Two slots lose the same amount, the earlier slot's onset, so IR B keeps its place against IR A:
 * the blended response is still a pure shift of the untrimmed one, with an IR B offset set and with
 * the two IRs at different rates. Quality's length is counted from the onset rather than the file's
 * first sample. Off is the default, and builds exactly what the cab built before the trim existed.
 * A change on a loaded cab waits for the message thread like Normalize.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "dsp/EffectRegistry.h"
#include "dsp/IrLeadingSilence.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "NonFiniteInputs.h"

using namespace guitarfx;

namespace
{
namespace fs = std::filesystem;

constexpr double kHostRate = 48000.0;
constexpr int kBlockSize = 64;
constexpr std::size_t kRenderLength = 64 * 96;
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kMargin = ir_leading_silence::kMarginSamples;

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
        fs::temp_directory_path() / ("guitarfx_ir_leading_silence_tests_" + std::to_string(std::random_device{}()));
    return dir;
}

/// A 32-bit float WAV of one or two channels at `rate`.
fs::path WriteWav(const std::string& name, const std::vector<float>& left, const std::vector<float>& right = {},
                  double rate = kHostRate)
{
    fs::create_directories(TempDir());
    const auto path = TempDir() / name;
    const std::uint16_t channels = right.empty() ? 1 : 2;
    std::vector<float> interleaved;

    for (std::size_t n = 0; n < left.size(); ++n)
    {
        interleaved.push_back(left[n]);

        if (channels == 2)
        {
            interleaved.push_back(n < right.size() ? right[n] : 0.0f);
        }
    }

    const auto dataSize = static_cast<std::uint32_t>(interleaved.size() * sizeof(float));
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t format = 3; // IEEE float
    const auto sampleRate = static_cast<std::uint32_t>(rate);
    const std::uint32_t byteRate = sampleRate * channels * sizeof(float);
    const auto blockAlign = static_cast<std::uint16_t>(channels * sizeof(float));
    const std::uint16_t bitsPerSample = 32;

    std::ofstream file(path, std::ios::binary);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    file.write(reinterpret_cast<const char*>(&format), 2);
    file.write(reinterpret_cast<const char*>(&channels), 2);
    file.write(reinterpret_cast<const char*>(&sampleRate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
    file.write(reinterpret_cast<const char*>(interleaved.data()), dataSize);
    return path;
}

fs::path WriteMonoWav(const std::string& name, const std::vector<float>& samples)
{
    return WriteWav(name, samples);
}

/// A cabinet-like IR at `rate`: silence until `onset` samples, then a dozen decaying partials
/// between 150 Hz and 5 kHz, each starting at its peak, so the onset is exactly `onset`. `seed`
/// picks the partials, so two IRs differ.
std::vector<float> CabLikeIR(std::size_t onset, std::size_t length, unsigned seed = 1234, double rate = kHostRate)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> frequency(150.0, 5000.0);
    std::uniform_real_distribution<double> amplitude(0.2, 1.0);
    std::uniform_real_distribution<double> decayMs(1.0, 8.0);
    std::vector<float> ir(length, 0.0f);

    for (int k = 0; k < 12; ++k)
    {
        const double hz = frequency(rng);
        const double gain = amplitude(rng);
        const double decaySeconds = decayMs(rng) / 1000.0;

        for (std::size_t n = onset; n < length; ++n)
        {
            const double t = static_cast<double>(n - onset) / rate;
            ir[n] += static_cast<float>(gain * std::exp(-t / decaySeconds) * std::cos(2.0 * kPi * hz * t));
        }
    }

    return ir;
}

struct CabSettings
{
    bool trim = false;
    double offsetMs = 0.0;
    double normalize = 1.0;
    double quality = 1.0; // Standard
};

ResourceRef IRRef(const fs::path& path, std::size_t slot)
{
    ResourceRef ir;
    ir.resourceType = "ir";
    ir.filePath = path;
    ir.metadata["resourceSlotIndex"] = std::to_string(slot);
    return ir;
}

/// A cab with IR A and, if given, IR B half and half, loaded the way a preset loads one: params
/// first, then the IRs, then Prepare.
std::unique_ptr<EffectProcessor> MakeCab(const fs::path& irA, const fs::path& irB, const CabSettings& settings)
{
    auto cab = EffectRegistry::Instance().Create("cab_ir");
    cab->SetParam("irBlend", 0.5);
    cab->SetParam("normalizeIR", settings.normalize);
    cab->SetParam("quality", settings.quality);
    cab->SetParam("slotBOffset", settings.offsetMs);
    cab->SetParam("trimLeadingSilence", settings.trim ? 1.0 : 0.0);

    std::vector<ResourceRef> refs{IRRef(irA, 0)};
    std::vector<fs::path> paths{irA};

    if (!irB.empty())
    {
        refs.push_back(IRRef(irB, 1));
        paths.push_back(irB);
    }

    cab->LoadResources(refs, paths);
    cab->Prepare(kHostRate, kBlockSize);
    return cab;
}

struct CabResponse
{
    std::vector<float> left;
    std::vector<float> right;
};

/// The cab's response to a unit impulse on both inputs, from a reset.
CabResponse ImpulseResponse(EffectProcessor& cab)
{
    cab.Reset();
    CabResponse response{std::vector<float>(kRenderLength), std::vector<float>(kRenderLength)};
    std::vector<float> in(kBlockSize, 0.0f);

    for (std::size_t start = 0; start < kRenderLength; start += kBlockSize)
    {
        std::fill(in.begin(), in.end(), 0.0f);
        in[0] = start == 0 ? 1.0f : 0.0f;
        float* inputs[2] = {in.data(), in.data()};
        float* outputs[2] = {response.left.data() + start, response.right.data() + start};
        cab.Process(inputs, outputs, kBlockSize);
    }

    return response;
}

/// The first sample at least -40 dB from the peak, where the response is heard to start.
std::size_t Onset(const std::vector<float>& x)
{
    return ir_leading_silence::FindOnsetSample(x, {}).value_or(x.size());
}

/// Whether `trimmed` is `untrimmed` moved `shift` samples earlier, to rounding, and not silence.
bool IsShiftOf(const std::vector<float>& trimmed, const std::vector<float>& untrimmed, std::size_t shift,
               const std::string& what)
{
    float peak = 0.0f;
    float worst = 0.0f;

    for (std::size_t n = 0; n + shift < untrimmed.size(); ++n)
    {
        peak = std::max(peak, std::abs(untrimmed[n + shift]));
        worst = std::max(worst, std::abs(trimmed[n] - untrimmed[n + shift]));
    }

    return Expect(peak > 0.01f && worst <= 1e-5f * peak, what + " is the untrimmed response " + std::to_string(shift) +
                                                             " samples earlier (worst " + std::to_string(worst) +
                                                             " of peak " + std::to_string(peak) + ")");
}

bool SameResponse(EffectProcessor& a, EffectProcessor& b, const std::string& what)
{
    const auto x = ImpulseResponse(a);
    const auto y = ImpulseResponse(b);
    return Expect(x.left == y.left && x.right == y.right, what);
}

// ── Finding the onset ─────────────────────────────────────────────────────

bool TestFindsOnset()
{
    bool ok = true;
    std::vector<float> ir(200, 0.0f);
    ir[40] = 0.009f; // under -40 dB of the peak: still lead-in
    ir[41] = 0.02f;  // the onset
    ir[60] = -2.0f;  // the peak, negative
    ok &= Expect(ir_leading_silence::FindOnsetSample(ir, {}) == std::size_t{41},
                 "the onset is the first sample within -40 dB of the peak, whatever its sign");

    std::vector<float> right(200, 0.0f);
    right[30] = 0.5f;
    ok &= Expect(ir_leading_silence::FindOnsetSample(ir, right) == std::size_t{30},
                 "a stereo IR's onset is the earlier channel's, against the louder channel's peak");

    ok &= Expect(!ir_leading_silence::FindOnsetSample(std::vector<float>(64, 0.0f), {}), "a silent IR has no onset");
    ok &= Expect(!ir_leading_silence::FindOnsetSample({}, {}), "an empty IR has no onset");

    std::vector<float> withBadSample(100, 0.0f);
    withBadSample[5] = test_support::RuntimeInfinityf();
    withBadSample[70] = 1.0f;
    ok &= Expect(ir_leading_silence::FindOnsetSample(withBadSample, {}) == std::size_t{70},
                 "a non-finite sample counts as silence");

    ok &= Expect(ir_leading_silence::SharedOnsetSeconds(0.002, 0.001) == 0.001 &&
                     ir_leading_silence::SharedOnsetSeconds(std::nullopt, 0.003) == 0.003 &&
                     ir_leading_silence::SharedOnsetSeconds(std::nullopt, std::nullopt) == 0.0,
                 "two slots share the earlier onset, and an empty slot has no say");
    ok &= Expect(ir_leading_silence::TrimSamples(96.0 / kHostRate, kHostRate) == 96 - kMargin &&
                     ir_leading_silence::TrimSamples(3.0 / kHostRate, kHostRate) == 0,
                 "the trim stops the margin short of the onset, and never goes negative");
    return ok;
}

// ── The cab ───────────────────────────────────────────────────────────────

bool TestTrimsToTheOnset()
{
    constexpr std::size_t kOnset = 96; // 2 ms of pre-delay
    const auto path = WriteMonoWav("predelay.wav", CabLikeIR(kOnset, 1600));
    auto off = ImpulseResponse(*MakeCab(path, {}, {}));
    auto on = ImpulseResponse(*MakeCab(path, {}, {.trim = true}));

    bool ok = Expect(Onset(off.left) == kOnset, "untrimmed, the cab's response starts at the IR's onset (" +
                                                    std::to_string(Onset(off.left)) + ")");
    ok &= Expect(Onset(on.left) == kMargin,
                 "trimmed, it starts the margin in (" + std::to_string(Onset(on.left)) + " samples)");
    ok &= IsShiftOf(on.left, off.left, kOnset - kMargin, "the trimmed response");
    return ok;
}

bool TestStereoIRTrimsBothChannelsTogether()
{
    // The right channel starts 40 samples before the left; both lose the right's lead-in.
    const auto path = WriteWav("stereo.wav", CabLikeIR(100, 1600, 1), CabLikeIR(60, 1600, 2));
    auto off = ImpulseResponse(*MakeCab(path, {}, {}));
    auto on = ImpulseResponse(*MakeCab(path, {}, {.trim = true}));
    bool ok = IsShiftOf(on.left, off.left, 60 - kMargin, "a stereo IR's left channel");
    ok &= IsShiftOf(on.right, off.right, 60 - kMargin, "its right channel");
    return ok;
}

bool TestTwoSlotsKeepTheirAlignment()
{
    bool ok = true;
    // B 1.5 ms after A, then B before A: either way both lose the earlier one's lead-in.
    const auto earlyA = WriteMonoWav("early-a.wav", CabLikeIR(48, 1600, 3));
    const auto lateB = WriteMonoWav("late-b.wav", CabLikeIR(120, 1600, 4));
    const auto lateA = WriteMonoWav("late-a.wav", CabLikeIR(150, 1600, 5));
    const auto earlyB = WriteMonoWav("early-b.wav", CabLikeIR(30, 1600, 6));

    for (const double offsetMs : {0.0, 0.37, -1.0})
    {
        const std::string suffix = " with IR B offset " + std::to_string(offsetMs) + " ms";
        auto off = ImpulseResponse(*MakeCab(earlyA, lateB, {.offsetMs = offsetMs}));
        auto on = ImpulseResponse(*MakeCab(earlyA, lateB, {.trim = true, .offsetMs = offsetMs}));
        ok &= IsShiftOf(on.left, off.left, 48 - kMargin, "A earlier than B" + suffix);

        off = ImpulseResponse(*MakeCab(lateA, earlyB, {.offsetMs = offsetMs}));
        on = ImpulseResponse(*MakeCab(lateA, earlyB, {.trim = true, .offsetMs = offsetMs}));
        ok &= IsShiftOf(on.left, off.left, 30 - kMargin, "B earlier than A" + suffix);
    }

    return ok;
}

bool TestSlotsAtDifferentRatesKeepTheirAlignment()
{
    // A at 44.1 kHz with its onset 1.36 ms in, B at 48 kHz 2.5 ms in. Both are trimmed at the host's
    // rate after A is resampled, so both move by exactly the same time. Normalize is off because
    // resampling leaves a little ringing in front of A's onset, which the trim takes and which
    // would move Normalize's gain by a hair; the shift is what is being checked.
    const auto a = WriteWav("a-44k.wav", CabLikeIR(60, 1500, 7, 44100.0), {}, 44100.0);
    const auto b = WriteMonoWav("b-48k.wav", CabLikeIR(120, 1600, 8));
    const std::size_t trim = ir_leading_silence::TrimSamples(60.0 / 44100.0, kHostRate);
    auto off = ImpulseResponse(*MakeCab(a, b, {.normalize = 0.0}));
    auto on = ImpulseResponse(*MakeCab(a, b, {.trim = true, .normalize = 0.0}));
    bool ok = Expect(trim == 61, "a 44.1 kHz onset of 60 samples is 65.3 at 48 kHz, so 61 come off");
    ok &= IsShiftOf(on.left, off.left, trim, "two slots at different rates");
    return ok;
}

bool TestQualityCountsFromTheOnset()
{
    // Economy keeps 10 ms, 480 samples, and fades the last 64 out. A slowly decaying tail outlasts
    // that, so the length binds. Normalize is off and the convolver has no latency, so the cab's
    // response is the IR's own samples.
    constexpr std::size_t kOnset = 144;
    constexpr std::size_t kEconomy = 480;
    constexpr std::size_t kUnfaded = kEconomy - 64;
    constexpr std::size_t kTrim = kOnset - kMargin;
    std::vector<float> ir(kOnset + 2000, 0.0f);
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);

    for (std::size_t n = kOnset; n < ir.size(); ++n)
    {
        ir[n] = noise(rng) * std::exp(-static_cast<float>(n - kOnset) / 4000.0f);
    }

    ir[kOnset] = 1.0f;
    const auto path = WriteMonoWav("long-tail.wav", ir);
    const auto first = [](const std::vector<float>& x, std::size_t count) {
        return std::vector<float>(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(count));
    };
    const auto loudestFrom = [](const std::vector<float>& x, std::size_t start) {
        float loudest = 0.0f;

        for (std::size_t n = start; n < x.size(); ++n)
        {
            loudest = std::max(loudest, std::abs(x[n]));
        }

        return loudest;
    };

    auto offCab = MakeCab(path, {}, {.normalize = 0.0, .quality = 0.0});
    auto onCab = MakeCab(path, {}, {.trim = true, .normalize = 0.0, .quality = 0.0});
    bool ok = true;
    const auto off = ImpulseResponse(*offCab);
    const auto on = ImpulseResponse(*onCab);

    ok &= IsShiftOf(first(off.left, kUnfaded), first(ir, kUnfaded), 0,
                    "untrimmed, Economy's 480 samples counted from the file's first, up to the fade,");
    ok &= IsShiftOf(first(on.left, kUnfaded), first(ir, kUnfaded + kTrim), kTrim,
                    "trimmed, Economy's 480 samples counted from the onset, up to the fade,");
    ok &= Expect(loudestFrom(off.left, kEconomy) < 1e-5f && loudestFrom(on.left, kEconomy) < 1e-5f,
                 "either way the cab keeps 480 samples");
    return ok;
}

bool TestOffIsTheDefaultAndChangesNothing()
{
    bool ok = true;
    const auto predelay = WriteMonoWav("off-predelay.wav", CabLikeIR(96, 1600, 10));
    const auto tight = WriteMonoWav("off-tight.wav", CabLikeIR(0, 1600, 11));
    const auto other = WriteMonoWav("off-other.wav", CabLikeIR(40, 1600, 12));

    ok &= Expect(EffectRegistry::Instance().Create("cab_ir")->GetParam("trimLeadingSilence") == 0.0,
                 "the trim is off by default");

    // A cab that never hears of the trim, as a preset from before it existed, and one told it is off.
    auto untouched = EffectRegistry::Instance().Create("cab_ir");
    untouched->SetParam("irBlend", 0.5);
    untouched->LoadResources({IRRef(predelay, 0), IRRef(other, 1)}, {predelay, other});
    untouched->Prepare(kHostRate, kBlockSize);
    ok &= SameResponse(*untouched, *MakeCab(predelay, other, {}), "off sounds exactly like the cab before the trim");

    // Nothing in front of the onset: on and off build the same IR, bit for bit.
    ok &= SameResponse(*MakeCab(tight, {}, {.trim = true}), *MakeCab(tight, {}, {}),
                       "with no leading silence, the trim changes nothing");
    return ok;
}

bool TestLiveIRChangeRetrims()
{
    bool ok = true;
    const auto a = WriteMonoWav("live-a.wav", CabLikeIR(100, 1600, 13));
    const auto earlyB = WriteMonoWav("live-early-b.wav", CabLikeIR(20, 1600, 14));
    const auto lateB = WriteMonoWav("live-late-b.wav", CabLikeIR(200, 1600, 15));

    // A prepared cab builds each IR as it loads. A's trim has to follow the B it is loaded with,
    // not the B it replaces.
    auto cab = MakeCab(a, lateB, {.trim = true});
    cab->LoadResources({IRRef(a, 0), IRRef(earlyB, 1)}, {a, earlyB});
    ok &= SameResponse(*cab, *MakeCab(a, earlyB, {.trim = true}), "an earlier IR B loaded later re-trims IR A");

    cab->LoadResources({IRRef(a, 0), IRRef(lateB, 1)}, {a, lateB});
    ok &= SameResponse(*cab, *MakeCab(a, lateB, {.trim = true}), "a later IR B loaded later re-trims IR A");
    return ok;
}

bool TestChangeWaitsForTheMessageThread()
{
    bool ok = true;
    const auto a = WriteMonoWav("deferred-a.wav", CabLikeIR(80, 1600, 16));
    const auto b = WriteMonoWav("deferred-b.wav", CabLikeIR(130, 1600, 17));
    auto cab = MakeCab(a, b, {});
    auto untrimmed = MakeCab(a, b, {});

    // SetParam can run on the audio thread, so it only records the change.
    cab->SetParam("trimLeadingSilence", 1.0);
    ok &= Expect(cab->GetParam("trimLeadingSilence") == 1.0, "GetParam reports the change straight away");
    ok &= SameResponse(*cab, *untrimmed, "nothing is rebuilt before the message thread");

    auto work = cab->TakeDeferredRebuild();
    ok &= Expect(work != nullptr, "the change waits for the message thread");

    if (work)
    {
        work->Build();
        cab->CommitDeferredRebuild(*work);
    }

    ok &= Expect(!cab->TakeDeferredRebuild(), "nothing is left waiting once it is built");
    ok &= SameResponse(*cab, *MakeCab(a, b, {.trim = true}),
                       "the rebuilt cab sounds exactly like one built with the trim on");

    // And back off again.
    cab->SetParam("trimLeadingSilence", 0.0);
    work = cab->TakeDeferredRebuild();

    if (work)
    {
        work->Build();
        cab->CommitDeferredRebuild(*work);
    }

    ok &= Expect(work != nullptr && cab->GetParam("trimLeadingSilence") == 0.0, "turning it off rebuilds too");
    ok &= SameResponse(*cab, *untrimmed, "turned off again, the cab sounds as it did");
    return ok;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool ok = true;
    ok &= TestFindsOnset();
    ok &= TestTrimsToTheOnset();
    ok &= TestStereoIRTrimsBothChannelsTogether();
    ok &= TestTwoSlotsKeepTheirAlignment();
    ok &= TestSlotsAtDifferentRatesKeepTheirAlignment();
    ok &= TestQualityCountsFromTheOnset();
    ok &= TestOffIsTheDefaultAndChangesNothing();
    ok &= TestLiveIRChangeRetrims();
    ok &= TestChangeWaitsForTheMessageThread();

    std::error_code ignored;
    fs::remove_all(TempDir(), ignored);

    if (!ok)
    {
        std::cout << "IRLeadingSilenceTests FAILED" << std::endl;
        return 1;
    }

    std::cout << "IRLeadingSilenceTests passed" << std::endl;
    return 0;
}
