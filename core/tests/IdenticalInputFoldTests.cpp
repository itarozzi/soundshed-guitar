/**
 * @file IdenticalInputFoldTests.cpp
 * @brief A Stereo input whose two sides match runs the rigs on a mono layout (IdenticalInputDetector,
 *        MultiPresetMixer::UpdateIdenticalInputFold): one amp model instead of two, with the same
 *        output.
 *
 *  - the detector folds after a second of matching signal, unfolds on the first block that
 *    differs, and counts digital silence for neither
 *  - through the mixer, a mono guitar on a stereo bus folds, and the output matches the mixer with
 *    folding off: the built-in amp and a NAM model
 *  - when the sides start to differ the rigs go stereo at once, and once the right side has faded
 *    in, the output matches the unfolded mixer again
 *  - a real stereo source never folds, through a silence included; nor does dual mono
 *  - a rig built while the input is folded goes stereo with the rest
 *  - turning folding off unfolds
 */

#include "dsp/EffectGuids.h"
#include "dsp/IdenticalInputDetector.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 64;
constexpr double kPi = 3.14159265358979323846;

int gFailures = 0;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what << "\n";
    gFailures += condition ? 0 : 1;
    return condition;
}

std::string Num(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3g", value);
    return buffer;
}

/// A plucked-string-ish guitar: a decaying note every half second, partials and a little noise,
/// so nothing about it is silent or steady.
float Guitar(std::size_t sample)
{
    const std::size_t note = sample / 24000;
    const double t = static_cast<double>(sample % 24000) / kSampleRate;
    const double f = 82.41 * std::pow(2.0, static_cast<double>(note % 5) * 5.0 / 12.0);
    double v = 0.0;

    for (int h = 1; h <= 5; ++h)
    {
        v += std::sin(2.0 * kPi * f * h * t) / (h * h);
    }

    const auto noise = static_cast<double>((sample * 2654435761u) % 1000u) / 1000.0 - 0.5;
    return static_cast<float>(0.3 * v * std::exp(-2.5 * t) + 1.0e-4 * noise);
}

Preset Chain(const std::string& id, const GraphNode& effect)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    preset.graph.nodes.push_back(effect);
    preset.graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    preset.graph.edges.push_back({"in", effect.id, 0, 0, 1.0});
    preset.graph.edges.push_back({effect.id, "out", 0, 0, 1.0});
    return preset;
}

GraphNode BuiltinAmp()
{
    GraphNode node;
    node.id = "amp";
    node.type = EffectGuids::kAmpBuiltin;
    node.category = "amp";
    node.enabled = true;
    return node;
}

GraphNode NamAmp()
{
    GraphNode node;
    node.id = "amp";
    node.type = EffectGuids::kAmpNamOptimized;
    node.category = "amp";
    node.enabled = true;
    ResourceRef ref;
    ref.resourceType = "nam";
    ref.filePath = (std::filesystem::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "TimR" /
                    "JCM800 2203 1985" / "JCM800 Hi P6 B8 M4 T7 G6.nam")
                       .string();
    node.resources.push_back(ref);
    return node;
}

/// A mixer as a DAW's stereo track has it: the host's bus, two in and two out.
struct Rig
{
    ResourceLibrary library;
    MultiPresetMixer mixer;
    std::vector<float> outLeft;
    std::vector<float> outRight;

    Rig(const Preset& preset, bool fold)
    {
        mixer.SetResourceLibrary(&library);
        mixer.SetSleepOnSilenceEnabled(false);
        mixer.SetMultiThreadedProcessingEnabled(false);
        mixer.SetHostControlledInput(true);
        mixer.SetAudioChannelCounts(2, 2);
        mixer.SetIdenticalInputFoldEnabled(fold);
        mixer.Prepare(kSampleRate, kBlock);
        mixer.AddActivePreset(preset, preset.id, preset.name);
    }

    /// One block; `right` makes the right input from the left one.
    void Block(std::size_t start, const std::function<float(std::size_t, float)>& right)
    {
        std::vector<float> inL(kBlock);
        std::vector<float> inR(kBlock);

        for (int i = 0; i < kBlock; ++i)
        {
            const std::size_t sample = start + static_cast<std::size_t>(i);
            inL[static_cast<std::size_t>(i)] = Guitar(sample);
            inR[static_cast<std::size_t>(i)] = right(sample, inL[static_cast<std::size_t>(i)]);
        }

        std::vector<float> outL(kBlock);
        std::vector<float> outR(kBlock);
        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        mixer.Process(inputs, outputs, kBlock);
        outLeft.insert(outLeft.end(), outL.begin(), outL.end());
        outRight.insert(outRight.end(), outR.begin(), outR.end());
    }
};

const auto kSame = [](std::size_t, float left) { return left; };
const auto kQuieterRight = [](std::size_t, float left) { return 0.5f * left; };

std::size_t Samples(double seconds)
{
    return static_cast<std::size_t>(seconds * kSampleRate) / kBlock * kBlock;
}

