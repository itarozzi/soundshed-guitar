/**
 * @file LoudnessTapTests.cpp
 * @brief The loudness in a node's DSP status readout: what StreamingLoudnessMeter measures, what
 *        LoudnessTap hands it, and which signal the executor and mixer tap.
 *
 *  - The streaming meter reads the BS.1770 reference tone as the offline meter does (-20.0 LUFS
 *    for a 997 Hz sine at -20 dBFS on both channels, -23.0 on one), its momentary and short-term
 *    windows read nothing until they have filled, and its integrated value is the offline
 *    meter's for the same samples, gaps and level changes included.
 *  - The tap gives the meter exactly what was pushed, whatever the block size and however the
 *    reads fall; it skips ahead rather than misreading when it falls behind, and once samples stop
 *    it counts the time as silence.
 *  - The executor taps the watched node's output, after it has run, and the tap adds no allocation
 *    to the audio path.
 *  - The mixer measures the app's output where the output meter does, after the master volume,
 *    and keeps the worst block's DSP load for the performance feed.
 */

#include "dsp/LoudnessTap.h"
#include "dsp/Loudness.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
/// Counts heap allocations, for the audio-path check. Every thread's count, so the check runs
/// with nothing else going on.
std::atomic<std::size_t> gAllocationCount{0};
} // namespace

void* operator new(std::size_t size)
{
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);

    if (void* pointer = std::malloc(std::max<std::size_t>(size, 1)))
    {
        return pointer;
    }

    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    std::free(pointer);
}

