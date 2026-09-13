/**
 * @file BoundaryNodeGainTests.cpp
 * @brief The Input and Output nodes' gain knobs must change what the engine plays.
 *
 * The same sine is measured at three depths -- a bare SignalGraphExecutor, a
 * MultiPresetMixer, and a PluginController driven with the messages the UI sends --
 * so a failure names the layer that dropped the gain. A unity gain effect sits between
 * the boundary nodes and has its own gain moved the same way as a control: if that one
 * moves and the boundary nodes do not, the fault is theirs alone.
 */

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr double kSineHz = 220.0;
constexpr float kSineAmplitude = 0.05f; // -26 dBFS, well clear of any output protection
constexpr double kToleranceDb = 0.25;
constexpr const char* kPresetId = "boundary-gain-preset";

// Long enough to outlast a swap crossfade and an outgoing preset's tail.
constexpr int kSettleBlocks = 1000;
constexpr int kStepBlocks = 40;
constexpr int kMeasureBlocks = 20;

using ProcessFn = std::function<bool(float**, float**, int)>;

class TestHost final : public IPluginHost
{
  public:
    explicit TestHost(fs::path userDataPath) : mUserDataPath(std::move(userDataPath))
    {
    }

    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(BrowseFileType, const std::string&,
                         std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)> callback) override
    {
        callback(BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mUserDataPath;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return kSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return kBlock;
    }

  private:
    fs::path mUserDataPath;
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

/// input -> unity gain -> output, with the boundary nodes stored first the way the app
/// saves them.
Preset BuildPreset(double inputGainDb, double outputGainDb)
{
    Preset preset;
    preset.id = kPresetId;
    preset.name = "Boundary gain";
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "__input__";
    in.type = kNodeTypeInput;
    in.params["gainDb"] = inputGainDb;

    GraphNode out;
    out.id = "__output__";
    out.type = kNodeTypeOutput;
    out.params["gainDb"] = outputGainDb;

    GraphNode fx;
    fx.id = "fx";
    fx.type = "gain";
    fx.category = "utility";
    fx.params["gainDb"] = 0.0;

    preset.graph.nodes = {in, out, fx};
    preset.graph.edges = {{"__input__", "fx", 0, 0, 1.0}, {"fx", "__output__", 0, 0, 1.0}};
    return preset;
}

class SineRig
{
  public:
    explicit SineRig(ProcessFn process) : mProcess(std::move(process))
    {
    }

    /// Runs `blocks` blocks of the sine and returns the left output's RMS over the last
    /// `measureBlocks` of them.
    double Run(int blocks, int measureBlocks)
    {
        constexpr double kTwoPi = 6.283185307179586;
        const double phaseStep = kTwoPi * kSineHz / kSampleRate;
        double sum = 0.0;
        int count = 0;

        for (int block = 0; block < blocks; ++block)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const auto sample = static_cast<float>(kSineAmplitude * std::sin(mPhase));
                mInL[static_cast<size_t>(i)] = sample;
                mInR[static_cast<size_t>(i)] = sample;
                mPhase = std::fmod(mPhase + phaseStep, kTwoPi);
            }

            std::fill(mOutL.begin(), mOutL.end(), 0.0f);
            std::fill(mOutR.begin(), mOutR.end(), 0.0f);
            float* inputs[] = {mInL.data(), mInR.data()};
            float* outputs[] = {mOutL.data(), mOutR.data()};

            if (!mProcess(inputs, outputs, kBlock) || block < blocks - measureBlocks)
            {
                continue;
            }

            for (const float sample : mOutL)
            {
                sum += static_cast<double>(sample) * sample;
                ++count;
            }
        }

        return count > 0 ? std::sqrt(sum / count) : 0.0;
    }

  private:
    ProcessFn mProcess;
    double mPhase = 0.0;
    std::vector<float> mInL = std::vector<float>(kBlock);
    std::vector<float> mInR = std::vector<float>(kBlock);
    std::vector<float> mOutL = std::vector<float>(kBlock);
    std::vector<float> mOutR = std::vector<float>(kBlock);
};

double ToDb(double ratio)
{
    return 20.0 * std::log10(std::max(ratio, 1.0e-12));
}

bool Expect(double measuredDb, double expectedDb, const std::string& what)
{
    const bool ok = std::abs(measuredDb - expectedDb) <= kToleranceDb;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << ": expected " << expectedDb << " dB, measured " << measuredDb
              << " dB\n";
    return ok;
}

bool ExpectSignal(double rms, const std::string& layer)
{
    if (rms >= 1.0e-4)
    {
        return true;
    }

    std::cout << "[FAIL] " << layer << ": the sine never reached the output (rms " << rms << ")\n";
    return false;
}

