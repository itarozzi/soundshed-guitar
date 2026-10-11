/**
 * @file PresetLevelMeasureTests.cpp
 * @brief measurePresetLevel: a preset rendered offline over a demo clip and measured, on a graph
 * of its own, while the live mixer plays on untouched.
 *
 * This test's main thread is the message thread; the host posts RunOnMainThread() to a queue it
 * pumps, as JUCE does, so answers arrive the way they do in the app.
 *  - A preset that passes its input straight through measures as the clip itself does, and its
 *    output gain moves the reading by exactly that gain: the plumbing adds and loses nothing.
 *  - A drive, delay chain and a NAM amp into an IR cab give finite, plausible levels, the delay a
 *    tail, and the same reading a mixer slot built from the preset plays.
 *  - Measured by id (an unsaved active preset included) it reads as sent whole; a scene reads
 *    that scene; maxSeconds measures that much of the clip.
 *  - With audio running, a measurement leaves the active preset, its slot and its processors as
 *    they were, and the audio thread keeps getting blocks through.
 *  - An unknown preset, scene or clip, and a resource with no file, fail with a reason and the
 *    request's id. A controller torn down mid-measurement stops at once.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "controller/internal/OfflineRenderSupport.h"
#include "dsp/Loudness.h"
#include "dsp/MultiPresetMixer.h"
#include "presets/PresetStorage.h"
#include "util/FileIO.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using json = nlohmann::json;

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr const char* kRiffFile = "guitar-riff-01.wav";
constexpr const char* kDiFile = "DI_Guitar_L.mp3";

int gFailures = 0;

bool Check(bool condition, const std::string& what, const std::string& detail = {})
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << "\n";

    if (!condition)
    {
        ++gFailures;
    }

    return condition;
}

std::string Num(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", value);
    return text;
}

/// Records what the controller sends, from the message thread the test pumps.
class RecordingHost final : public test::PumpedTestHost
{
  public:
    explicit RecordingHost(const fs::path& root) : PumpedTestHost(root, std::this_thread::get_id(), kSampleRate, 128)
    {
    }

    void SendMessageToUI(const std::string& message) override
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mMessages.push_back(message);
    }

    /// Pumps until a measurement answer carrying `requestId` arrives; null on timeout.
    json WaitForAnswer(const std::string& requestId, std::chrono::seconds timeout = std::chrono::seconds(120))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline)
        {
            Pump();
            {
                const std::lock_guard<std::mutex> lock(mMutex);

                for (const auto& text : mMessages)
                {
                    const auto message = json::parse(text, nullptr, false);
                    const auto type = message.is_object() ? message.value("type", std::string{}) : std::string{};

                    if ((type == "presetLevelMeasured" || type == "presetLevelMeasureFailed") &&
                        message.value("requestId", std::string{}) == requestId)
                    {
                        return message;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return nullptr;
    }

  private:
    std::mutex mMutex;
    std::vector<std::string> mMessages;
};

fs::path FreshSandbox()
{
    const fs::path sandbox =
        fs::temp_directory_path() / "guitarfx-preset-level-tests" / std::to_string(std::random_device{}());
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox / "ui" / "demo", ec);
    test::SetSettingsEnvRoot(sandbox);

    // The bundled demo folder, with the clips the tests play: a short riff, the long DI (the
    // default), and a riff listed under an id whose file is not there.
    const fs::path demo = fs::path(GUITARFX_DEMO_AUDIO_DIR);
    fs::copy_file(demo / kRiffFile, sandbox / "ui" / "demo" / kRiffFile, ec);
    fs::copy_file(demo / kDiFile, sandbox / "ui" / "demo" / kDiFile, ec);
    const json clips = {{"clips",
                         {{{"id", "riff"}, {"title", "Riff"}, {"file", kRiffFile}},
                          {{"id", "di-riff-03"}, {"title", "DI"}, {"file", kDiFile}},
                          {{"id", "gone"}, {"title", "Gone"}, {"file", "not-there.wav"}}}}};
    std::ofstream(sandbox / "ui" / "demo" / "clips.json") << clips.dump();
    return sandbox;
}

GraphNode Node(const std::string& id, const std::string& type, std::map<std::string, double> params = {})
{
    GraphNode node;
    node.id = id;
    node.type = type;
    node.enabled = true;
    node.params = std::move(params);
    return node;
}

/// input -> nodes... -> output, with the boundary gains given.
SignalGraph Chain(const std::vector<GraphNode>& nodes, double outputGainDb = 0.0)
{
    SignalGraph graph;
    graph.nodes.push_back(Node("__input__", kNodeTypeInput, {{"gainDb", 0.0}}));

    for (const auto& node : nodes)
    {
        graph.nodes.push_back(node);
    }

    graph.nodes.push_back(Node("__output__", kNodeTypeOutput, {{"gainDb", outputGainDb}}));

    for (std::size_t i = 0; i + 1 < graph.nodes.size(); ++i)
    {
        graph.edges.push_back({graph.nodes[i].id, graph.nodes[i + 1].id, 0, 0, 1.0});
    }

    return graph;
}

Preset MakePreset(const std::string& id, const SignalGraph& graph)
{
    Preset preset;
    preset.id = id;
    preset.name = id;
    preset.graph = graph;
    return preset;
}

json PresetJson(const Preset& preset)
{
    return json::parse(PresetStorage::SerializeToJson(preset));
}

json Measure(PluginController& controller, RecordingHost& host, json request)
{
    static int counter = 0;
    const std::string requestId = "level-" + std::to_string(++counter);
    request["type"] = "measurePresetLevel";
    request["requestId"] = requestId;
    controller.HandleUIMessage(request.dump());
    return host.WaitForAnswer(requestId);
}

bool Measured(const json& answer)
{
    return answer.is_object() && answer.value("type", std::string{}) == "presetLevelMeasured" &&
           answer["integratedLufs"].is_number();
}

double Lufs(const json& answer)
{
    return Measured(answer) ? answer["integratedLufs"].get<double>() : -999.0;
}

std::string Describe(const json& answer)
{
    if (!answer.is_object())
    {
        return "no answer";
    }

    if (answer.value("type", std::string{}) == "presetLevelMeasureFailed")
    {
        return "failed: " + answer.value("message", std::string{});
    }

    return Num(answer.value("integratedLufs", -999.0)) + " LUFS, peak " + Num(answer.value("peakDbfs", -999.0)) +
           ", rms " + Num(answer.value("rmsDbfs", -999.0)) + ", " + Num(answer.value("durationSeconds", 0.0)) +
           " s, tail " + Num(answer.value("tailSeconds", 0.0)) + " s";
}

/// The riff as the measurement decodes it: first channel, at kSampleRate.
std::vector<float> DecodedRiff()
{
    std::string error;
    auto prepared = controller_detail::PrepareOfflineRenderBuffer(
        util::ReadFileBytes(fs::path(GUITARFX_DEMO_AUDIO_DIR) / kRiffFile), kSampleRate, "riff", "riff", error);
    return prepared ? prepared->channelSamples.front() : std::vector<float>{};
}

/// What a fresh mixer with `preset` alone in it puts out for the riff, as the measurement would
/// see it: integrated loudness over the riff's length. The preset goes through JSON first, as
/// every preset the app loads does (that is where stored node params are brought up to date).
std::optional<double> MixerSlotLoudness(const Preset& preset)
{
    const auto loaded = PresetStorage::DeserializeFromJson(PresetStorage::SerializeToJson(preset));
    const auto riff = DecodedRiff();

    if (!loaded || riff.empty())
    {
        return std::nullopt;
    }

    MultiPresetMixer mixer;
    mixer.SetAudioChannelCounts(1, 2);
    mixer.Prepare(kSampleRate, 512);
    mixer.AddActivePreset(*loaded, loaded->id, loaded->name);
    std::vector<float> left(riff.size()), right(riff.size()), inL(512), inR(512), outL(512), outR(512);

    for (std::size_t at = 0; at < riff.size(); at += 512)
    {
        const std::size_t count = std::min<std::size_t>(512, riff.size() - at);
        std::fill(inL.begin(), inL.end(), 0.0f);
        std::copy_n(riff.data() + at, count, inL.data());
        std::copy(inL.begin(), inL.end(), inR.begin());
        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {outL.data(), outR.data()};
        mixer.SetTempo(120.0);
        mixer.Process(ins, outs, 512);
        std::copy_n(outL.data(), count, left.data() + at);
        std::copy_n(outR.data(), count, right.data() + at);
    }

    return loudness::MeasureIntegratedLoudness(left, right, kSampleRate).integratedLufs;
}

void TestPassThroughAndGain(PluginController& controller, RecordingHost& host)
{
    std::cout << "\nThe plumbing\n";
    const auto riff = DecodedRiff();
    const auto clip = loudness::MeasureIntegratedLoudness(riff.data(), nullptr, riff.size(), kSampleRate);
    // A centred rig is 3 dB down on each side in the mix (the equal-power pan law).
    const double centre = 20.0 * std::log10(std::cos(3.14159265358979323846 / 4.0));

    const auto straight =
        Measure(controller, host, {{"preset", PresetJson(MakePreset("thru", Chain({})))}, {"clipId", "riff"}});
    Check(Measured(straight) && std::abs(straight.value("slotGainDb", 0.0) - centre) < 1.0e-3,
          "the answer gives the mix's gain on a centred rig", Num(straight.value("slotGainDb", 0.0)));
    Check(Measured(straight) && clip.integratedLufs &&
              std::abs(Lufs(straight) - (*clip.integratedLufs + centre)) < 0.01,
          "a preset passing its input through reads as the clip does, less that gain",
          Describe(straight) + " vs clip " + Num(clip.integratedLufs.value_or(-999.0)));
    Check(Measured(straight) &&
              std::abs(straight["peakDbfs"].get<double>() - (clip.peakDbfs.value_or(99.0) + centre)) < 0.01 &&
              std::abs(straight["durationSeconds"].get<double>() - clip.DurationSeconds()) < 1.0e-6,
          "with the clip's peak, less that gain, and its length");
    Check(straight.value("requestId", std::string{}).rfind("level-", 0) == 0 &&
              straight.value("presetId", "") == "thru" && straight.value("sceneId", "") == "scene-1" &&
              straight.value("clipId", "") == "riff" && straight.value("sampleRate", 0.0) == kSampleRate,
          "the answer names the request, preset, scene, clip and rate");

    const auto quieter =
        Measure(controller, host, {{"preset", PresetJson(MakePreset("thru", Chain({}, -6.0)))}, {"clipId", "riff"}});
    Check(Measured(quieter) && std::abs(Lufs(quieter) - (Lufs(straight) - 6.0)) < 0.05,
          "-6 dB on its output reads 6 LU quieter", Describe(quieter));
}

void TestRealChains(PluginController& controller, RecordingHost& host)
{
    std::cout << "\nReal chains\n";
    const auto driveDelay =
        MakePreset("drive-delay", Chain({Node("drive", "overdrive"), Node("delay", "delay_digital")}));
    const auto answer = Measure(controller, host, {{"preset", PresetJson(driveDelay)}, {"clipId", "riff"}});
    Check(Measured(answer) && Lufs(answer) > -60.0 && Lufs(answer) < 0.0 && answer["peakDbfs"].get<double>() < 12.0,
          "a drive and delay give a plausible level", Describe(answer));
    Check(Measured(answer) && answer.value("tailSeconds", 0.0) > 0.2, "and the delay's tail is rendered",
          Describe(answer));

    // The same preset as a mixer slot plays it, read through JSON as a load reads it.
    const auto slot = MixerSlotLoudness(driveDelay);
    Check(slot && std::abs(*slot - Lufs(answer)) < 0.05, "it reads what a lone mixer slot built from the preset plays",
          Num(slot.value_or(-999.0)) + " vs " + Num(Lufs(answer)));

    const fs::path assets = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets";
    auto amp = Node("amp", "amp_nam");
    amp.resources.push_back({"nam", "", assets / "amps" / "Guitar" / "A2" / "Boosted-6505+-A2-Chugs-FullRig.nam"});
    auto cab = Node("cab", "cab_ir");
    cab.resources.push_back({"ir", "", assets / "ir" / "421 1960.wav"});
    const auto rig =
        Measure(controller, host, {{"preset", PresetJson(MakePreset("rig", Chain({amp, cab})))}, {"clipId", "riff"}});
    Check(Measured(rig) && Lufs(rig) > -50.0 && Lufs(rig) < 6.0, "a NAM amp into an IR cab gives a plausible level",
          Describe(rig));
}

void TestScenesIdsAndLength(PluginController& controller, RecordingHost& host)
{
    std::cout << "\nScenes, ids and length\n";
    auto twoScenes = MakePreset("scenes", Chain({Node("drive", "overdrive")}));
    twoScenes.scenes.push_back({"loud", "Loud", Chain({Node("drive", "overdrive")})});
    twoScenes.scenes.push_back({"soft", "Soft", Chain({Node("drive", "overdrive")}, -10.0)});

    const auto loud = Measure(controller, host, {{"preset", PresetJson(twoScenes)}, {"clipId", "riff"}});
    const auto soft =
        Measure(controller, host, {{"preset", PresetJson(twoScenes)}, {"sceneId", "soft"}, {"clipId", "riff"}});
    Check(Measured(loud) && loud.value("sceneId", "") == "loud", "without a scene, the first one is measured",
          Describe(loud));
    Check(Measured(soft) && soft.value("sceneId", "") == "soft" && std::abs(Lufs(soft) - (Lufs(loud) - 10.0)) < 0.05,
          "a scene asked for is the one measured", Describe(soft));

    // The default clip is the long DI; maxSeconds measures the start of it.
    const auto start = Measure(controller, host, {{"preset", PresetJson(twoScenes)}, {"maxSeconds", 4.0}});
    Check(Measured(start) && start.value("clipId", "") == "di-riff-03" &&
              std::abs(start.value("durationSeconds", 0.0) - 4.0) < 0.01,
          "the default clip is the DI, cut to maxSeconds", Describe(start));
}

void TestFailures(PluginController& controller, RecordingHost& host)
{
    std::cout << "\nFailures\n";
    const auto failed = [](const json& answer, const std::string& needle) {
        return answer.is_object() && answer.value("type", std::string{}) == "presetLevelMeasureFailed" &&
               answer.value("message", std::string{}).find(needle) != std::string::npos;
    };
    const auto simple = PresetJson(MakePreset("simple", Chain({Node("drive", "overdrive")})));

    const auto unknown = Measure(controller, host, {{"presetId", "no-such-preset"}});
    Check(failed(unknown, "not found") && unknown.value("presetId", "") == "no-such-preset",
          "an unknown presetId fails, naming it", Describe(unknown));
    Check(failed(Measure(controller, host, json::object()), "presetId"), "a request naming no preset fails");
    Check(failed(Measure(controller, host, {{"preset", simple}, {"sceneId", "nope"}}), "Scene 'nope'"),
          "an unknown scene fails");
    Check(failed(Measure(controller, host, {{"preset", simple}, {"clipId", "nope"}}), "nope"), "an unknown clip fails");
    Check(failed(Measure(controller, host, {{"preset", simple}, {"clipId", "gone"}}), "gone"),
          "a clip whose file is missing fails");

    auto amp = Node("amp", "amp_nam");
    amp.resources.push_back({"nam", "", fs::path(GUITARFX_TEST_RESOURCES_DIR) / "no-such-model.nam"});
    const auto missing =
        Measure(controller, host, {{"preset", PresetJson(MakePreset("missing", Chain({amp})))}, {"clipId", "riff"}});
    Check(failed(missing, "missing") && missing["missingResources"].is_array() &&
              missing["missingResources"].size() == 1 && missing["missingResources"][0].value("nodeId", "") == "amp",
          "a model with no file fails, listing the node", Describe(missing));

    // A disabled node's missing model is not heard, so it does not stop a measurement.
    amp.enabled = false;
    const auto bypassed =
        Measure(controller, host, {{"preset", PresetJson(MakePreset("bypassed", Chain({amp})))}, {"clipId", "riff"}});
    Check(Measured(bypassed), "a bypassed node's missing model does not", Describe(bypassed));
}

void TestLiveMixerUntouched(PluginController& controller, RecordingHost& host)
{
    std::cout << "\nThe live mixer\n";
    const auto live = MakePreset("live", Chain({Node("drive", "overdrive")}));
    controller.HandleUIMessage(json{{"type", "loadPreset"}, {"presetId", "live"}, {"preset", PresetJson(live)}}.dump());
    host.Pump();

    const auto activeBefore = controller.GetActivePreset() ? controller.GetActivePreset()->id : std::string{};
    const auto slotsBefore = controller.GetMixer().GetActivePresetIds();
    const auto* processorBefore = controller.GetMixer().GetNodeProcessor("live", "drive");

    test::AudioThread audio(controller, 128);
    Check(audio.WaitForBlocks(20), "audio is running before the measurement");

    // Another preset, a heavier one, measured while the live one plays.
    const auto other = MakePreset("other", Chain({Node("drive", "overdrive"), Node("delay", "delay_digital")}));
    const auto started = std::chrono::steady_clock::now();
    controller.HandleUIMessage(json{{"type", "measurePresetLevel"},
                                    {"requestId", "during-audio"},
                                    {"preset", PresetJson(other)},
                                    {"clipId", "riff"}}
                                   .dump());
    const bool audioKeptRunning = audio.WaitForBlocks(50);
    const auto answer = host.WaitForAnswer("during-audio");
    const auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    Check(Measured(answer), "the measurement answers while audio runs", Describe(answer) + ", " + Num(took) + " s");
    Check(audioKeptRunning, "the audio thread kept getting blocks through");

    const auto activeAfter = controller.GetActivePreset() ? controller.GetActivePreset()->id : std::string{};
    Check(activeBefore == "live" && activeAfter == activeBefore, "the active preset is unchanged", activeAfter);
    Check(controller.GetMixer().GetActivePresetIds() == slotsBefore, "the mixer's slots are unchanged");
    Check(processorBefore != nullptr && controller.GetMixer().GetNodeProcessor("live", "drive") == processorBefore,
          "the live slot's processors were not rebuilt");

    // By id: the active preset is unsaved, and still measures as it does sent whole.
    const auto byId = Measure(controller, host, {{"presetId", "live"}, {"clipId", "riff"}});
    const auto whole = Measure(controller, host, {{"preset", PresetJson(live)}, {"clipId", "riff"}});
    Check(Measured(byId) && Measured(whole) && std::abs(Lufs(byId) - Lufs(whole)) < 1.0e-6,
          "the unsaved active preset measured by id reads as it does sent whole", Describe(byId));
}

void TestShutdownMidMeasurement()
{
    std::cout << "\nTear-down\n";
    const fs::path sandbox = FreshSandbox();
    std::chrono::duration<double> took{};
    {
        RecordingHost host(sandbox);
        auto controller = std::make_unique<PluginController>(host);
        controller->Initialize();
        controller->Prepare(kSampleRate, 128);

        // The whole 110 s DI through a drive: long enough to still be running at tear-down.
        const auto preset = MakePreset("long", Chain({Node("drive", "overdrive"), Node("delay", "delay_digital")}));
        controller->HandleUIMessage(
            json{{"type", "measurePresetLevel"}, {"requestId", "long"}, {"preset", PresetJson(preset)}}.dump());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto start = std::chrono::steady_clock::now();
        controller.reset();
        took = std::chrono::steady_clock::now() - start;
        // An answer posted before the tear-down that runs after it must not reach the dead controller.
        host.Pump();
    }
    // Decoding the clip is not interruptible, so allow for that; a full render would take far longer.
    Check(took.count() < 10.0, "a controller torn down mid-measurement stops promptly", Num(took.count()) + " s");

    std::error_code ec;
    fs::remove_all(sandbox, ec);
}
} // namespace

int main()
{
    const fs::path sandbox = FreshSandbox();
    {
        RecordingHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        controller.Prepare(kSampleRate, 128);

        TestPassThroughAndGain(controller, host);
        TestRealChains(controller, host);
        TestScenesIdsAndLength(controller, host);
        TestFailures(controller, host);
        TestLiveMixerUntouched(controller, host);
    }
    TestShutdownMidMeasurement();

    std::error_code ec;
    fs::remove_all(sandbox, ec);

    std::cout << "\nPreset level measurement tests: "
              << (gFailures == 0 ? "all passed" : std::to_string(gFailures) + " failed") << "\n";
    return gFailures == 0 ? 0 : 1;
}