namespace
{
using namespace guitarfx;
using Clock = std::chrono::steady_clock;

constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;
/// Amplitude of a sine at -20 dBFS.
constexpr double kMinus20Dbfs = 0.1;

bool Report(const std::string& label, bool passed, const std::string& detail = {})
{
    std::cout << "  " << std::left << std::setw(66) << (label + ":") << (passed ? "PASS" : "FAIL");

    if (!detail.empty())
    {
        std::cout << " (" << detail << ")";
    }

    std::cout << "\n";
    return passed;
}

std::string Lufs(const std::optional<double>& value)
{
    if (!value)
    {
        return "none";
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << *value << " LUFS";
    return out.str();
}

bool Near(const std::optional<double>& value, double expected, double tolerance)
{
    return value && std::abs(*value - expected) <= tolerance;
}

/// A continuous sine, handed out a block at a time.
class SineSource
{
  public:
    SineSource(double frequencyHz, double amplitude, double sampleRate = kSampleRate)
        : mFrequencyHz(frequencyHz), mAmplitude(amplitude), mSampleRate(sampleRate)
    {
    }

    void SetAmplitude(double amplitude)
    {
        mAmplitude = amplitude;
    }

    void Fill(float* block, std::size_t frames)
    {
        for (std::size_t i = 0; i < frames; ++i)
        {
            block[i] = static_cast<float>(mAmplitude * std::sin(2.0 * kPi * mFrequencyHz * mTime));
            mTime += 1.0 / mSampleRate;
        }
    }

    std::vector<float> Take(double seconds)
    {
        std::vector<float> samples(static_cast<std::size_t>(std::llround(seconds * mSampleRate)));
        Fill(samples.data(), samples.size());
        return samples;
    }

  private:
    double mFrequencyHz;
    double mAmplitude;
    double mSampleRate;
    double mTime = 0.0;
};

// ── The meter ────────────────────────────────────────────────────────────────

bool TestMeterReadsTheReferenceTone()
{
    std::cout << "\nThe streaming meter reads the reference tone\n";
    bool ok = true;

    for (const double rate : {44100.0, 48000.0, 96000.0})
    {
        SineSource sine(997.0, kMinus20Dbfs, rate);
        const auto tone = sine.Take(4.0);
        const std::vector<float> silent(tone.size(), 0.0f);
        const std::string at = " at " + std::to_string(static_cast<int>(rate)) + " Hz";

        loudness::StreamingLoudnessMeter both;
        both.Prepare(rate);
        both.Push(tone.data(), tone.data(), tone.size());
        ok &= Report("Both channels: momentary -20.0" + at, Near(both.MomentaryLufs(), -20.0, 0.05),
                     Lufs(both.MomentaryLufs()));
        ok &= Report("Both channels: short-term -20.0" + at, Near(both.ShortTermLufs(), -20.0, 0.05),
                     Lufs(both.ShortTermLufs()));
        ok &= Report("Both channels: integrated -20.0" + at, Near(both.IntegratedLufs(), -20.0, 0.05),
                     Lufs(both.IntegratedLufs()));

        loudness::StreamingLoudnessMeter mono;
        mono.Prepare(rate);
        mono.Push(tone.data(), nullptr, tone.size());
        ok &= Report("Mono counts on both channels" + at, Near(mono.IntegratedLufs(), -20.0, 0.05),
                     Lufs(mono.IntegratedLufs()));

        loudness::StreamingLoudnessMeter oneSide;
        oneSide.Prepare(rate);
        oneSide.Push(tone.data(), silent.data(), tone.size());
        ok &= Report("One channel reads -23.0" + at, Near(oneSide.IntegratedLufs(), -23.0, 0.05),
                     Lufs(oneSide.IntegratedLufs()));
    }

    return ok;
}

bool TestMeterWindowsFillBeforeReading()
{
    std::cout << "\nEach window reads nothing until it has filled\n";
    bool ok = true;

    SineSource sine(997.0, kMinus20Dbfs);
    loudness::StreamingLoudnessMeter meter;
    meter.Prepare(kSampleRate);

    const auto first = sine.Take(0.39);
    meter.Push(first.data(), nullptr, first.size());
    ok &= Report("390 ms: no momentary yet", !meter.MomentaryLufs().has_value(), Lufs(meter.MomentaryLufs()));
    ok &= Report("390 ms: no integrated yet", !meter.IntegratedLufs().has_value(), Lufs(meter.IntegratedLufs()));

    const auto toFourHundred = sine.Take(0.01);
    meter.Push(toFourHundred.data(), nullptr, toFourHundred.size());
    ok &= Report("400 ms: momentary reads", Near(meter.MomentaryLufs(), -20.0, 0.05), Lufs(meter.MomentaryLufs()));
    ok &= Report("400 ms: integrated reads", Near(meter.IntegratedLufs(), -20.0, 0.05), Lufs(meter.IntegratedLufs()));
    ok &= Report("400 ms: no short-term yet", !meter.ShortTermLufs().has_value(), Lufs(meter.ShortTermLufs()));

    const auto toThree = sine.Take(2.6);
    meter.Push(toThree.data(), nullptr, toThree.size());
    ok &= Report("3 s: short-term reads", Near(meter.ShortTermLufs(), -20.0, 0.05), Lufs(meter.ShortTermLufs()));
    ok &= Report("Measured time is what was pushed", std::abs(meter.MeasuredSeconds() - 3.0) < 1e-9);

    meter.Reset();
    ok &= Report("A reset forgets everything", !meter.MomentaryLufs() && !meter.ShortTermLufs() &&
                                                   !meter.IntegratedLufs() && meter.MeasuredSeconds() == 0.0);
    return ok;
}

bool TestMeterIntegratedMatchesTheOfflineMeter()
{
    std::cout << "\nIntegrated loudness is the offline meter's\n";
    bool ok = true;

    // Loud, quiet enough to fall under the relative gate, silent, then mid: every gate gets used.
    SineSource sine(440.0, 0.0);
    std::vector<float> left;
    std::vector<float> right;

    for (const auto& [amplitude, seconds] :
         std::vector<std::pair<double, double>>{{0.3, 2.05}, {0.01, 1.5}, {0.0, 1.0}, {0.1, 2.3}})
    {
        sine.SetAmplitude(amplitude);
        const auto piece = sine.Take(seconds);
        left.insert(left.end(), piece.begin(), piece.end());

        for (const float sample : piece)
        {
            right.push_back(0.5f * sample);
        }
    }

    const auto offline = loudness::MeasureIntegratedLoudness(left, right, kSampleRate);

    // Pushed in uneven pieces, as blocks arrive.
    loudness::StreamingLoudnessMeter meter;
    meter.Prepare(kSampleRate);

    for (std::size_t position = 0, piece = 37; position < left.size(); piece = (piece * 7) % 1013 + 1)
    {
        const std::size_t count = std::min(piece, left.size() - position);
        meter.Push(left.data() + position, right.data() + position, count);
        position += count;
    }

    ok &= Report("Matches IntegratedLoudnessMeter",
                 offline.integratedLufs && meter.IntegratedLufs() &&
                     std::abs(*offline.integratedLufs - *meter.IntegratedLufs()) < 1e-6,
                 Lufs(meter.IntegratedLufs()) + " vs " + Lufs(offline.integratedLufs));
    return ok;
}

bool TestMeterSilence()
{
    std::cout << "\nSilence\n";
    bool ok = true;

    SineSource sine(997.0, kMinus20Dbfs);
    loudness::StreamingLoudnessMeter meter;
    meter.Prepare(kSampleRate);
    auto tone = sine.Take(4.0);
    meter.Push(tone.data(), nullptr, tone.size());

    meter.PushSilence(static_cast<std::size_t>(0.5 * kSampleRate));
    ok &= Report("Half a second of silence empties momentary", !meter.MomentaryLufs().has_value(),
                 Lufs(meter.MomentaryLufs()));
    ok &= Report("...while short-term still has tone in its window",
                 meter.ShortTermLufs().has_value() && *meter.ShortTermLufs() < -20.0, Lufs(meter.ShortTermLufs()));

    // The blocks straddling the stop are part tone and pass both gates, as BS.1770 has it; the
    // offline meter reads the same samples the same way.
    const std::size_t toneFrames = tone.size();
    tone.resize(toneFrames + static_cast<std::size_t>(0.5 * kSampleRate), 0.0f);
    const auto offline = loudness::MeasureIntegratedLoudness(tone.data(), nullptr, tone.size(), kSampleRate);
    const auto afterStop = meter.IntegratedLufs();
    ok &= Report("The stop reads as the offline meter reads it",
                 afterStop && offline.integratedLufs && std::abs(*afterStop - *offline.integratedLufs) < 1e-6,
                 Lufs(afterStop) + " vs " + Lufs(offline.integratedLufs));

    meter.PushSilence(static_cast<std::size_t>(30.0 * kSampleRate));
    ok &= Report("Long silence empties short-term", !meter.ShortTermLufs().has_value(), Lufs(meter.ShortTermLufs()));
    ok &= Report("...and changes integrated no further", meter.IntegratedLufs() == afterStop,
                 Lufs(meter.IntegratedLufs()));

    loudness::StreamingLoudnessMeter quiet;
    quiet.Prepare(kSampleRate);
    quiet.PushSilence(static_cast<std::size_t>(5.0 * kSampleRate));
    ok &= Report("Nothing but silence reads nothing",
                 !quiet.MomentaryLufs() && !quiet.ShortTermLufs() && !quiet.IntegratedLufs());
    return ok;
}

// ── The tap ──────────────────────────────────────────────────────────────────

/// Pushes `seconds` of `source` into `tap` in `blockSize` blocks, reading every `readEvery`
/// blocks with the clock moved on by the audio pushed since the last read.
LoudnessTap::Reading PushAndRead(LoudnessTap& tap, SineSource& source, int blockSize, double seconds, int readEvery,
                                 Clock::time_point& now, bool stereo = false)
{
    std::vector<float> block(static_cast<std::size_t>(blockSize));
    std::vector<float> half(static_cast<std::size_t>(blockSize));
    const int blocks = static_cast<int>(std::llround(seconds * kSampleRate / blockSize));
    LoudnessTap::Reading reading;

    for (int i = 0; i < blocks; ++i)
    {
        source.Fill(block.data(), block.size());
        std::transform(block.begin(), block.end(), half.begin(), [](float sample) { return 0.5f * sample; });
        tap.Push(block.data(), stereo ? half.data() : nullptr, blockSize);

        if ((i + 1) % readEvery == 0 || i + 1 == blocks)
        {
            now += std::chrono::microseconds(static_cast<long long>(1e6 * readEvery * blockSize / kSampleRate));
            reading = tap.Analyze(kSampleRate, now);
        }
    }

    return reading;
}

bool TestTapGivesTheMeterWhatWasPushed()
{
    std::cout << "\nThe tap gives the meter exactly what was pushed\n";
    bool ok = true;

    for (const bool stereo : {false, true})
    {
        SineSource forTap(997.0, kMinus20Dbfs);
        SineSource forMeter(997.0, kMinus20Dbfs);
        LoudnessTap tap;
        auto now = Clock::now();
        // 64-sample blocks read every 75 blocks (100 ms): the app's own block size and feed rate.
        const auto reading = PushAndRead(tap, forTap, 64, 4.0, 75, now, stereo);

        const auto direct = forMeter.Take(4.0);
        std::vector<float> half(direct.size());
        std::transform(direct.begin(), direct.end(), half.begin(), [](float sample) { return 0.5f * sample; });
        loudness::StreamingLoudnessMeter meter;
        meter.Prepare(kSampleRate);
        meter.Push(direct.data(), stereo ? half.data() : direct.data(), direct.size());

        const std::string kind = stereo ? "Stereo" : "Mono";
        const auto same = [](const std::optional<double>& a, const std::optional<double>& b) {
            return a.has_value() == b.has_value() && (!a || std::abs(*a - *b) < 1e-9);
        };
        ok &= Report(kind + ": momentary matches", same(reading.momentaryLufs, meter.MomentaryLufs()),
                     Lufs(reading.momentaryLufs) + " vs " + Lufs(meter.MomentaryLufs()));
        ok &= Report(kind + ": short-term matches", same(reading.shortTermLufs, meter.ShortTermLufs()),
                     Lufs(reading.shortTermLufs));
        ok &= Report(kind + ": integrated matches", same(reading.integratedLufs, meter.IntegratedLufs()),
                     Lufs(reading.integratedLufs));
        ok &= Report(kind + ": measured time is the audio pushed", std::abs(reading.measuredSeconds - 4.0) < 1e-9,
                     std::to_string(reading.measuredSeconds) + " s");
    }

    return ok;
}

bool TestTapBacklogAndRestart()
{
    std::cout << "\nA reader that falls behind, and restarts\n";
    bool ok = true;

    SineSource sine(997.0, kMinus20Dbfs);
    LoudnessTap tap;
    auto now = Clock::now();

    // Twice the ring with no read: the reader keeps the newest half and measures that.
    std::vector<float> block(512);

    for (std::size_t pushed = 0; pushed < 2 * LoudnessTap::kRingFrames; pushed += block.size())
    {
        sine.Fill(block.data(), block.size());
        tap.Push(block.data(), nullptr, static_cast<int>(block.size()));
    }

    const auto behind = tap.Analyze(kSampleRate, now);
    const double halfRingSeconds = static_cast<double>(LoudnessTap::kRingFrames / 2) / kSampleRate;
    ok &= Report("Only the newest half of the ring is measured",
                 std::abs(behind.measuredSeconds - halfRingSeconds) < 1e-9,
                 std::to_string(behind.measuredSeconds) + " s");
    ok &= Report("...and what it measures is right", Near(behind.integratedLufs, -20.0, 0.05),
                 Lufs(behind.integratedLufs));

    tap.Restart();
    const auto restarted = tap.Analyze(kSampleRate, now);
    ok &= Report("A restart forgets everything", !restarted.momentaryLufs && !restarted.shortTermLufs &&
                                                     !restarted.integratedLufs && restarted.measuredSeconds == 0.0);

    std::vector<float> huge(LoudnessTap::kRingFrames + 1000, static_cast<float>(kMinus20Dbfs));
    tap.Push(huge.data(), nullptr, static_cast<int>(huge.size()));
    tap.Push(nullptr, nullptr, 64);
    tap.Push(huge.data(), nullptr, 0);
    const auto afterHuge = tap.Analyze(kSampleRate, now);
    ok &= Report("A block longer than the ring is taken safely", afterHuge.measuredSeconds <= halfRingSeconds + 1e-9,
                 std::to_string(afterHuge.measuredSeconds) + " s");

    // A new rate means a new filter; what was measured at the old one is dropped.
    PushAndRead(tap, sine, 64, 1.0, 75, now);
    const auto newRate = tap.Analyze(44100.0, now);
    ok &= Report("A rate change starts the measurement again", newRate.measuredSeconds == 0.0,
                 std::to_string(newRate.measuredSeconds) + " s");
    return ok;
}

bool TestTapCountsAPauseAsSilence()
{
    std::cout << "\nOnce samples stop, the time counts as silence\n";
    bool ok = true;

    SineSource sine(997.0, kMinus20Dbfs);
    LoudnessTap tap;
    auto now = Clock::now();
    const auto playing = PushAndRead(tap, sine, 64, 4.0, 75, now);

    // A pause shorter than a big host block is not silence.
    now += std::chrono::milliseconds(200);
    const auto shortGap = tap.Analyze(kSampleRate, now);
    ok &= Report("A short gap holds the readings",
                 shortGap.momentaryLufs == playing.momentaryLufs && shortGap.measuredSeconds == playing.measuredSeconds,
                 Lufs(shortGap.momentaryLufs));

    for (int i = 0; i < 10; ++i)
    {
        now += std::chrono::milliseconds(100);
        tap.Analyze(kSampleRate, now);
    }

    const auto paused = tap.Analyze(kSampleRate, now);
    ok &= Report("A second without samples empties momentary", !paused.momentaryLufs.has_value(),
                 Lufs(paused.momentaryLufs));
    ok &= Report("...counts the time as measured",
                 std::abs(paused.measuredSeconds - (playing.measuredSeconds + 1.2)) < 0.01,
                 std::to_string(paused.measuredSeconds) + " s");

    // Integrated reads the stop as the offline meter reads the tone followed by silence.
    SineSource again(997.0, kMinus20Dbfs);
    auto toneThenSilence = again.Take(4.0);
    toneThenSilence.resize(toneThenSilence.size() + static_cast<std::size_t>(1.2 * kSampleRate), 0.0f);
    const auto offline =
        loudness::MeasureIntegratedLoudness(toneThenSilence.data(), nullptr, toneThenSilence.size(), kSampleRate);
    ok &= Report("...and integrated reads it as silence",
                 offline.integratedLufs && Near(paused.integratedLufs, *offline.integratedLufs, 1e-6),
                 Lufs(paused.integratedLufs) + " vs " + Lufs(offline.integratedLufs));

    // A long stall feeds no more silence than changes anything.
    now += std::chrono::seconds(60);
    const auto stalled = tap.Analyze(kSampleRate, now);
    ok &= Report("A long stall is capped", stalled.measuredSeconds - paused.measuredSeconds < 3.6,
                 std::to_string(stalled.measuredSeconds - paused.measuredSeconds) + " s fed");
    ok &= Report("...empties short-term", !stalled.shortTermLufs.has_value(), Lufs(stalled.shortTermLufs));
    ok &= Report("...and leaves integrated alone", stalled.integratedLufs == paused.integratedLufs,
                 Lufs(stalled.integratedLufs));

    const auto back = PushAndRead(tap, sine, 64, 0.5, 75, now);
    ok &= Report("A returning tone reads again", Near(back.momentaryLufs, -20.0, 0.05), Lufs(back.momentaryLufs));
    return ok;
}

// ── The executor and the mixer ───────────────────────────────────────────────

/// in -> gain (-6 dB) -> out.
SignalGraph MakeGainGraph()
{
    SignalGraph graph;
    graph.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back({"gain", "gain", "utility", "Gain", true});
    graph.nodes.back().params["gainDb"] = -6.0;
    graph.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "gain", 0, 0, 1.0});
    graph.edges.push_back({"gain", "out", 0, 0, 1.0});
    return graph;
}

