/**
 * @file BlendDefinitionWorkflowTests.cpp
 * @brief A blend node plays the models its blend definition maps, wherever its chain is built.
 *
 * A saved preset keeps only a blend node's blendId. The controller fills in the models from
 * the blend library when it builds the chain. These tests drive the controller the way the
 * UI does and check that:
 *  - with nothing captured, Blend sweeps the models; with settings captured, the mapped
 *    knobs pick the models, a blank row is not given an invented setting, and an unset
 *    knob starts at the median of the captured values, where the UI draws it;
 *  - saving a blend rebuilds every playing slot that uses it, so the edit is heard, and a
 *    snap override set on the node survives that;
 *  - a value for a parameter no model was captured at changes nothing;
 *  - a preset added to the mixer as a second slot plays its blend's models, not dry input;
 *  - a model a blend plays, and a blend a preset plays, cannot be deleted.
 * The models are generated Linear NAMs, each a plain gain, so the output level says which
 * one is playing.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"

namespace fs = std::filesystem;
using namespace guitarfx;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;
constexpr float kQuietGain = 0.2f;
constexpr float kLoudGain = 0.6f;
constexpr const char* kPresetId = "blend-workflow-preset";
constexpr const char* kSlotPresetId = "blend-workflow-slot";

bool Check(bool condition, const std::string& what)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what << "\n";
    return condition;
}

class TestHost final : public IPluginHost
{
  public:
    explicit TestHost(fs::path userDataPath) : mUserDataPath(std::move(userDataPath))
    {
    }

    void SendMessageToUI(const std::string& message) override
    {
        messages.push_back(message);
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

    std::vector<std::string> messages;

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

/// A Linear NAM model that scales its input by `gain`.
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

/// Indexes a model file into the resource library, as the resource browser does, and
/// returns the id the library gave it.
std::string RegisterModel(PluginController& controller, TestHost& host, const fs::path& path, const std::string& name)
{
    host.messages.clear();
    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveLocalLibraryResource"}, {"resourceType", "nam"}, {"name", name}, {"filePath", path.string()}}
                                   .dump());

    for (auto it = host.messages.rbegin(); it != host.messages.rend(); ++it)
    {
        const auto message = nlohmann::json::parse(*it, nullptr, false);

        if (!message.is_discarded() && message.value("type", "") == "resourceImported")
        {
            return message.value("id", "");
        }
    }

    return {};
}

/// One model row as the blend editor saves it (blendEditor.ts save()): the primary
/// parameter's id always, and its value, plus the parameters map, only when captured.
nlohmann::json EditorMapping(const std::string& modelId, std::optional<double> gain)
{
    nlohmann::json mapping = {{"id", modelId}, {"parameterId", "gain"}};

    if (gain)
    {
        mapping["parameterValue"] = *gain;
        mapping["parameters"] = {{"gain", *gain}};
    }

    return mapping;
}

void SaveBlend(PluginController& controller, const std::string& blendId, const nlohmann::json& mappings)
{
    nlohmann::json models = nlohmann::json::array();

    for (const auto& mapping : mappings)
    {
        models.push_back(mapping.value("id", ""));
    }

    controller.HandleUIMessage(nlohmann::json{{"type", "saveBlendDefinition"},
                                              {"blend",
                                               {{"id", blendId},
                                                {"name", blendId},
                                                {"category", "amp"},
                                                {"parameters", {"gain"}},
                                                {"models", models},
                                                {"modelMappings", mappings},
                                                {"blendMode", "interpolate"}}}}
                                   .dump());
}

GraphNode BlendNode(const std::string& nodeId, const std::string& blendId, double blend, bool enabled = true)
{
    GraphNode node;
    node.id = nodeId;
    node.type = EffectGuids::kAmpNamBlend;
    node.category = "amp";
    node.enabled = enabled;
    node.config["blendId"] = blendId;
    node.params["blend"] = blend;
    return node;
}

/// input -> each node in turn -> output.
Preset BuildPreset(const std::string& presetId, const std::vector<GraphNode>& chain)
{
    Preset preset;
    preset.id = presetId;
    preset.name = presetId;
    preset.version = 2;
    preset.category = "Test";

    GraphNode in;
    in.id = "__input__";
    in.type = kNodeTypeInput;

    GraphNode out;
    out.id = "__output__";
    out.type = kNodeTypeOutput;

    preset.graph.nodes = {in, out};
    std::string previous = in.id;

    for (const auto& node : chain)
    {
        preset.graph.nodes.push_back(node);
        preset.graph.edges.push_back({previous, node.id, 0, 0, 1.0});
        previous = node.id;
    }

    preset.graph.edges.push_back({previous, out.id, 0, 0, 1.0});
    return preset;
}

nlohmann::json PresetJson(const Preset& preset)
{
    return nlohmann::json::parse(PresetStorage::SerializeToJson(preset));
}

/// Feeds a steady 220 Hz sine through the controller, as the host callback would.
class SignalDriver
{
  public:
    explicit SignalDriver(PluginController& controller) : mController(controller)
    {
    }

    /// Runs until every slot a rebuild retired has finished fading, then a little more so
    /// the replacement is fully faded in.
    void Settle()
    {
        constexpr int kMaxBlocks = static_cast<int>(20.0 * kSampleRate / kBlock);

        for (int i = 0; i < kMaxBlocks && mController.GetMixer().GetRetiringPresetCount() > 0; ++i)
        {
            Run(1);
        }

        Run(64);
    }

    /// Output level over input level, measured over `blocks` blocks.
    double MeasureGain(int blocks = 64)
    {
        mInEnergy = 0.0;
        mOutEnergy = 0.0;
        Run(blocks);
        return mInEnergy > 0.0 ? std::sqrt(mOutEnergy / mInEnergy) : 0.0;
    }

    void Run(int blocks)
    {
        std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
        constexpr double kTwoPi = 6.283185307179586;
        const double phaseStep = kTwoPi * 220.0 / kSampleRate;

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                const auto sample = static_cast<float>(0.1 * std::sin(mPhase));
                inL[static_cast<size_t>(i)] = inR[static_cast<size_t>(i)] = sample;
                mPhase = std::fmod(mPhase + phaseStep, kTwoPi);
            }

            float* inputs[] = {inL.data(), inR.data()};
            float* outputs[] = {outL.data(), outR.data()};

            if (!mController.ProcessAudio(inputs, outputs, kBlock))
            {
                continue;
            }

            for (int i = 0; i < kBlock; ++i)
            {
                mInEnergy += static_cast<double>(inL[static_cast<size_t>(i)]) * inL[static_cast<size_t>(i)];
                mOutEnergy += static_cast<double>(outL[static_cast<size_t>(i)]) * outL[static_cast<size_t>(i)];
            }
        }
    }

  private:
    PluginController& mController;
    double mPhase = 0.0;
    double mInEnergy = 0.0;
    double mOutEnergy = 0.0;
};

const ResourceRef* FindRef(const GraphNode& node, const std::string& resourceId)
{
    const auto it = std::find_if(node.resources.begin(), node.resources.end(),
                                 [&](const ResourceRef& ref) { return ref.resourceId == resourceId; });
    return it == node.resources.end() ? nullptr : &*it;
}

bool Near(double actual, double expected, double tolerance)
{
    return std::abs(actual - expected) <= tolerance;
}

std::string Describe(double value)
{
    return std::to_string(value).substr(0, 5);
}

void LoadPreset(PluginController& controller, const Preset& preset)
{
    controller.HandleUIMessage(
        nlohmann::json{{"type", "loadPreset"}, {"presetId", preset.id}, {"preset", PresetJson(preset)}}.dump());
}

/// A knob turned in the params panel (sendSignalPathNodeParamUpdate).
void SetNodeParam(PluginController& controller, const std::string& presetId, const std::string& nodeId,
                  const std::string& key, double value)
{
    controller.HandleUIMessage(nlohmann::json{
        {"type", "updateSignalPathNodeParam"},
        {"presetId", presetId},
        {"nodeId", nodeId},
        {"paramKey", key},
        {"value", value}}.dump());
}

/// A node setting changed in the params panel (sendSignalPathNodeConfigUpdate).
void SetNodeConfig(PluginController& controller, const std::string& presetId, const std::string& nodeId,
                   const std::string& key, const std::string& value)
{
    controller.HandleUIMessage(nlohmann::json{
        {"type", "updateSignalPathNodeConfig"},
        {"presetId", presetId},
        {"nodeId", nodeId},
        {"key", key},
        {"value", value},
        {"persist", true}}.dump());
}

/// The `detail` of the last message of `type`, or empty.
std::string LastDetail(const TestHost& host, const std::string& type)
{
    for (auto it = host.messages.rbegin(); it != host.messages.rend(); ++it)
    {
        const auto message = nlohmann::json::parse(*it, nullptr, false);

        if (!message.is_discarded() && message.value("type", "") == type)
        {
            return message.value("detail", "");
        }
    }

    return {};
}

bool SawMessage(const TestHost& host, const std::string& type)
{
    return std::any_of(host.messages.begin(), host.messages.end(), [&](const std::string& text) {
        const auto message = nlohmann::json::parse(text, nullptr, false);
        return !message.is_discarded() && message.value("type", "") == type;
    });
}

double Ratio(double over, double under)
{
    return under > 0.0 ? over / under : 0.0;
}

bool Run()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-blend-definition-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    const fs::path quietPath = WriteLinearModel(sandbox, "linear-quiet", kQuietGain);
    const fs::path loudPath = WriteLinearModel(sandbox, "linear-loud", kLoudGain);
    const fs::path sparePath = WriteLinearModel(sandbox, "linear-spare", 0.4f);
    const double loudOverQuiet = static_cast<double>(kLoudGain) / kQuietGain;
    bool passed = true;

    {
        TestHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, kBlock);

        const std::string quiet = RegisterModel(controller, host, quietPath, "Linear quiet");
        const std::string loud = RegisterModel(controller, host, loudPath, "Linear loud");
        const std::string spare = RegisterModel(controller, host, sparePath, "Linear spare");

        if (!Check(!quiet.empty() && !loud.empty() && !spare.empty(), "the models are in the resource library"))
        {
            return false;
        }

        SignalDriver driver(controller);

        // ── With nothing captured, Blend sweeps the models in list order ────────
        SaveBlend(controller, "listed", {EditorMapping(quiet, std::nullopt), EditorMapping(loud, std::nullopt)});
        LoadPreset(controller, BuildPreset(kPresetId, {BlendNode("blend", "listed", 0.0)}));
        driver.Settle();
        const double sweepStart = driver.MeasureGain();
        SetNodeParam(controller, kPresetId, "blend", "blend", 1.0);
        driver.Settle();
        const double sweepEnd = driver.MeasureGain();

        passed = Check(Near(Ratio(sweepEnd, sweepStart), loudOverQuiet, 0.3),
                       "sweep: with nothing captured, Blend runs from the first model to the last (x" +
                           Describe(Ratio(sweepEnd, sweepStart)) + ", want x" + Describe(loudOverQuiet) + ")") &&
                 passed;

        // ── Captured settings reach the effect, and the knob starts where it is drawn ──
        // "sweep" runs quiet (gain 0) to loud (gain 1). "partial" has a row the user left
        // blank, which the editor still saves with the primary parameter's id.
        SaveBlend(controller, "sweep", {EditorMapping(quiet, 0.0), EditorMapping(loud, 1.0)});
        SaveBlend(controller, "partial", {EditorMapping(quiet, 0.0), EditorMapping(loud, std::nullopt)});
        LoadPreset(controller, BuildPreset(kPresetId, {BlendNode("blend", "sweep", 0.0),
                                                       BlendNode("partial", "partial", 0.0, false)}));

        {
            const auto& active = controller.GetActivePreset();
            const auto* sweepNode = active ? active->graph.FindNode("blend") : nullptr;
            const auto* partialNode = active ? active->graph.FindNode("partial") : nullptr;
            const auto* quietRef = sweepNode ? FindRef(*sweepNode, quiet) : nullptr;
            const auto* loudRef = sweepNode ? FindRef(*sweepNode, loud) : nullptr;

            passed = Check(quietRef && loudRef && quietRef->parameterValue && loudRef->parameterValue &&
                               Near(*quietRef->parameterValue, 0.0, 1e-9) && Near(*loudRef->parameterValue, 1.0, 1e-9),
                           "mapping: each model keeps the setting the editor saved") &&
                     passed;

            const auto* blankRef = partialNode ? FindRef(*partialNode, loud) : nullptr;
            passed = Check(blankRef && blankRef->parameters.empty() && blankRef->parameterId.empty() &&
                               blankRef->parameterValue && Near(*blankRef->parameterValue, 1.0, 1e-9),
                           "mapping: a row with no captured value sits at its list position, with no invented "
                           "setting") &&
                     passed;

            const bool seeded =
                sweepNode && sweepNode->params.contains("gain") && Near(sweepNode->params.at("gain"), 0.5, 1e-9);
            passed = Check(seeded, "seed: an unset Gain starts at the median of the captured values, where the "
                                   "knob is drawn") &&
                     passed;
        }

        driver.Settle();
        const double atMedian = driver.MeasureGain();
        SetNodeParam(controller, kPresetId, "blend", "gain", 0.0);
        driver.Settle();
        const double atQuiet = driver.MeasureGain();
        SetNodeParam(controller, kPresetId, "blend", "gain", 1.0);
        driver.Settle();
        const double atLoud = driver.MeasureGain();

        const double halfway = (kQuietGain + kLoudGain) / 2.0 / kQuietGain;
        passed = Check(Near(Ratio(atMedian, atQuiet), halfway, 0.15),
                       "gain: the seeded knob plays halfway between the captures (x" +
                           Describe(Ratio(atMedian, atQuiet)) + ", want x" + Describe(halfway) + ")") &&
                 passed;
        passed = Check(Near(Ratio(atLoud, atQuiet), loudOverQuiet, 0.3),
                       "gain: turning Gain from 0 to 1 moves from the quiet model to the loud one (x" +
                           Describe(Ratio(atLoud, atQuiet)) + ", want x" + Describe(loudOverQuiet) + ")") &&
                 passed;

        // ── Saving the blend is heard in the one playing slot ───────────────────
        SaveBlend(controller, "sweep", {EditorMapping(quiet, 1.0), EditorMapping(loud, 0.0)});
        driver.Settle();
        const double afterSwap = driver.MeasureGain();

        passed = Check(Near(Ratio(afterSwap, atLoud), 1.0 / loudOverQuiet, 0.05),
                       "save: a reversed mapping is heard at once in the playing preset (x" +
                           Describe(Ratio(afterSwap, atLoud)) + ", want x" + Describe(1.0 / loudOverQuiet) + ")") &&
                 passed;

        // ── The node's own blend mode survives the blend being applied again ────
        // At gain 0.4 the loud model, now captured at 0, is the nearer.
        SetNodeParam(controller, kPresetId, "blend", "gain", 0.4);
        driver.Settle();
        const double interpolated = driver.MeasureGain();
        SetNodeConfig(controller, kPresetId, "blend", "blendModeOverride", "snap");
        driver.Settle();
        const double snapped = driver.MeasureGain();
        SaveBlend(controller, "sweep", {EditorMapping(quiet, 1.0), EditorMapping(loud, 0.0)});
        driver.Settle();
        const double rebuilt = driver.MeasureGain();

        passed = Check(Near(Ratio(snapped, atLoud), 1.0, 0.03) && Ratio(interpolated, atLoud) < 0.9,
                       "override: snap on the node plays only the nearest model (x" + Describe(Ratio(snapped, atLoud)) +
                           ", interpolated x" + Describe(Ratio(interpolated, atLoud)) + ")") &&
                 passed;
        passed = Check(Near(Ratio(rebuilt, snapped), 1.0, 0.03),
                       "override: it survives the blend being saved and applied again (x" +
                           Describe(Ratio(rebuilt, snapped)) + ")") &&
                 passed;

        // ── A parameter dropped from the blend no longer steers it ──────────────
        SetNodeConfig(controller, kPresetId, "blend", "blendModeOverride", "");
        driver.Settle();
        const double beforeStale = driver.MeasureGain();
        SetNodeParam(controller, kPresetId, "blend", "bass", 0.9);
        driver.Settle();
        const double afterStale = driver.MeasureGain();

        passed = Check(Near(Ratio(afterStale, beforeStale), 1.0, 0.01),
                       "stale: a value for a parameter no model was captured at changes nothing (x" +
                           Describe(Ratio(afterStale, beforeStale)) + ")") &&
                 passed;

        // ── A second mixer slot plays its blend ─────────────────────────────────
        SetNodeParam(controller, kPresetId, "blend", "gain", 1.0);
        Preset slotPreset = BuildPreset(kSlotPresetId, {BlendNode("blend", "sweep", 0.0)});
        slotPreset.graph.FindNode("blend")->params["gain"] = 1.0;
        controller.HandleUIMessage(nlohmann::json{
            {"type", "addActivePreset"},
            {"presetId", kSlotPresetId},
            {"name", "Second slot"},
            {"preset",
             PresetJson(slotPreset)}}.dump());

        const auto* slotBlend = controller.GetMixer().GetNodeProcessor(kSlotPresetId, "blend");
        passed = Check(slotBlend && slotBlend->HasResource(),
                       "mixer: a preset added as a second slot loads its blend's models") &&
                 passed;

        // ── Saving the blend is heard in every slot that plays it ───────────────
        driver.Settle();
        const double bothQuiet = driver.MeasureGain();
        SaveBlend(controller, "sweep", {EditorMapping(quiet, 0.0), EditorMapping(loud, 1.0)});
        driver.Settle();
        const double bothLoud = driver.MeasureGain();

        passed = Check(Near(Ratio(bothLoud, bothQuiet), loudOverQuiet, 0.3),
                       "save: both mixer slots pick up the edit (x" + Describe(Ratio(bothLoud, bothQuiet)) +
                           ", want x" + Describe(loudOverQuiet) + ")") &&
                 passed;

        const auto activeIds = controller.GetMixer().GetActivePresetIds();
        passed = Check(activeIds.size() == 2,
                       "save: the mixer still has both slots (" + std::to_string(activeIds.size()) + ")") &&
                 passed;

        // ── Deleting what a blend still needs is refused ─────────────────────────
        // A one-element brace list would make nlohmann build the element itself, not an array.
        SaveBlend(controller, "spare", nlohmann::json::array({EditorMapping(spare, 0.0)}));
        const auto deleteModel = [&]() {
            controller.HandleUIMessage(nlohmann::json{
                {"type", "deleteLibraryResource"},
                {"resourceType", "nam"},
                {"resourceId", spare}}.dump());
        };

        host.messages.clear();
        deleteModel();
        const std::string resourceRefusal = LastDetail(host, "resourceDeleteFailed");
        passed = Check(resourceRefusal == "Used by blend: spare",
                       "delete: a model only a blend plays is kept (\"" + resourceRefusal + "\")") &&
                 passed;

        host.messages.clear();
        controller.HandleUIMessage(nlohmann::json{{"type", "deleteBlendDefinition"}, {"blendId", "sweep"}}.dump());
        const std::string blendRefusal = LastDetail(host, "error");
        passed = Check(blendRefusal.rfind("Used by preset:", 0) == 0,
                       "delete: a blend a preset plays is kept (\"" + blendRefusal + "\")") &&
                 passed;

        host.messages.clear();
        controller.HandleUIMessage(nlohmann::json{{"type", "deleteBlendDefinition"}, {"blendId", "spare"}}.dump());
        deleteModel();
        passed = Check(!SawMessage(host, "error") && SawMessage(host, "resourceRemoved"),
                       "delete: an unused blend goes, and then so can its model") &&
                 passed;
    }

    fs::remove_all(sandbox, ec);
    return passed;
}
} // namespace

int main()
{
    std::cout << std::unitbuf;
    RegisterAllEffects();

    const bool passed = Run();
    std::cout << (passed ? "BlendDefinitionWorkflowTests PASSED\n" : "BlendDefinitionWorkflowTests FAILED\n");
    return passed ? 0 : 1;
}
