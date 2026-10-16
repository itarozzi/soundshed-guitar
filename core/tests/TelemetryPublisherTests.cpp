/**
 * @file TelemetryPublisherTests.cpp
 * @brief Covers the signal-diagnostics wire format, and specifically what belongs
 *        in the roster versus in a per-frame payload.
 *
 * The roster exists so that everything static about a node is sent once instead of
 * twenty times a second, and it is re-sent whenever it differs from the last one.
 * That makes it a trap: put a value in it that tracks the *signal* rather than the
 * node set, and the roster is rebuilt and re-sent several times a second forever.
 * Channel count did exactly that until it moved into the frame, so these tests pin
 * the arrangement down from both sides -- the roster must sit still while the signal
 * moves, and must still be re-sent when the node set genuinely changes.
 */

#include "IPluginHost.h"
#include "controller/TelemetryPublisher.h"
#include "controller/internal/ControllerUtils.h"
#include "dsp/EffectRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/SpectrumTap.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace guitarfx;
using namespace guitarfx::controller_detail;

constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 128;

/// Values per node entry in a frame's "d": the four level values plus channelCount.
constexpr std::size_t kNodeTupleLength = 5;

/// Fields per roster entry: scope, presetId, nodeId, nodeType, hasAnalyzer.
constexpr std::size_t kRosterEntryLength = 5;

bool Report(const std::string& label, bool passed, const std::string& detail = {})
{
    std::cout << "  " << std::left << std::setw(58) << (label + ":") << (passed ? "PASS" : "FAIL");

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << "\n";
    return passed;
}

/// Minimal standalone host. The telemetry publisher only asks it for the sample
/// rate and block size, which ride along in the performance-stats message.
class TestHost final : public IPluginHost
{
  public:
    void SendMessageToUI(const std::string&) override
    {
    }

    void BrowseFileAsync(BrowseFileType, const std::string&, std::function<void(const BrowseFileResult&)>) override
    {
    }

    void SaveFileAsync(BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const BrowseFileResult&)>) override
    {
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        if (fn)
        {
            fn();
        }
    }

    [[nodiscard]] std::filesystem::path GetUserDataPath() const override
    {
        return {};
    }

    [[nodiscard]] std::filesystem::path GetBundledAssetsPath() const override
    {
        return {};
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return kSampleRate;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return kBlockSize;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }
};

Preset MakePreset(const std::string& id)
{
    Preset preset;
    preset.id = id;
    preset.name = id;

    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back({"gain", "gain", "utility", "Gain", true});
    graph.nodes.back().params["gainDb"] = 0.0;
    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "gain", 0, 0, 1.0});
    graph.edges.push_back({"gain", "out", 0, 0, 1.0});

    preset.graph = std::move(graph);
    return preset;
}

struct Fixture
{
    TestHost host;
    MultiPresetMixer mixer;
    std::vector<nlohmann::json> messages;
    TelemetryPublisher publisher;

    std::vector<float> inL = std::vector<float>(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> inR = std::vector<float>(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> outL = std::vector<float>(static_cast<std::size_t>(kBlockSize), 0.0f);
    std::vector<float> outR = std::vector<float>(static_cast<std::size_t>(kBlockSize), 0.0f);

    Fixture()
        : publisher(host, mixer, [this](const std::string& message) {
              messages.push_back(nlohmann::json::parse(message, nullptr, false));
          })
    {
        mixer.Prepare(kSampleRate, kBlockSize);
        mixer.SetSignalDiagnosticsEnabled(true);
        mixer.AddActivePreset(MakePreset("presetA"), "presetA", "presetA");
    }

    /// Runs one block of audio. `stereo` sets the input mode, which is what moves a node's
    /// reported channel count (never the signal), and whether the right channel carries its own
    /// content.
    void ProcessBlock(bool stereo)
    {
        mixer.SetMonoMode(!stereo);

        for (int i = 0; i < kBlockSize; ++i)
        {
            const double t = static_cast<double>(i) / kSampleRate;
            const auto left = static_cast<float>(0.3 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * t));
            inL[static_cast<std::size_t>(i)] = left;
            inR[static_cast<std::size_t>(i)] = stereo ? -left : 0.0f;
        }

        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        mixer.Process(inputs, outputs, kBlockSize);
    }

    /// Forces one diagnostics send. The publisher rate-limits to
    /// kSignalDiagnosticsRateHz against a real clock, so waiting out that interval
    /// is the only way to make consecutive sends actually go out.
    void SendDiagnostics()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kSignalDiagnosticsRateHz + 10));
        publisher.RequestSignalDiagnostics();
    }

    [[nodiscard]] std::vector<nlohmann::json> OfType(const std::string& type) const
    {
        std::vector<nlohmann::json> out;

        for (const auto& message : messages)
        {
            if (!message.is_discarded() && message.value("type", std::string{}) == type)
            {
                out.push_back(message);
            }
        }

        return out;
    }
};