struct ExecutorRun
{
    static constexpr int kBlock = 64;
    std::vector<float> inL = std::vector<float>(kBlock);
    std::vector<float> inR = std::vector<float>(kBlock, 0.0f);
    std::vector<float> outL = std::vector<float>(kBlock);
    std::vector<float> outR = std::vector<float>(kBlock);
    SineSource sine{997.0, kMinus20Dbfs};
    Clock::time_point now = Clock::now();

    /// Runs `seconds` of audio, reading every 100 ms as the telemetry feed does, with the clock
    /// moved on by the audio run. Returns the last read.
    bool Run(SignalGraphExecutor& executor, double seconds, LoudnessTap::Reading& reading)
    {
        constexpr int kBlocksPerRead = 75;
        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        const int blocks = static_cast<int>(std::llround(seconds * kSampleRate / kBlock));
        bool read = false;

        for (int i = 0; i < blocks; ++i)
        {
            sine.Fill(inL.data(), inL.size());
            executor.Process(inputs, outputs, kBlock);

            if ((i + 1) % kBlocksPerRead == 0 || i + 1 == blocks)
            {
                now += std::chrono::microseconds(
                    static_cast<long long>(1e6 * (((i % kBlocksPerRead) + 1) * kBlock) / kSampleRate));
                read = executor.ReadWatchedLoudness(reading, now);
            }
        }

        return read;
    }
};

