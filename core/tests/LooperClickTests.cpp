// The loop's click: the metronome follows a loop through the controller. Silent while the loop is
// stopped, and on the loop's own place in the metronome's bar whenever it plays: after a Stop and
// Play, after the click is switched on mid-loop, after a tempo change, and after a project is opened
// again. The loop is recorded from silence, so every onset in the output is a click.

#include "LooperControllerSession.h"
#include "controller/LooperProjectFiles.h"
#include "dsp/effects/BuiltinEffects.h"

#include <filesystem>

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace looper_session;

namespace
{
constexpr double kBpm = 240.0;

/// Samples where a click starts in out[from, to): above the threshold after a long quiet.
std::vector<std::size_t> Onsets(const std::vector<float>& out, std::size_t from, std::size_t to)
{
    std::vector<std::size_t> onsets;
    // A window opening part way into a click does not count where it opens.
    std::size_t quiet = 0;

    while (quiet < 4000 && quiet < from && std::abs(out[from - 1 - quiet]) <= 0.004f)
    {
        ++quiet;
    }

    quiet = quiet >= std::min<std::size_t>(4000, from) ? 4000 : quiet;

    for (std::size_t i = from; i < std::min(to, out.size()); ++i)
    {
        if (std::abs(out[i]) > 0.004f)
        {
            if (quiet >= 4000)
            {
                onsets.push_back(i);
            }

            quiet = 0;
        }
        else
        {
            ++quiet;
        }
    }

    return onsets;
}

struct Loop
{
    double clickBar = 0.0;
    std::int64_t length = 0;
    std::int64_t playheadAt = 0; ///< the playhead at sample `at`
    std::size_t at = 0;
};

/// The loop as the looper reports it now, at the session's next sample. The state is sent at most
/// so often, so one block at a time until a new one comes: it was read just after that block.
Loop Read(Session& s)
{
    const int sent = s.host.Count("looperState");

    for (int i = 0; i < 4000 && s.host.Count("looperState") == sent; ++i)
    {
        s.Run(static_cast<double>(kBlock) / kSampleRate);
    }

    const auto state = s.State();
    Loop loop;
    loop.clickBar = state.value("clickBar", 0.0);
    loop.length = std::llround(state.value("lengthSec", 0.0) * kSampleRate);
    loop.playheadAt = std::llround(state.value("positionSec", 0.0) * kSampleRate);
    loop.at = s.phase;
    return loop;
}

/// Every click in out[from, to) falls on a beat of the loop's bar, and there are some. The loop plays
/// throughout, so the playhead at sample t is (playheadAt + t - at) mod length.
void CheckOnTheLoop(const std::vector<float>& out, const Loop& loop, std::size_t from, std::size_t to, double bpm,
                    const std::string& what)
{
    const double beat = kSampleRate * 60.0 / bpm;
    const double bar = 4.0 * beat;
    const auto onsets = Onsets(out, from, to);
    double worst = 0.0;

    for (const std::size_t t : onsets)
    {
        const std::int64_t rel = static_cast<std::int64_t>(t) - static_cast<std::int64_t>(loop.at);
        const std::int64_t playhead = ((loop.playheadAt + rel) % loop.length + loop.length) % loop.length;
        const double place = std::fmod(loop.clickBar * bar + static_cast<double>(playhead), beat);
        worst = std::max(worst, std::min(place, beat - place));
    }

    Check(onsets.size() >= 3, what + ": the click plays (" + std::to_string(onsets.size()) + " onsets)");
    Check(worst <= 2.0, what + ": every click is on the loop's beat, worst " + std::to_string(worst) + " samples off");
}

void TestTheClickFollowsTheLoop(const fs::path& sandbox)
{
    LooperHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();
    std::vector<float> out;
    Session s{host, controller};
    s.capture = &out;

    s.Send({{"type", "setMetronome"}, {"enabled", true}, {"bpm", kBpm}, {"timeSigNum", 4}, {"timeSigDen", 4}});
    s.Send({{"type", "setLooper"},
            {"enabled", true},
            {"maxSeconds", 30},
            {"length", "free"},
            {"clickWhileRecording", true}});
    s.Run(0.37);

    // A free loop of about a bar and a third, started off the beat, played to the click.
    s.Command("recPlayDub");
    s.Run(1.31);
    s.Command("recPlayDub");
    s.Run(0.2);
    Loop loop = Read(s);
    Check(loop.length > 0 && s.State().value("state", std::string{}) == "playing", "a loop plays");
    Check(s.State().value("click", false), "the state says the click is on");
    std::size_t from = s.phase;
    s.Run(2.0);
    CheckOnTheLoop(out, loop, from, s.phase, kBpm, "the first passes");

    // Stopped: no click at all.
    s.Command("stop");
    s.Run(0.1);
    from = s.phase;
    s.Run(1.13);
    Check(Onsets(out, from, s.phase).empty(), "silent while the loop is stopped");

    // Play again, anywhere in the click's old bar: the click comes back on the loop.
    from = s.phase;
    s.Command("recPlayDub");
    s.Run(0.05);
    loop = Read(s);
    s.Run(2.5);
    CheckOnTheLoop(out, loop, from, s.phase, kBpm, "after Stop and Play");

    // Switched off and on mid-loop: back on the loop at once.
    s.Send({{"type", "setMetronome"}, {"enabled", false}});
    s.Run(0.29);
    s.Send({{"type", "setMetronome"}, {"enabled", true}});
    from = s.phase;
    s.Run(2.0);
    loop = Read(s);
    CheckOnTheLoop(out, loop, from, s.phase, kBpm, "after the click is switched on mid-loop");

    // The metronome's tempo is the loop's bar: a new tempo, and the click is on the loop at it.
    constexpr double kNewBpm = 200.0;
    s.Send({{"type", "setMetronome"}, {"bpm", kNewBpm}});
    s.Run(0.1);
    from = s.phase;
    s.Run(2.0);
    loop = Read(s);
    CheckOnTheLoop(out, loop, from, s.phase, kNewBpm, "after a tempo change");

    // Saved with its click, and opened again with the metronome off: the project turns it on, at its
    // tempo, and the click is on the loop when it plays.
    s.Send({{"type", "getRiffLibrary"}});
    s.Run(0.01);
    const fs::path loops = fs::path(host.Last("riffLibraryState")["library"].value("path", std::string{})) / "loops";
    const double clickBar = s.State().value("clickBar", -1.0);
    s.Send({{"type", "saveLooperProject"}, {"title", "Click"}});
    s.Settle(800);
    const std::string id = s.State().value("projectId", std::string{});
    const auto project = looper_files::ReadJson(loops / id / "project.json");
    Check(project && project->value("click", false) && std::abs(project->value("clickBar", -1.0) - clickBar) < 1e-12 &&
              std::abs(project->value("tempoBpm", 0.0) - kNewBpm) < 1e-9,
          "the project keeps its click, where the loop starts in the bar, and the tempo: " +
              (project ? project->dump().substr(0, 200) : std::string("none")));
    s.Command("clear");
    s.Run(0.2);
    s.Send({{"type", "setMetronome"}, {"enabled", false}, {"bpm", 120.0}});
    s.Run(0.05);
    s.Send({{"type", "loadLooperProject"}, {"projectId", id}});
    s.Settle(800);
    auto state = s.State();
    Check(state.value("layerCount", 0) == 1 && state.value("click", false) &&
              std::abs(state.value("clickBar", -1.0) - clickBar) < 1e-12,
          "the project brings its click back on: " + state.dump().substr(0, 300));
    Check(!state.value("dirty", true), "and is not unsaved for it");
    from = s.phase;
    s.Run(0.6);
    Check(Onsets(out, from, s.phase).empty(), "an opened loop is stopped, and so is its click");
    from = s.phase;
    s.Command("play");
    s.Run(0.05);
    loop = Read(s);
    s.Run(2.0);
    CheckOnTheLoop(out, loop, from, s.phase, kNewBpm, "an opened project");

    // Cleared: the metronome is its own again, and runs.
    s.Command("clear");
    s.Run(0.3);
    from = s.phase;
    s.Run(1.0);
    Check(Onsets(out, from, s.phase).size() >= 3, "with no loop the metronome runs free");
}

void TestABarsLoopAfterStopAndPlay(const fs::path& sandbox)
{
    LooperHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();
    std::vector<float> out;
    Session s{host, controller};
    s.capture = &out;

    s.Send({{"type", "setMetronome"}, {"enabled", true}, {"bpm", kBpm}, {"timeSigNum", 4}, {"timeSigDen", 4}});
    s.Send({{"type", "setLooper"},
            {"enabled", true},
            {"maxSeconds", 30},
            {"length", "bars"},
            {"bars", 1},
            {"countInBars", 1},
            {"clickWhileRecording", true}});
    s.Run(0.21);
    s.Command("recPlayDub");
    s.Run(2.5);
    Check(s.State().value("state", std::string{}) == "playing" && std::abs(s.State().value("clickBar", -1.0)) < 1e-12,
          "a Bars loop starts on bar 1: " + s.State().dump().substr(0, 200));
    s.Command("stop");
    s.Run(0.77);
    const std::size_t from = s.phase;
    s.Command("play");
    s.Run(0.05);
    const Loop loop = Read(s);
    s.Run(2.0);
    CheckOnTheLoop(out, loop, from, s.phase, kBpm, "a Bars loop after Stop and Play");
    const auto onsets = Onsets(out, from, s.phase);
    Check(!onsets.empty() && onsets.front() - from <= 2, "the downbeat plays on the press that starts the loop");
    s.Command("clear");
    s.Run(0.2);
    s.Send({{"type", "setLooper"}, {"length", "free"}});
}

/// An armed Bars take started half way into a bar starts on that beat, and the click follows it.
void TestAnArmedTakeOnABeat(const fs::path& sandbox)
{
    LooperHost host(sandbox, std::this_thread::get_id(), kSampleRate, kBlock);
    PluginController controller(host);
    controller.Initialize();
    controller.Prepare(kSampleRate, kBlock);
    host.Pump();
    std::vector<float> out;
    Session s{host, controller};
    s.capture = &out;

    s.Send({{"type", "setMetronome"}, {"enabled", true}, {"bpm", kBpm}, {"timeSigNum", 4}, {"timeSigDen", 4}});
    s.Send({{"type", "setLooper"},
            {"enabled", true},
            {"maxSeconds", 30},
            {"length", "bars"},
            {"bars", 1},
            {"start", "armed"},
            {"countInBars", 0},
            {"clickWhileRecording", true}});
    s.Run(0.3);
    s.Command("recPlayDub");
    // A bar is a second at 240 BPM, from the click starting at the first block: play on its third beat.
    s.Run(1.5 - static_cast<double>(s.phase) / kSampleRate + 0.02);
    s.playing = true;
    s.Run(1.3);
    s.playing = false;
    s.Run(0.2);
    const auto state = s.State();
    const double clickBar = state.value("clickBar", -1.0);
    Check(state.value("state", std::string{}) == "playing" && std::abs(clickBar - 0.5) < 0.01,
          "an armed take played on the third beat starts on it: " + state.dump().substr(0, 260));
    // Muted, so the click is all there is to hear.
    const auto layers = host.Last("looperLayers")["layers"];
    s.Send({{"type", "setLooperLayer"},
            {"layerId", layers.empty() ? std::string{} : layers[0].value("id", std::string{})},
            {"muted", true}});
    s.Command("stop");
    s.Run(0.61);
    const std::size_t from = s.phase;
    s.Command("play");
    s.Run(0.05);
    const Loop loop = Read(s);
    s.Run(2.0);
    CheckOnTheLoop(out, loop, from, s.phase, kBpm, "an armed take on a beat after Stop and Play");
    s.Command("clear");
    s.Run(0.2);
    s.Send({{"type", "setLooper"}, {"length", "free"}, {"start", "press"}});
}
} // namespace

int main()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-looper-click-tests";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    RegisterAllEffects();

    TestTheClickFollowsTheLoop(sandbox);
    TestABarsLoopAfterStopAndPlay(sandbox);
    TestAnArmedTakeOnABeat(sandbox);
    fs::remove_all(sandbox, ec);

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "LooperClickTests passed" << std::endl;
    return 0;
}