/// A changing layout must not drag the roster along with it.
bool TestRosterIsStableWhileChannelCountMoves()
{
    std::cout << "\nRoster stability across signal changes\n";
    bool ok = true;

    Fixture fixture;

    // Alternate the mono and stereo input modes. Before channel count moved into the frame,
    // each flip rebuilt the roster and re-sent it.
    for (int i = 0; i < 6; ++i)
    {
        fixture.ProcessBlock(i % 2 == 0);
        fixture.SendDiagnostics();
    }

    const auto rosters = fixture.OfType("sldRoster");
    const auto frames = fixture.OfType("sld");

    ok &= Report("Several frames were sent", frames.size() >= 4, std::to_string(frames.size()) + " frames");
    ok &= Report("The roster was sent exactly once", rosters.size() == 1, std::to_string(rosters.size()) + " rosters");

    // The channel count must still be reported, and must still track the layout --
    // suppressing the roster churn by freezing the value would be no fix at all.
    bool sawMono = false;
    bool sawStereo = false;

    for (const auto& frame : frames)
    {
        const auto& values = frame["d"];

        for (std::size_t node = 0; node * kNodeTupleLength < values.size(); ++node)
        {
            const int channelCount = values[node * kNodeTupleLength + 4].get<int>();

            if (channelCount == 1)
            {
                sawMono = true;
            }
            else if (channelCount == 2)
            {
                sawStereo = true;
            }
        }
    }

    ok &= Report("Channel count still tracks the layout", sawMono && sawStereo,
                 std::string("mono=") + (sawMono ? "yes" : "no") + " stereo=" + (sawStereo ? "yes" : "no"));

    return ok;
}

/// The frame and the roster have to agree on how many values each node contributes,
/// or the UI silently drops every frame as malformed.
bool TestFrameNodeStrideMatchesRoster()
{
    std::cout << "\nFrame and roster agree on width\n";
    bool ok = true;

    Fixture fixture;
    fixture.ProcessBlock(true);
    fixture.SendDiagnostics();

    const auto rosters = fixture.OfType("sldRoster");
    const auto frames = fixture.OfType("sld");

    if (rosters.empty() || frames.empty())
    {
        return Report("A roster and a frame were sent", false);
    }

    const auto nodeCount = rosters.front()["nodes"].size();
    const auto frameValues = frames.front()["d"].size();

    ok &= Report("Frame carries one node tuple per roster node", frameValues == nodeCount * kNodeTupleLength,
                 std::to_string(frameValues) + " values for " + std::to_string(nodeCount) + " nodes");

    bool everyEntryIsRightWidth = !rosters.front()["nodes"].empty();

    for (const auto& entry : rosters.front()["nodes"])
    {
        if (entry.size() != kRosterEntryLength)
        {
            everyEntryIsRightWidth = false;
        }
    }

    ok &= Report("Roster entries carry no per-block fields", everyEntryIsRightWidth,
                 std::to_string(kRosterEntryLength) + " fields expected");

    return ok;
}

/// The other half of the contract: suppressing roster churn must not suppress a
/// roster the UI genuinely needs.
bool TestRosterIsResentWhenNodeSetChanges()
{
    std::cout << "\nRoster resend on a real node-set change\n";
    bool ok = true;

    Fixture fixture;
    fixture.ProcessBlock(true);
    fixture.SendDiagnostics();

    const auto before = fixture.OfType("sldRoster").size();

    fixture.mixer.AddActivePreset(MakePreset("presetB"), "presetB", "presetB");
    fixture.ProcessBlock(true);
    fixture.SendDiagnostics();

    const auto rosters = fixture.OfType("sldRoster");
    ok &= Report("Adding a preset re-sent the roster", rosters.size() > before,
                 std::to_string(before) + " then " + std::to_string(rosters.size()));

    if (rosters.size() >= 2)
    {
        const auto firstSeq = rosters.front().value("seq", 0u);
        const auto lastSeq = rosters.back().value("seq", 0u);
        ok &= Report("The roster sequence advanced", lastSeq > firstSeq,
                     std::to_string(firstSeq) + " -> " + std::to_string(lastSeq));

        // A frame is matched to a roster by seq, so the newest frame must quote the
        // newest roster or the UI will discard it.
        const auto frames = fixture.OfType("sld");

        if (!frames.empty())
        {
            ok &= Report("The newest frame quotes the newest roster", frames.back().value("seq", 0u) == lastSeq);
        }
    }

    return ok;
}