double MaxDifference(const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t to)
{
    double worst = 0.0;

    for (std::size_t i = from; i < to && i < a.size() && i < b.size(); ++i)
    {
        worst = std::max(worst, static_cast<double>(std::fabs(a[i] - b[i])));
    }

    return worst;
}

double Peak(const std::vector<float>& a, std::size_t from, std::size_t to)
{
    double peak = 0.0;

    for (std::size_t i = from; i < to && i < a.size(); ++i)
    {
        peak = std::max(peak, static_cast<double>(std::fabs(a[i])));
    }

    return peak;
}

void TestDetector()
{
    std::cout << "Detector\n";
    IdenticalInputDetector detector;
    detector.Prepare(kSampleRate);
    std::vector<float> left(kBlock);
    std::vector<float> right(kBlock);
    std::vector<float> silence(kBlock, 0.0f);

    for (int i = 0; i < kBlock; ++i)
    {
        left[static_cast<std::size_t>(i)] = Guitar(static_cast<std::size_t>(i) + 100);
    }

    right = left;
    const int secondOfBlocks = static_cast<int>(kSampleRate) / kBlock;
    bool folded = false;

    for (int block = 0; block < secondOfBlocks - 1; ++block)
    {
        folded = detector.Update(left.data(), right.data(), kBlock);
    }

    Check(!folded, "not folded before a second of matching signal");
    Check(detector.Update(left.data(), right.data(), kBlock), "folded after a second of matching signal");
    Check(detector.Update(silence.data(), silence.data(), kBlock), "digital silence leaves it folded");

    right[10] = std::nextafter(right[10], 1.0f);
    Check(!detector.Update(left.data(), right.data(), kBlock), "one sample one step apart unfolds it");

    for (int block = 0; block < 2 * secondOfBlocks; ++block)
    {
        folded = detector.Update(silence.data(), silence.data(), kBlock);
    }

    Check(!folded, "digital silence alone never folds it");
    Check(!detector.Update(left.data(), nullptr, kBlock), "a missing side never folds");
    Check(detector.Update(left.data(), left.data(), kBlock) == false, "the count started over after the gap");
}

/// A mono guitar on a stereo bus folds, matches the unfolded mixer, and unfolds when the sides
/// start to differ; after the right side's fade-in it matches the unfolded mixer again.
void TestFoldMatches(const std::string& name, const GraphNode& amp, double settledTolerance)
{
    std::cout << name << "\n";
    Rig folded(Chain("p", amp), true);
    Rig reference(Chain("p", amp), false);

    const std::size_t monoEnd = Samples(1.5);
    const std::size_t stereoEnd = Samples(2.5);
    bool foldedAtOneAndAHalf = false;
    bool unfoldedAtOnce = true;
    bool rigMonoWhileFolded = false;

    for (std::size_t start = 0; start < stereoEnd; start += kBlock)
    {
        const bool stereo = start >= monoEnd;
        folded.Block(start, stereo ? std::function<float(std::size_t, float)>(kQuieterRight) : kSame);
        reference.Block(start, stereo ? std::function<float(std::size_t, float)>(kQuieterRight) : kSame);

        if (start + kBlock == monoEnd)
        {
            foldedAtOneAndAHalf = folded.mixer.IsInputFolded();
            rigMonoWhileFolded = folded.mixer.GetRigInputLayout() == ChannelLayout::Mono;
        }

        if (start == monoEnd)
        {
            unfoldedAtOnce = !folded.mixer.IsInputFolded() && folded.mixer.GetRigInputLayout() == ChannelLayout::Stereo;
        }
    }

    Check(!reference.mixer.IsInputFolded(), "the reference never folds");
    Check(foldedAtOneAndAHalf && rigMonoWhileFolded, "matching sides fold the rig to mono");
    Check(unfoldedAtOnce, "the first differing block unfolds it");

    const double peak = Peak(reference.outLeft, 0, monoEnd);
    const double monoDiff = std::max(MaxDifference(folded.outLeft, reference.outLeft, 0, monoEnd),
                                     MaxDifference(folded.outRight, reference.outRight, 0, monoEnd));
    Check(peak > 0.01, "the amp makes sound (peak " + Num(peak) + ")");
    Check(monoDiff <= 1.0e-6 * std::max(1.0, peak),
          "folded, the output is the unfolded mixer's (max difference " + Num(monoDiff) + ")");
    Check(MaxDifference(folded.outLeft, folded.outRight, Samples(1.1), monoEnd) == 0.0,
          "folded, the two sides are identical");

    // The right side holds the left's for StereoFadeIn's hold, then ramps to its own; 300 ms on,
    // what made it has heard enough of its own input to match the reference.
    const std::size_t settled = monoEnd + Samples(0.3);
    const double leftDiff = MaxDifference(folded.outLeft, reference.outLeft, monoEnd, stereoEnd);
    const double rightDiff = MaxDifference(folded.outRight, reference.outRight, settled, stereoEnd);
    Check(leftDiff <= 1.0e-6 * std::max(1.0, peak),
          "unfolded, the left side never left the reference (" + Num(leftDiff) + ")");
    Check(rightDiff <= settledTolerance * peak,
          "unfolded, the right side is back on the reference within 300 ms (" + Num(rightDiff / peak) + " of peak)");

    double worstStep = 0.0;
    double referenceStep = 0.0;

    // Over the change the right side carries the left's output, then crosses to its own: no step
    // in it may be bigger than the reference's own largest, on either side.
    for (std::size_t i = monoEnd - 1; i + 1 < stereoEnd; ++i)
    {
        worstStep = std::max(worstStep, static_cast<double>(std::fabs(folded.outRight[i + 1] - folded.outRight[i])));
        referenceStep =
            std::max({referenceStep, static_cast<double>(std::fabs(reference.outRight[i + 1] - reference.outRight[i])),
                      static_cast<double>(std::fabs(reference.outLeft[i + 1] - reference.outLeft[i]))});
    }

    Check(worstStep <= 1.1 * referenceStep, "the right side's change of source does not click (largest step " +
                                                Num(worstStep) + " against " + Num(referenceStep) + ")");
}