bool TestExecutorTapsTheNodeOutput()
{
    std::cout << "\nThe executor measures the watched node's output\n";
    bool ok = true;

    SignalGraphExecutor executor;
    executor.SetInputLayout(ChannelLayout::Mono); // a guitar on one input
    executor.SetGraph(MakeGainGraph());
    executor.Prepare(kSampleRate, ExecutorRun::kBlock);
    ExecutorRun run;
    LoudnessTap::Reading reading;

    ok &= Report("Nothing is read before a watch", !executor.ReadWatchedLoudness(reading));
    ok &= Report("An unknown node cannot be watched", !executor.WatchNodeLoudness("no_such_node"));
    ok &= Report("The gain node can be watched", executor.WatchNodeLoudness("gain"));

    run.Run(executor, 3.5, reading);
    ok &= Report("The gain's loudness is its output, 6 dB down", Near(reading.momentaryLufs, -26.0, 0.1),
                 Lufs(reading.momentaryLufs));
    ok &= Report("Short-term and integrated agree",
                 Near(reading.shortTermLufs, -26.0, 0.1) && Near(reading.integratedLufs, -26.0, 0.1),
                 Lufs(reading.shortTermLufs) + ", " + Lufs(reading.integratedLufs));

    // The input node fills its buffers outside the per-node pass, and is tapped there.
    ok &= Report("Watching moves to the input node", executor.WatchNodeLoudness("in"));
    run.Run(executor, 1.0, reading);
    ok &= Report("Moving the watch restarts the measurement", std::abs(reading.measuredSeconds - 1.0) < 0.01,
                 std::to_string(reading.measuredSeconds) + " s");
    ok &= Report("The input reads the tone as it arrives", Near(reading.momentaryLufs, -20.0, 0.1),
                 Lufs(reading.momentaryLufs));

    // A bypassed node passes its input on, and that is what it puts out.
    executor.WatchNodeLoudness("gain");
    executor.SetNodeEnabled("gain", false);
    run.Run(executor, 1.0, reading);
    ok &=
        Report("A bypassed node reads its input", Near(reading.momentaryLufs, -20.0, 0.1), Lufs(reading.momentaryLufs));
    executor.SetNodeEnabled("gain", true);

    // A graph edit rebuilds every node state; the watch carries across it, and so does the
    // measurement, since it is the same node.
    const double before = reading.measuredSeconds;
    executor.SetGraph(MakeGainGraph());
    executor.Prepare(kSampleRate, ExecutorRun::kBlock);
    run.Run(executor, 1.0, reading);
    ok &= Report("A rebuilt graph keeps measuring the watched node",
                 std::abs(reading.measuredSeconds - (before + 1.0)) < 0.01,
                 std::to_string(reading.measuredSeconds) + " s");
    ok &= Report("...and reads it after the rebuild", Near(reading.momentaryLufs, -26.0, 0.1),
                 Lufs(reading.momentaryLufs));

    executor.ClearLoudnessWatch();
    ok &= Report("Clearing the watch stops reads", !executor.ReadWatchedLoudness(reading));

    executor.WatchNodeLoudness("gain");
    SignalGraph withoutGain;
    withoutGain.nodes.push_back({"in", kNodeTypeInput, "", "Input", true});
    withoutGain.nodes.push_back({"out", kNodeTypeOutput, "", "Output", true});
    withoutGain.edges.push_back({"in", "out", 0, 0, 1.0});
    executor.SetGraph(withoutGain);
    ok &= Report("A node that is gone cannot be watched", !executor.WatchNodeLoudness("gain"));
    return ok;
}

