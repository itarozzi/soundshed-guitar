/**
 * @file BlendEffectTests.cpp
 * @brief MultiModelNAMAmpEffect mixes its models without cuts, and runs only those it plays.
 *
 * The models are generated Linear NAMs that scale their input, fed a constant input, so the
 * output level at each sample is the mix itself:
 *  - a change of model fades rather than cuts, even in snap mode;
 *  - a model that was not running is warmed up unheard before it fades in;
 *  - a knob on a captured setting runs one model, between two runs two, and a model that
 *    has faded out stops running;
 *  - a target for a parameter no model was captured at changes nothing;
 *  - the node's blendModeOverride wins over the definition's blendMode, and "" follows it.
 */

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/FiniteCheck.h"
#include "dsp/effects/MultiModelNAMAmpEffect.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr float kInput = 0.1f;
constexpr float kQuietGain = 0.2f;
constexpr float kLoudGain = 0.6f;

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

fs::path WriteLinearModel(const fs::path& dir, const std::string& name, float gain)
{
    const nlohmann::json model = {{"version", "0.5.4"},
                                  {"architecture", "Linear"},
                                  {"config", {{"receptive_field", 4}, {"bias", false}}},
                                  {"weights", {gain, 0.0f, 0.0f, 0.0f}},
                                  {"sample_rate", kSampleRate},
                                  {"metadata", nlohmann::json::object()}};

    const fs::path path = dir / (name + ".nam");
    std::ofstream(path) << model.dump();
    return path;
}

ResourceRef CapturedAt(double gain)
{
    ResourceRef ref;
    ref.resourceType = "nam";
    ref.parameterId = "gain";
    ref.parameterValue = gain;
    ref.parameters["gain"] = gain;
    return ref;
}

/// A quiet model captured at gain 0 and a loud one at gain 1, prepared and loaded.
struct Fixture
{
    MultiModelNAMAmpEffect effect;

    Fixture(const fs::path& quiet, const fs::path& loud, const std::string& blendMode)
    {
        effect.SetConfig("blendMode", blendMode);
        effect.SetParam("useCalibration", 0.0);
        effect.Prepare(kSampleRate, kBlock);
        effect.LoadResources({CapturedAt(0.0), CapturedAt(1.0)}, {quiet, loud});
    }

    /// Runs `blocks` blocks of constant input and returns every output sample, as a
    /// gain over the input.
    std::vector<float> Run(int blocks)
    {
        std::vector<float> in(kBlock, kInput), out(kBlock), gains;
        gains.reserve(static_cast<size_t>(blocks * kBlock));

        for (int b = 0; b < blocks; ++b)
        {
            effect.ProcessMono(in.data(), out.data(), kBlock);

            for (const float sample : out)
            {
                gains.push_back(sample / kInput);
            }
        }

        return gains;
    }

    double Settle()
    {
        const auto gains = Run(static_cast<int>(0.5 * kSampleRate / kBlock));
        return gains.back();
    }
};

int BlocksFor(double seconds)
{
    return static_cast<int>(std::ceil(seconds * kSampleRate / kBlock));
}

float LargestStep(const std::vector<float>& gains)
{
    float largest = 0.0f;

    for (std::size_t i = 1; i < gains.size(); ++i)
    {
        largest = std::max(largest, std::abs(gains[i] - gains[i - 1]));
    }

    return largest;
}

bool AllFinite(const std::vector<float>& gains)
{
    return std::all_of(gains.begin(), gains.end(), [](float g) { return IsFinite(g); });
}

std::string Describe(double value)
{
    return std::to_string(value).substr(0, 6);
}

bool TestSnapSwitchFades(const fs::path& quiet, const fs::path& loud)
{
    Fixture fixture(quiet, loud, "snap");
    fixture.effect.SetParam("gain", 0.0);
    const double before = fixture.Settle();

    fixture.effect.SetParam("gain", 1.0);
    const auto during = fixture.Run(BlocksFor(0.03));
    const auto rest = fixture.Run(BlocksFor(0.4));

    bool passed =
        Check(std::abs(before - kQuietGain) < 0.01, "snap: settles on the quiet model (" + Describe(before) + ")");
    passed = Check(std::abs(during.back() - before) < 0.01,
                   "snap: the new model is not heard during its warm-up (" + Describe(during.back()) + ")") &&
             passed;
    passed = Check(std::abs(rest.back() - kLoudGain) < 0.01,
                   "snap: then the loud model takes over (" + Describe(rest.back()) + ")") &&
             passed;

    // Unramped, the change is 0.4 of the input in one sample.
    std::vector<float> all = during;
    all.insert(all.end(), rest.begin(), rest.end());
    const float step = LargestStep(all);
    passed =
        Check(step < 0.002f, "snap: the change is a fade, not a cut (largest step " + Describe(step) + ")") && passed;
    passed = Check(AllFinite(all), "snap: output stays finite") && passed;
    passed = Check(fixture.effect.GetRunningModelCount() == 1, "snap: the faded-out model stops running") && passed;
    return passed;
}