/// Moves each gain in turn through `setGain` and checks the level follows. The effect's
/// own gain goes first: it is the control that proves the path under test is live.
bool RunGainSteps(const std::string& layer, SineRig& rig, double reference,
                  const std::function<void(const std::string&, double)>& setGain)
{
    const auto stepDb = [&] { return ToDb(rig.Run(kStepBlocks, kMeasureBlocks) / reference); };

    bool passed = true;
    setGain("fx", -12.0);
    passed = Expect(stepDb(), -12.0, layer + ": effect gain -12 dB (control)") && passed;
    setGain("fx", 0.0);

    setGain("__output__", -12.0);
    passed = Expect(stepDb(), -12.0, layer + ": output node gain -12 dB") && passed;
    setGain("__output__", 6.0);
    passed = Expect(stepDb(), 6.0, layer + ": output node gain +6 dB") && passed;
    setGain("__output__", 0.0);

    setGain("__input__", -12.0);
    passed = Expect(stepDb(), -12.0, layer + ": input node gain -12 dB") && passed;
    setGain("__input__", 6.0);
    passed = Expect(stepDb(), 6.0, layer + ": input node gain +6 dB") && passed;
    setGain("__input__", 0.0);
    return passed;
}

bool TestExecutor()
{
    SignalGraphExecutor executor;
    executor.SetGraph(BuildPreset(0.0, 0.0).graph);
    executor.Prepare(kSampleRate, kBlock);
    SineRig rig([&](float** in, float** out, int n) {
        executor.Process(in, out, n);
        return true;
    });
    const double reference = rig.Run(kStepBlocks, kMeasureBlocks);

    if (!ExpectSignal(reference, "executor"))
    {
        return false;
    }

    bool passed = RunGainSteps("executor", rig, reference, [&](const std::string& nodeId, double gainDb) {
        executor.SetNodeParam(nodeId, "gainDb", gainDb);
    });

    SignalGraphExecutor saved;
    saved.SetGraph(BuildPreset(-6.0, -6.0).graph);
    saved.Prepare(kSampleRate, kBlock);
    SineRig savedRig([&](float** in, float** out, int n) {
        saved.Process(in, out, n);
        return true;
    });
    passed = Expect(ToDb(savedRig.Run(kStepBlocks, kMeasureBlocks) / reference), -12.0,
                    "executor: boundary gains stored in the graph apply") &&
             passed;
    return passed;
}

bool TestMixer()
{
    MultiPresetMixer mixer;
    mixer.Prepare(kSampleRate, kBlock);
    mixer.AddActivePreset(BuildPreset(0.0, 0.0), kPresetId, "Boundary gain");
    SineRig rig([&](float** in, float** out, int n) {
        mixer.Process(in, out, n);
        return true;
    });
    const double reference = rig.Run(kSettleBlocks, kMeasureBlocks);

    if (!ExpectSignal(reference, "mixer"))
    {
        return false;
    }

    return RunGainSteps("mixer", rig, reference, [&](const std::string& nodeId, double gainDb) {
        mixer.SetNodeParam(kPresetId, nodeId, "gainDb", gainDb);
    });
}

void LoadPreset(PluginController& controller, const Preset& preset)
{
    controller.HandleUIMessage(nlohmann::json{{"type", "loadPreset"},
                                              {"presetId", preset.id},
                                              {"preset", nlohmann::json::parse(PresetStorage::SerializeToJson(preset))}}
                                   .dump());
}

bool TestController()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-boundary-node-gain-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    bool passed = true;

    {
        TestHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);
        LoadPreset(controller, BuildPreset(0.0, 0.0));

        std::cout << "controller: active preset '"
                  << (controller.GetActivePreset() ? controller.GetActivePreset()->id : std::string{"<none>"})
                  << "', mixer slots:";

        for (const auto& id : controller.GetMixer().GetActivePresetIds())
        {
            std::cout << " '" << id << "'";
        }

        std::cout << "\n";

        SineRig rig([&](float** in, float** out, int n) { return controller.ProcessAudio(in, out, n); });
        const double reference = rig.Run(kSettleBlocks, kMeasureBlocks);

        if (!ExpectSignal(reference, "controller"))
        {
            passed = false;
        }
        else
        {
            // The message the node params panel sends when a knob moves.
            passed = RunGainSteps("controller", rig, reference, [&](const std::string& nodeId, double gainDb) {
                controller.HandleUIMessage(nlohmann::json{
                    {"type", "updateSignalPathNodeParam"},
                    {"presetId", kPresetId},
                    {"nodeId", nodeId},
                    {"paramKey", "gainDb"},
                    {"value", gainDb}}.dump());
            });

            LoadPreset(controller, BuildPreset(-6.0, -6.0));
            passed = Expect(ToDb(rig.Run(kSettleBlocks, kMeasureBlocks) / reference), -12.0,
                            "controller: saved boundary gains apply on load") &&
                     passed;
        }
    }

    fs::remove_all(sandbox, ec);
    return passed;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool passed = TestExecutor();
    passed = TestMixer() && passed;
    passed = TestController() && passed;
    return passed ? 0 : 1;
}