bool TestTapAddsNothingToTheAudioPath()
{
    std::cout << "\nThe tap adds no allocation to the audio path\n";
    bool ok = true;

    SignalGraphExecutor executor;
    executor.SetInputLayout(ChannelLayout::Mono);
    executor.SetGraph(MakeGainGraph());
    executor.Prepare(kSampleRate, ExecutorRun::kBlock);
    ExecutorRun run;
    float* inputs[2] = {run.inL.data(), run.inR.data()};
    float* outputs[2] = {run.outL.data(), run.outR.data()};

    const auto countAllocations = [&]() {
        // A second of 64-sample blocks.
        const std::size_t before = gAllocationCount.load(std::memory_order_relaxed);

        for (int i = 0; i < 750; ++i)
        {
            run.sine.Fill(run.inL.data(), run.inL.size());
            executor.Process(inputs, outputs, ExecutorRun::kBlock);
        }

        return gAllocationCount.load(std::memory_order_relaxed) - before;
    };

    countAllocations(); // anything a first block sets up
    const std::size_t unwatched = countAllocations();
    executor.WatchNodeLoudness("gain");
    const std::size_t watched = countAllocations();
    ok &= Report("A second of blocks allocates nothing, unwatched", unwatched == 0, std::to_string(unwatched));
    ok &= Report("...nor while a node is watched", watched == 0, std::to_string(watched));

    // Informational: what a read costs the message thread, a second of audio at a time.
    LoudnessTap::Reading reading;
    const auto start = Clock::now();
    executor.ReadWatchedLoudness(reading, run.now += std::chrono::seconds(1));
    const double readUs = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    std::cout << "  (reading one second of audio took " << std::fixed << std::setprecision(0) << readUs
              << " us on the message thread)\n";
    return ok;
}