/// Runs enough idle ticks for the spectrum feed's divider to fire at least twice.
void TickSpectrumFeed(Fixture& fixture)
{
    for (int tick = 0; tick < 2 * (60 / kSpectrumRateHz); ++tick)
    {
        fixture.publisher.OnIdle();
    }
}

/// The EQ spectrum feed goes out only for a watched node, only while the UI can see it,
/// and stops when told to.
bool TestSpectrumFeedFollowsTheWatch()
{
    std::cout << "\nSpectrum feed\n";
    bool ok = true;

    Fixture fixture;
    fixture.ProcessBlock(false);
    TickSpectrumFeed(fixture);
    ok &= Report("Nothing is sent before a watch", fixture.OfType("sldS").empty());

    fixture.publisher.WatchSpectrum("preset", "presetA", "gain");
    fixture.ProcessBlock(false);
    TickSpectrumFeed(fixture);
    const auto frames = fixture.OfType("sldS");
    ok &= Report("A watched node's spectrum is sent", !frames.empty(), std::to_string(frames.size()) + " frames");

    if (!frames.empty())
    {
        const auto& frame = frames.back();
        ok &= Report("The frame names the watched node", frame.value("scope", std::string{}) == "preset" &&
                                                             frame.value("presetId", std::string{}) == "presetA" &&
                                                             frame.value("id", std::string{}) == "gain");
        ok &= Report("The frame carries every bin", frame["s"].size() == static_cast<std::size_t>(SpectrumTap::kBins),
                     std::to_string(frame["s"].size()) + " bins");
        ok &= Report("The frame carries its range", frame["r"].size() == 4 &&
                                                        frame["r"][0].get<double>() == SpectrumTap::kMinFrequencyHz &&
                                                        frame["r"][1].get<double>() == SpectrumTap::kMaxFrequencyHz);
        bool wholeDb = true;

        for (const auto& value : frame["s"])
        {
            wholeDb = wholeDb && value.is_number_integer();
        }

        ok &= Report("Bins go out as whole dB", wholeDb);
    }

    fixture.publisher.SetUiVisible(false);
    auto before = fixture.OfType("sldS").size();
    TickSpectrumFeed(fixture);
    ok &= Report("Nothing is sent while the UI is hidden", fixture.OfType("sldS").size() == before);
    fixture.publisher.SetUiVisible(true);

    fixture.publisher.StopSpectrum();
    before = fixture.OfType("sldS").size();
    TickSpectrumFeed(fixture);
    ok &= Report("Nothing is sent after the watch stops", fixture.OfType("sldS").size() == before);

    fixture.publisher.WatchSpectrum("preset", "presetA", "no_such_node");
    TickSpectrumFeed(fixture);
    ok &= Report("Nothing is sent for a node that is not there", fixture.OfType("sldS").size() == before);

    return ok;
}

/// Runs half a second of audio, then enough idle ticks for the loudness feed to fire twice.
void RunLoudnessFeed(Fixture& fixture)
{
    for (int block = 0; block < static_cast<int>(0.5 * kSampleRate / kBlockSize); ++block)
    {
        fixture.ProcessBlock(false);
    }

    for (int tick = 0; tick < 2 * (60 / kLoudnessRateHz); ++tick)
    {
        fixture.publisher.OnIdle();
    }
}

/// The DSP status readout's loudness goes out only for a watched node, only while the UI can see
/// it, and stops when told to.
bool TestLoudnessFeedFollowsTheWatch()
{
    std::cout << "\nLoudness feed\n";
    bool ok = true;

    Fixture fixture;
    RunLoudnessFeed(fixture);
    ok &= Report("Nothing is sent before a watch", fixture.OfType("sldL").empty());

    // The first read taps the node, so the audio before it goes unmeasured: a second of it after.
    fixture.publisher.WatchLoudness("preset", "presetA", "gain");
    RunLoudnessFeed(fixture);
    RunLoudnessFeed(fixture);
    RunLoudnessFeed(fixture);
    const auto frames = fixture.OfType("sldL");
    ok &= Report("A watched node's loudness is sent", !frames.empty(), std::to_string(frames.size()) + " frames");

    if (!frames.empty())
    {
        const auto& frame = frames.back();
        ok &= Report("The frame names the watched node", frame.value("scope", std::string{}) == "preset" &&
                                                             frame.value("presetId", std::string{}) == "presetA" &&
                                                             frame.value("id", std::string{}) == "gain");
        const auto& values = frame["l"];
        ok &= Report("The frame carries momentary, short-term, integrated and time",
                     values.is_array() && values.size() == 4, values.dump());

        if (values.is_array() && values.size() == 4)
        {
            // A 220 Hz sine at 0.3 on a mono input, so about -10 LUFS; short-term has not filled.
            ok &= Report("Momentary reads the tone",
                         values[0].is_number() && values[0].get<double>() > -16.0 && values[0].get<double>() < -4.0,
                         values[0].dump());
            ok &= Report("Short-term is null until its window fills", values[1].is_null(), values[1].dump());
            ok &= Report("Integrated reads the tone", values[2].is_number(), values[2].dump());
            ok &= Report("The time measured is about a second",
                         values[3].is_number() && std::abs(values[3].get<double>() - 1.0) < 0.2, values[3].dump());
        }
    }

    fixture.publisher.SetUiVisible(false);
    auto before = fixture.OfType("sldL").size();
    RunLoudnessFeed(fixture);
    ok &= Report("Nothing is sent while the UI is hidden", fixture.OfType("sldL").size() == before);
    fixture.publisher.SetUiVisible(true);

    fixture.publisher.StopLoudness();
    before = fixture.OfType("sldL").size();
    RunLoudnessFeed(fixture);
    ok &= Report("Nothing is sent after the watch stops", fixture.OfType("sldL").size() == before);

    fixture.publisher.WatchLoudness("preset", "presetA", "no_such_node");
    RunLoudnessFeed(fixture);
    ok &= Report("Nothing is sent for a node that is not there", fixture.OfType("sldL").size() == before);

    return ok;
}