bool TestRunningModels(const fs::path& quiet, const fs::path& loud)
{
    Fixture fixture(quiet, loud, "interpolate");
    fixture.effect.SetParam("gain", 1.0);
    fixture.Settle();
    const auto onCapture = fixture.effect.GetRunningModelCount();

    fixture.effect.SetParam("gain", 0.5);
    const double between = fixture.Settle();
    const auto betweenCount = fixture.effect.GetRunningModelCount();

    bool passed = Check(onCapture == 1,
                        "running: a knob on a captured setting runs one model (" + std::to_string(onCapture) + ")");
    passed = Check(betweenCount == 2, "running: a knob between two runs both (" + std::to_string(betweenCount) + ")") &&
             passed;
    passed = Check(std::abs(between - 0.5 * (kQuietGain + kLoudGain)) < 0.01,
                   "running: halfway mixes them evenly (" + Describe(between) + ")") &&
             passed;
    return passed;
}

bool TestUnmappedParameterIgnored(const fs::path& quiet, const fs::path& loud)
{
    Fixture fixture(quiet, loud, "interpolate");
    fixture.effect.SetParam("gain", 0.3);
    const double before = fixture.Settle();

    // A parameter since dropped from the blend: no model was captured at it.
    fixture.effect.SetParam("bass", 0.9);
    const double after = fixture.Settle();

    return Check(std::abs(after - before) < 1e-4,
                 "unmapped: a stale target leaves the mix alone (" + Describe(before) + " -> " + Describe(after) + ")");
}

bool TestBlendModeOverride(const fs::path& quiet, const fs::path& loud)
{
    Fixture fixture(quiet, loud, "interpolate");
    fixture.effect.SetParam("gain", 0.3);
    const double interpolated = fixture.Settle();

    fixture.effect.SetConfig("blendModeOverride", "snap");
    const double snapped = fixture.Settle();

    fixture.effect.SetConfig("blendModeOverride", "");
    const double followed = fixture.Settle();

    bool passed = Check(interpolated > kQuietGain + 0.02 && interpolated < kLoudGain,
                        "override: the definition interpolates (" + Describe(interpolated) + ")");
    passed = Check(std::abs(snapped - kQuietGain) < 0.01,
                   "override: snap on the node plays the nearest model (" + Describe(snapped) + ")") &&
             passed;
    passed = Check(std::abs(followed - interpolated) < 1e-3,
                   "override: clearing it follows the definition again (" + Describe(followed) + ")") &&
             passed;
    return passed;
}

bool TestStereoMatchesMono(const fs::path& quiet, const fs::path& loud)
{
    Fixture fixture(quiet, loud, "interpolate");
    fixture.effect.SetParam("gain", 0.5);

    std::vector<float> inL(kBlock, kInput), inR(kBlock, kInput), outL(kBlock), outR(kBlock);
    float* inputs[] = {inL.data(), inR.data()};
    float* outputs[] = {outL.data(), outR.data()};

    for (int b = 0; b < BlocksFor(0.5); ++b)
    {
        fixture.effect.Process(inputs, outputs, kBlock);
    }

    const double left = outL.back() / kInput;
    const double right = outR.back() / kInput;
    const double expected = 0.5 * (kQuietGain + kLoudGain);
    return Check(std::abs(left - expected) < 0.01 && std::abs(right - expected) < 0.01,
                 "stereo: both channels carry the mix (" + Describe(left) + ", " + Describe(right) + ")");
}

bool Run()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-blend-effect-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);

    const fs::path quiet = WriteLinearModel(sandbox, "linear-quiet", kQuietGain);
    const fs::path loud = WriteLinearModel(sandbox, "linear-loud", kLoudGain);

    bool passed = TestSnapSwitchFades(quiet, loud);
    passed = TestRunningModels(quiet, loud) && passed;
    passed = TestUnmappedParameterIgnored(quiet, loud) && passed;
    passed = TestBlendModeOverride(quiet, loud) && passed;
    passed = TestStereoMatchesMono(quiet, loud) && passed;

    fs::remove_all(sandbox, ec);
    return passed;
}
} // namespace

int main()
{
    std::cout << std::unitbuf;
    const bool passed = Run();
    std::cout << (passed ? "BlendEffectTests PASSED\n" : "BlendEffectTests FAILED\n");
    return passed ? 0 : 1;
}