bool TestMixerFindsTheNodeByScope()
{
    std::cout << "\nThe mixer finds the node by scope\n";
    bool ok = true;

    MultiPresetMixer mixer;
    mixer.Prepare(kSampleRate, ExecutorRun::kBlock);

    Preset preset;
    preset.id = "presetA";
    preset.name = "presetA";
    preset.graph = MakeGainGraph();
    mixer.AddActivePreset(preset, "presetA", "presetA");

    LoudnessTap::Reading reading;
    ok &= Report("A preset node is found by preset id", mixer.ReadNodeLoudness("preset", "presetA", "gain", reading));
    ok &= Report("An empty preset id means the live preset", mixer.ReadNodeLoudness("preset", "", "gain", reading));
    ok &= Report("Another preset's id finds nothing", !mixer.ReadNodeLoudness("preset", "presetB", "gain", reading));
    ok &= Report("An unknown scope finds nothing", !mixer.ReadNodeLoudness("sideways", "", "gain", reading));
    ok &= Report("The global chain's input is found", mixer.ReadNodeLoudness("pre", "", "__input__", reading));
    ok &= Report("The global chain's output is found", mixer.ReadNodeLoudness("post", "", "__output__", reading));

    ExecutorRun run;
    float* inputs[2] = {run.inL.data(), run.inR.data()};
    float* outputs[2] = {run.outL.data(), run.outR.data()};
    mixer.ClearLoudnessTaps();
    mixer.ReadNodeLoudness("preset", "presetA", "gain", reading);

    for (int i = 0; i < static_cast<int>(1.0 * kSampleRate / ExecutorRun::kBlock); ++i)
    {
        run.sine.Fill(run.inL.data(), run.inL.size());
        mixer.Process(inputs, outputs, ExecutorRun::kBlock);
    }

    mixer.ReadNodeLoudness("preset", "presetA", "gain", reading);
    ok &= Report("The preset's gain node reads the tone",
                 reading.momentaryLufs.has_value() && *reading.momentaryLufs < -15.0 && *reading.momentaryLufs > -40.0,
                 Lufs(reading.momentaryLufs));
    return ok;
}