void TestNeverFolds()
{
    std::cout << "Sources that never fold\n";

    {
        Rig rig(Chain("p", BuiltinAmp()), true);
        bool everFolded = false;

        // Stereo, then digital silence on both sides, then stereo again.
        std::vector<float> zeros(kBlock, 0.0f);
        std::vector<float> outL(kBlock);
        std::vector<float> outR(kBlock);

        for (std::size_t start = 0; start < Samples(4.0); start += kBlock)
        {
            if (start >= Samples(1.0) && start < Samples(2.5))
            {
                float* inputs[2] = {zeros.data(), zeros.data()};
                float* outputs[2] = {outL.data(), outR.data()};
                rig.mixer.Process(inputs, outputs, kBlock);
            }
            else
            {
                rig.Block(start, kQuieterRight);
            }

            everFolded = everFolded || rig.mixer.IsInputFolded();
        }

        Check(!everFolded, "a stereo source never folds, through a silence included");
    }

    {
        Rig rig(Chain("p", BuiltinAmp()), true);
        rig.mixer.SetDualMono(true);
        bool everFolded = false;

        for (std::size_t start = 0; start < Samples(2.0); start += kBlock)
        {
            rig.Block(start, kSame);
            everFolded = everFolded || rig.mixer.IsInputFolded();
        }

        Check(!everFolded, "dual mono never folds");
    }
}

void TestFoldStaysThroughSilenceAndCanBeTurnedOff()
{
    std::cout << "Silence and the switch\n";
    Rig rig(Chain("p", BuiltinAmp()), true);
    std::size_t start = 0;

    for (; start < Samples(1.5); start += kBlock)
    {
        rig.Block(start, kSame);
    }

    std::vector<float> zeros(kBlock, 0.0f);
    std::vector<float> outL(kBlock);
    std::vector<float> outR(kBlock);
    float* inputs[2] = {zeros.data(), zeros.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    for (int block = 0; block < static_cast<int>(kSampleRate) / kBlock; ++block)
    {
        rig.mixer.Process(inputs, outputs, kBlock);
    }

    Check(rig.mixer.IsInputFolded(), "a second of digital silence leaves a mono guitar folded");

    rig.mixer.SetIdenticalInputFoldEnabled(false);
    rig.Block(start, kSame);
    Check(!rig.mixer.IsInputFolded() && rig.mixer.GetRigInputLayout() == ChannelLayout::Stereo,
          "turning folding off unfolds at the next block");
}

void TestRigBuiltWhileFolded()
{
    std::cout << "A rig built while folded\n";
    Rig rig(Chain("p", BuiltinAmp()), true);
    std::size_t start = 0;

    for (; start < Samples(1.5); start += kBlock)
    {
        rig.Block(start, kSame);
    }

    Check(rig.mixer.IsInputFolded(), "folded before the rig is replaced");
    rig.mixer.ReplaceActivePresetInPlace(Chain("p", BuiltinAmp()), "p", "p");

    const std::size_t unfoldAt = start + Samples(0.5);

    for (; start < unfoldAt + Samples(1.0); start += kBlock)
    {
        rig.Block(start, start >= unfoldAt ? std::function<float(std::size_t, float)>(kQuieterRight) : kSame);
    }

    // The right input is half the left: an amp driven half as hard puts out something else.
    const double sideDiff = MaxDifference(rig.outLeft, rig.outRight, unfoldAt + Samples(0.5), start);
    Check(!rig.mixer.IsInputFolded() && sideDiff > 1.0e-3,
          "the new rig runs stereo once the sides differ (sides differ by " + Num(sideDiff) + ")");
}
} // namespace

int main()
{
    std::cout << std::unitbuf;
    RegisterAllEffects();

    TestDetector();
    TestFoldMatches("Built-in amp", BuiltinAmp(), 1.0e-3);
    TestFoldMatches("NAM amp", NamAmp(), 1.0e-4);
    TestNeverFolds();
    TestFoldStaysThroughSilenceAndCanBeTurnedOff();
    TestRigBuiltWhileFolded();

    std::cout << (gFailures == 0 ? "IdenticalInputFoldTests PASSED\n" : "IdenticalInputFoldTests FAILED\n");
    return gFailures == 0 ? 0 : 1;
}