/// Frames of one slot: 0 for a node's readout (no slot field), 1 for the app's output.
std::vector<nlohmann::json> LoudnessFrames(const Fixture& fixture, int slot)
{
    std::vector<nlohmann::json> out;

    for (const auto& frame : fixture.OfType("sldL"))
    {
        if (frame.value("slot", 0) == slot)
        {
            out.push_back(frame);
        }
    }

    return out;
}

/// The output meter's loudness runs beside a node's, and stops on its own.
bool TestOutputLoudnessFeed()
{
    std::cout << "\nOutput loudness feed\n";
    bool ok = true;

    Fixture fixture;
    fixture.publisher.WatchOutputLoudness();
    fixture.publisher.WatchLoudness("preset", "presetA", "gain");
    RunLoudnessFeed(fixture);
    RunLoudnessFeed(fixture);
    RunLoudnessFeed(fixture);

    const auto output = LoudnessFrames(fixture, 1);
    const auto node = LoudnessFrames(fixture, 0);
    ok &= Report("The output's loudness is sent", !output.empty(), std::to_string(output.size()) + " frames");
    ok &= Report("...alongside the node's", !node.empty(), std::to_string(node.size()) + " frames");

    if (!output.empty())
    {
        const auto& frame = output.back();
        ok &= Report("The frame names the output and no node",
                     frame.value("scope", std::string{}) == "output" && !frame.contains("id"));
        ok &= Report("Momentary reads the tone",
                     frame["l"].is_array() && frame["l"].size() == 4 && frame["l"][0].is_number(), frame["l"].dump());
    }

    fixture.publisher.StopOutputLoudness();
    const auto outputBefore = LoudnessFrames(fixture, 1).size();
    const auto nodeBefore = LoudnessFrames(fixture, 0).size();
    RunLoudnessFeed(fixture);
    ok &= Report("Stopping the output stops only its frames",
                 LoudnessFrames(fixture, 1).size() == outputBefore && LoudnessFrames(fixture, 0).size() > nodeBefore);
    return ok;
}

/// Every performance frame carries the worst block since the one before.
bool TestPerformanceFrameCarriesThePeak()
{
    std::cout << "\nPeak DSP load\n";
    bool ok = true;

    Fixture fixture;

    for (int block = 0; block < 20; ++block)
    {
        fixture.ProcessBlock(false);
    }

    fixture.publisher.RequestPerformanceStats();
    const auto frames = fixture.OfType("dspPerformance");
    ok &= Report("A performance frame is sent", !frames.empty());

    if (!frames.empty())
    {
        const auto& stats = frames.back()["stats"];
        const double peak = stats.value("peakDspLoadPercent", -1.0);
        const double last = stats.value("dspLoadPercent", -1.0);
        ok &= Report("It carries the peak load", peak > 0.0, std::to_string(peak) + "%");
        ok &= Report("The peak is no lower than the last block", peak + 1e-9 >= last,
                     std::to_string(peak) + "% vs " + std::to_string(last) + "%");
    }

    return ok;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool allPassed = true;

    for (const auto& test :
         {TestRosterIsStableWhileChannelCountMoves, TestFrameNodeStrideMatchesRoster,
          TestRosterIsResentWhenNodeSetChanges, TestSpectrumFeedFollowsTheWatch, TestLoudnessFeedFollowsTheWatch,
          TestOutputLoudnessFeed, TestPerformanceFrameCarriesThePeak})
    {
        if (!test())
        {
            allPassed = false;
        }
    }

    std::cout << "\n" << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return allPassed ? 0 : 1;
}