/// A mixer with the -6 dB gain preset, and a sine to run through it.
struct MixerRun
{
    MultiPresetMixer mixer;
    ExecutorRun run;

    MixerRun()
    {
        mixer.Prepare(kSampleRate, ExecutorRun::kBlock);
        Preset preset;
        preset.id = "presetA";
        preset.name = "presetA";
        preset.graph = MakeGainGraph();
        mixer.AddActivePreset(preset, "presetA", "presetA");
    }

    void Process(double seconds)
    {
        float* inputs[2] = {run.inL.data(), run.inR.data()};
        float* outputs[2] = {run.outL.data(), run.outR.data()};

        for (int i = 0; i < static_cast<int>(seconds * kSampleRate / ExecutorRun::kBlock); ++i)
        {
            run.sine.Fill(run.inL.data(), run.inL.size());
            mixer.Process(inputs, outputs, ExecutorRun::kBlock);
        }
    }
};

bool TestMixerMeasuresItsOutput()
{
    std::cout << "\nThe mixer measures the app's output\n";
    bool ok = true;

    MixerRun m;
    LoudnessTap::Reading reading;
    ok &=
        Report("The first read taps the output", m.mixer.ReadOutputLoudness(reading) && reading.measuredSeconds == 0.0);

    m.Process(1.0);
    m.mixer.ReadOutputLoudness(reading);
    ok &= Report("A second of audio is measured", std::abs(reading.measuredSeconds - 1.0) < 0.01,
                 std::to_string(reading.measuredSeconds) + " s");
    ok &= Report("The output reads the tone",
                 reading.momentaryLufs.has_value() && *reading.momentaryLufs < -15.0 && *reading.momentaryLufs > -40.0,
                 Lufs(reading.momentaryLufs));

    // Measured after the master volume, as the output meter is: a muted output is silent.
    m.mixer.SetOutputMuted(true);
    m.Process(0.5);
    m.mixer.ReadOutputLoudness(reading);
    ok &= Report("A muted output reads as silence", !reading.momentaryLufs.has_value(), Lufs(reading.momentaryLufs));
    m.mixer.SetOutputMuted(false);

    // Let go and taken again, it starts over.
    m.mixer.ClearOutputLoudnessTap();
    m.Process(0.5);
    m.mixer.ReadOutputLoudness(reading);
    ok &= Report("Unwatched audio is not measured", reading.measuredSeconds == 0.0,
                 std::to_string(reading.measuredSeconds) + " s");
    m.Process(0.5);
    m.mixer.ReadOutputLoudness(reading);
    ok &= Report("A new watch measures from its start", std::abs(reading.measuredSeconds - 0.5) < 0.01,
                 std::to_string(reading.measuredSeconds) + " s");
    return ok;
}

