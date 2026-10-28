/**
 * @file AudioPerf.cpp
 * @brief Times effects and small chains through a SignalGraphExecutor, so two builds of the app
 *        can be compared for DSP CPU the way AudioSnapshot compares their sound.
 *
 * Not a ctest test. `tools/audio-ab/audio-ab.mjs perf` builds this against each revision it
 * compares, copying it into older checkouts with the rest of the harness, then runs the
 * revisions' builds in turn, round after round, and keeps each case's best round. See
 * tools/audio-ab/README.md.
 *
 * Each case in the definition's "perfCases" is built like a chain (BuildChain: node types by
 * alias, fresh-node params, test resources) and run on its own SignalGraphExecutor, as the
 * effects pass runs one effect: NAM nodes get the interface calibration the controller injects
 * at default settings, unless the case sets one. The input is the demo DI, looped. It is mono by
 * default, as the app's default input is: the Mono input layout on revisions that have one, L ==
 * R on older ones, which detect that themselves. With --stereo the right channel is another part
 * of the DI and the layout is Stereo. Denormals are flushed, the thread runs at high priority
 * pinned to one core, and a second of audio warms each case up before timing starts.
 *
 * Usage:
 *   AudioPerf --definition FILE --assets DIR --di FILE [--rate 48000] [--block 64]
 *             [--seconds 10] [--rounds 1] [--filter TEXT] [--stereo] [--core 4]
 *
 * Prints a "config {json}" line, then a "result {json}" line per case per round: id, name,
 * round, meanUs, medianUs, p99Us, maxUs, blocks, rms, latencySamples and passthrough (every
 * output sample equal to its input: nothing was processed). A case that cannot be built prints
 * id, name and error instead.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "AudioSnapshotChains.h"
#include "AudioSnapshotSupport.h"
#include "dsp/EffectRegistry.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"

// 1.6.0 on: the mixer gives the executor a Mono input layout for the default mono input. 1.5.0
// has no layout; it runs the mono path by itself when both channels are identical.
#if __has_include("dsp/ChannelLayout.h")
    #include "dsp/ChannelLayout.h"
    #define AUDIO_PERF_HAS_LAYOUT 1
#endif

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__linux__)
    #include <pthread.h>
    #include <sched.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using namespace audio_snapshot;

namespace
{
constexpr int kHarnessVersion = 1;
constexpr double kWarmUpSeconds = 1.0;

struct Options
{
    fs::path definition;
    fs::path assets;
    fs::path di;
    double sampleRate = 48000.0;
    int block = 64;
    double seconds = 10.0;
    int rounds = 1;
    std::string filter;
    bool stereo = false;
    int core = 4;
};

bool ParseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string{}; };

        if (a == "--definition")
        {
            o.definition = next();
        }
        else if (a == "--assets")
        {
            o.assets = next();
        }
        else if (a == "--di")
        {
            o.di = next();
        }
        else if (a == "--rate")
        {
            o.sampleRate = std::atof(next().c_str());
        }
        else if (a == "--block")
        {
            o.block = std::atoi(next().c_str());
        }
        else if (a == "--seconds")
        {
            o.seconds = std::atof(next().c_str());
        }
        else if (a == "--rounds")
        {
            o.rounds = std::atoi(next().c_str());
        }
        else if (a == "--filter")
        {
            o.filter = next();
        }
        else if (a == "--stereo")
        {
            o.stereo = true;
        }
        else if (a == "--core")
        {
            o.core = std::atoi(next().c_str());
        }
        else
        {
            std::cerr << "unknown argument: " << a << "\n";
            return false;
        }
    }

    return !o.definition.empty() && !o.assets.empty() && !o.di.empty() && o.sampleRate > 0.0 && o.block > 0 &&
           o.seconds > 0.0 && o.rounds > 0;
}

/// Pins this thread to one logical core and raises its priority, so the scheduler moving it,
/// or another process taking its core, does not show up as DSP cost. Returns the core, or -1.
int PinThread(int core)
{
    const int cores = static_cast<int>(std::thread::hardware_concurrency());

    if (core < 0 || cores <= 0)
    {
        return -1;
    }

    core = std::min(core, cores - 1);
#if defined(_WIN32)
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << core) != 0 ? core : -1;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0 ? core : -1;
#else
    return -1;
#endif
}

#if defined(AUDIO_PERF_HAS_LAYOUT)
template <typename Executor> void SetInputLayout(Executor& dsp, bool stereo)
{
    if constexpr (requires { dsp.SetInputLayout(guitarfx::ChannelLayout::Mono); })
    {
        dsp.SetInputLayout(stereo ? guitarfx::ChannelLayout::Stereo : guitarfx::ChannelLayout::Mono);
    }
}
#else
template <typename Executor> void SetInputLayout(Executor&, bool)
{
}
#endif

struct Case
{
    std::string id;
    std::string name;
    guitarfx::SignalGraph graph;
    std::string error;
};

Case BuildCase(const json& spec, const Resources& resources)
{
    Case c;
    c.id = spec.at("id").get<std::string>();
    c.name = spec.value("name", c.id);
    ChainBuild chain = BuildChain(spec, resources);

    if (!chain.error.empty())
    {
        c.error = chain.error;
        return c;
    }

    // The effects pass's stand-in for the controller: the NAM calibration it injects at default
    // settings, unless the case sets its own.
    const json nodes = spec.at("nodes");
    std::size_t index = 0;

    for (auto& node : chain.preset.graph.nodes)
    {
        if (node.type == guitarfx::kNodeTypeInput || node.type == guitarfx::kNodeTypeOutput)
        {
            continue;
        }

        const json params = nodes.at(index++).value("params", json::object());

        if (IsNamCalibratable(node.type) && !params.contains("calibrationInputLevel"))
        {
            node.params["calibrationInputLevel"] = kDefaultNamInterfaceCalibrationDbu;
            node.params["calibrationInputLevelEnabled"] = 1.0;
        }
    }

    c.graph = std::move(chain.preset.graph);
    return c;
}

json TimeCase(const Case& c, const std::vector<float>& left, const std::vector<float>& right, const Options& o)
{
    using Clock = std::chrono::steady_clock;
    auto dsp = std::make_unique<guitarfx::SignalGraphExecutor>();
    dsp->SetInputTrim(0.0);
    dsp->SetOutputTrim(0.0);
    dsp->SetTempo(kTempoBpm);
    SetInputLayout(*dsp, o.stereo);
    dsp->SetGraph(c.graph);
    dsp->Prepare(o.sampleRate, o.block);
    const int latency = dsp->GetTotalLatencySamples();

    const ScopedFlushDenormals noDenormals;
    const auto blockSize = static_cast<std::size_t>(o.block);
    const auto warmBlocks = static_cast<std::size_t>(o.sampleRate * kWarmUpSeconds) / blockSize;
    const auto timedBlocks = std::max<std::size_t>(1, static_cast<std::size_t>(o.sampleRate * o.seconds) / blockSize);
    std::vector<float> inL(blockSize), inR(blockSize), outL(blockSize), outR(blockSize);
    std::vector<double> us;
    us.reserve(timedBlocks);
    double sumSq = 0.0;
    bool passthrough = true;
    std::size_t pos = 0;

    for (std::size_t b = 0; b < warmBlocks + timedBlocks; ++b)
    {
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            inL[i] = left[pos];
            inR[i] = right[pos];
            pos = (pos + 1) % left.size();
        }

        float* ins[2] = {inL.data(), inR.data()};
        float* outs[2] = {outL.data(), outR.data()};
        const auto t0 = Clock::now();
        dsp->Process(ins, outs, o.block);
        const auto t1 = Clock::now();

        if (b < warmBlocks)
        {
            continue;
        }

        us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());

        for (std::size_t i = 0; i < blockSize; ++i)
        {
            sumSq += static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i];
        }

        // Compared with the block as fed, not inL/inR: an executor may process in place.
        if (passthrough)
        {
            const std::size_t from = (pos + left.size() - blockSize) % left.size();

            for (std::size_t i = 0; i < blockSize && passthrough; ++i)
            {
                const std::size_t at = (from + i) % left.size();
                passthrough = outL[i] == left[at] && outR[i] == right[at];
            }
        }
    }

    double mean = 0.0;

    for (const double v : us)
    {
        mean += v;
    }

    mean /= static_cast<double>(us.size());
    std::sort(us.begin(), us.end());
    const auto quantile = [&](double q) {
        return us[std::min(us.size() - 1, static_cast<std::size_t>(q * static_cast<double>(us.size())))];
    };

    return {{"meanUs", mean},
            {"medianUs", quantile(0.5)},
            {"p99Us", quantile(0.99)},
            {"maxUs", us.back()},
            {"blocks", us.size()},
            {"rms", std::sqrt(sumSq / (2.0 * static_cast<double>(us.size() * blockSize)))},
            {"latencySamples", latency},
            {"passthrough", passthrough}};
}

void Print(const char* kind, const json& j)
{
    std::printf("%s %s\n", kind, j.dump().c_str());
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    Options o;

    if (!ParseArgs(argc, argv, o))
    {
        std::cerr << "usage: AudioPerf --definition FILE --assets DIR --di FILE [--rate 48000] [--block 64]\n"
                     "                 [--seconds 10] [--rounds 1] [--filter TEXT] [--stereo] [--core 4]\n";
        return 2;
    }

    try
    {
        const int pinned = PinThread(o.core);
        const json def = LoadJson(o.definition);
        const Resources resources = LoadResources(def, o.assets);

        double diRate = 0.0;
        const auto diRaw = ReadAudioMono(o.di, diRate);
        Stimulus stim;

        if (diRaw.empty() || !MakeStimulus("di", o.sampleRate, Resample(diRaw, diRate, o.sampleRate), stim))
        {
            std::cerr << "cannot read the DI " << o.di.string() << "\n";
            return 1;
        }

        // The six seconds of playing, without the tail of silence the renders leave.
        const std::vector<float> left(stim.samples.begin(),
                                      stim.samples.begin() + static_cast<std::ptrdiff_t>(stim.playFrames));
        std::vector<float> right = left;

        if (o.stereo)
        {
            std::rotate(right.begin(), right.begin() + static_cast<std::ptrdiff_t>(right.size() / 2), right.end());
        }

        guitarfx::RegisterAllEffects();
        std::vector<Case> cases;
        const json specs = def.value("perfCases", json::array());

        for (const auto& spec : specs)
        {
            const std::string id = spec.at("id").get<std::string>();
            const std::string name = spec.value("name", id);

            if (o.filter.empty() || id.find(o.filter) != std::string::npos || name.find(o.filter) != std::string::npos)
            {
                cases.push_back(BuildCase(spec, resources));
            }
        }

#if defined(AUDIO_PERF_HAS_LAYOUT)
        const char* layout = o.stereo ? "Stereo layout" : "Mono layout";
#else
        const char* layout = o.stereo ? "no layout, L != R" : "no layout, L == R";
#endif
        Print("config", {{"harnessVersion", kHarnessVersion},
                         {"sampleRate", o.sampleRate},
                         {"block", o.block},
                         {"seconds", o.seconds},
                         {"stereo", o.stereo},
                         {"layout", layout},
                         {"pinnedCore", pinned},
                         {"cases", cases.size()}});

        for (int round = 1; round <= o.rounds; ++round)
        {
            for (const auto& c : cases)
            {
                json result = {{"id", c.id}, {"name", c.name}, {"round", round}};

                if (!c.error.empty())
                {
                    result["error"] = c.error;
                }
                else
                {
                    try
                    {
                        result.update(TimeCase(c, left, right, o));
                    }
                    catch (const std::exception& e)
                    {
                        result["error"] = e.what();
                    }
                }

                Print("result", result);
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "AudioPerf: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