bool TestMixerKeepsThePeakLoad()
{
    std::cout << "\nThe mixer keeps the worst block's DSP load\n";
    bool ok = true;

    MixerRun m;
    m.mixer.SetSignalDiagnosticsEnabled(false);
    m.Process(0.2);
    ok &= Report("Nothing is kept with diagnostics off", m.mixer.TakePeakDspLoadPercent() == 0.0);

    m.mixer.SetSignalDiagnosticsEnabled(true);
    m.Process(0.2);
    const auto stats = m.mixer.GetPerformanceStats();
    const double peak = m.mixer.TakePeakDspLoadPercent();
    std::ostringstream detail;
    detail << std::fixed << std::setprecision(2) << "peak " << peak << "%, last block " << stats.dspLoadPercent << "%";
    ok &= Report("A peak is kept", peak > 0.0, detail.str());
    ok &= Report("It is no lower than the last block's load", peak + 1e-9 >= stats.dspLoadPercent, detail.str());
    ok &= Report("Taking it starts a new span", m.mixer.TakePeakDspLoadPercent() == 0.0);
    return ok;
}
} // namespace

int main()
{
    RegisterAllEffects();

    bool allPassed = true;

    for (const auto& test :
         {TestMeterReadsTheReferenceTone, TestMeterWindowsFillBeforeReading, TestMeterIntegratedMatchesTheOfflineMeter,
          TestMeterSilence, TestTapGivesTheMeterWhatWasPushed, TestTapBacklogAndRestart, TestTapCountsAPauseAsSilence,
          TestExecutorTapsTheNodeOutput, TestTapAddsNothingToTheAudioPath, TestMixerFindsTheNodeByScope,
          TestMixerMeasuresItsOutput, TestMixerKeepsThePeakLoad})
    {
        if (!test())
        {
            allPassed = false;
        }
    }

    std::cout << "\n" << (allPassed ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    return allPassed ? 0 : 1;
}
