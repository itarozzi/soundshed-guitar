/**
 * @file SpliceTransposerTests.cpp
 * @brief The Low Latency transpose engine (SpliceTransposer) and its pick-attack detector.
 *
 *   - the latency it reports is the tap's mean delay, (floor + window) / 2, at every window and rate
 *   - one and two octaves either way, and fractional shifts, land on pitch within a few cents
 *   - a shifted tone keeps its level, and so does shifted noise, whose splices never match
 *   - a pick attack after a sustained note arrives a few milliseconds late shifting down, and
 *     within the upshift's latency cap shifting up, wherever the tap had drifted
 *   - stereo channels stay exactly proportional through every splice
 *   - a pedal sweep through +12 and -12 leaves no hole and no step
 *   - NaN input, extreme shifts and window changes mid-stream stay finite
 *   - the detector fires on every pick of a riff and not on a sustained chord's beating
 *   - none of it allocates on the audio thread
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "dsp/PickAttackDetector.h"
#include "dsp/SpliceTransposer.h"
#include "helpers/AudioThreadAllocations.h"
#include "helpers/GuitarPhraseSynth.h"

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSampleRate = 48000.0;

using guitarfx::PickAttackDetector;
using guitarfx::SpliceTransposer;

int gFailures = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    gFailures += condition ? 0 : 1;
    std::cout << (condition ? "  [PASS] " : "  [FAIL] ") << what;

    if (!detail.empty())
    {
        std::cout << "  (" << detail << ")";
    }

    std::cout << std::endl;
}

std::string Num(double v, int precision = 2)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", precision, v);
    return std::string(buffer);
}

/// Six harmonics falling as 1/k: a real waveform for the splices to match, with no inharmonicity.
std::vector<float> Harmonic(double frequency, double seconds, double sampleRate = kSampleRate, double amplitude = 0.3)
{
    std::vector<float> out(static_cast<size_t>(seconds * sampleRate));

    for (size_t i = 0; i < out.size(); ++i)
    {
        double v = 0.0;

        for (int k = 1; k <= 6; ++k)
        {
            v += std::sin(2.0 * kPi * frequency * k * static_cast<double>(i) / sampleRate) / k;
        }

        out[i] = static_cast<float>(amplitude * v / 2.45);
    }

    return out;
}

std::vector<float> Noise(double seconds, double amplitude, unsigned seed = 7)
{
    std::vector<float> out(static_cast<size_t>(seconds * kSampleRate));
    unsigned state = seed;

    for (auto& v : out)
    {
        state = state * 1664525u + 1013904223u;
        v = static_cast<float>(amplitude * (static_cast<double>(state >> 8) / 8388608.0 - 1.0));
    }

    return out;
}

/// Runs mono `input` through an engine at `semitones`, engaged from the first sample.
std::vector<float> Shift(const std::vector<float>& input, double semitones, double sampleRate = kSampleRate,
                         double windowSeconds = SpliceTransposer::kDefaultWindowSeconds)
{
    SpliceTransposer engine;
    engine.Prepare(sampleRate);
    engine.SetWindowSeconds(windowSeconds);
    engine.SetSemitones(semitones, false);
    engine.Reset();
    engine.Engage();
    std::vector<float> out(input.size());

    for (size_t i = 0; i < input.size(); ++i)
    {
        float right = 0.0f;
        engine.Write(input[i], input[i]);
        engine.Process(out[i], right);
    }

    return out;
}

/// f0 of the frame starting at `start`, by normalised autocorrelation within 12% of `expected`.
double FrameF0(const std::vector<float>& x, size_t start, size_t length, double sampleRate, double expected)
{
    const double period = sampleRate / expected;
    const int low = std::max(2, static_cast<int>(period / 1.12));
    const int high = static_cast<int>(period * 1.12) + 2;

    if (start + length + static_cast<size_t>(high) + 2 >= x.size())
    {
        return 0.0;
    }

    const auto ncc = [&](int lag) {
        double xy = 0.0;
        double xx = 0.0;
        double yy = 0.0;

        for (size_t i = 0; i < length; ++i)
        {
            const double a = x[start + i];
            const double b = x[start + i + static_cast<size_t>(lag)];
            xy += a * b;
            xx += a * a;
            yy += b * b;
        }

        return xy / (std::sqrt(xx * yy) + 1.0e-20);
    };

    int bestLag = low;
    double best = -2.0;

    for (int lag = low; lag <= high; ++lag)
    {
        const double c = ncc(lag);

        if (c > best)
        {
            best = c;
            bestLag = lag;
        }
    }

    const double before = ncc(bestLag - 1);
    const double after = ncc(bestLag + 1);
    const double curvature = before - 2.0 * best + after;
    const double offset = std::abs(curvature) > 1.0e-12 ? 0.5 * (before - after) / curvature : 0.0;
    return sampleRate / (bestLag + std::clamp(offset, -1.0, 1.0));
}

/// Worst cents error of the frames over [from, to] seconds against `expected` Hz.
double WorstCents(const std::vector<float>& x, double expected, double from, double to, double sampleRate = kSampleRate)
{
    const auto length = static_cast<size_t>(std::max(0.04, 4.5 / expected) * sampleRate);
    double worst = 0.0;

    for (double t = from; t < to; t += 0.05)
    {
        const double f = FrameF0(x, static_cast<size_t>(t * sampleRate), length, sampleRate, expected);
        worst = std::max(worst, f > 0.0 ? std::abs(1200.0 * std::log2(f / expected)) : 1200.0);
    }

    return worst;
}

double Rms(const std::vector<float>& x, size_t from, size_t to)
{
    double sum = 0.0;

    for (size_t i = from; i < to && i < x.size(); ++i)
    {
        sum += static_cast<double>(x[i]) * x[i];
    }

    return std::sqrt(sum / std::max<size_t>(1, to - from));
}

void TestLatency()
{
    std::cout << "\n--- Reported latency ---\n";
    bool ok = true;
    std::string detail;

    for (const double rate : {44100.0, 48000.0, 96000.0})
    {
        for (const double window : {0.02, 0.03, 0.04, 0.06})
        {
            SpliceTransposer engine;
            engine.Prepare(rate);
            engine.SetWindowSeconds(window);
            const double expected =
                0.5 * (std::round(SpliceTransposer::kFloorSeconds * rate) + std::round(window * rate));

            if (engine.NominalLatencySamples() != static_cast<int>(std::lround(expected)))
            {
                ok = false;
                detail = Num(rate, 0) + " Hz, " + Num(window * 1000.0, 0) +
                         " ms: " + std::to_string(engine.NominalLatencySamples());
            }
        }
    }

    Check(ok, "latency is the tap's mean delay at every window and rate", detail);

    SpliceTransposer engine;
    engine.Prepare(kSampleRate);
    Check(engine.NominalLatencySamples() == 768, "the default window reports 16 ms at 48 kHz",
          std::to_string(engine.NominalLatencySamples()));
}

void TestPitch()
{
    std::cout << "\n--- Shifts land on pitch ---\n";

    // A 220 Hz tone, so two octaves down (55 Hz) still has a measurable period.
    for (const double semitones : {-24.0, -12.0, -7.0, -2.5, 2.0, 7.0, 12.0, 24.0})
    {
        const auto input = Harmonic(220.0, 1.6);
        const auto output = Shift(input, semitones);
        const double expected = 220.0 * std::exp2(semitones / 12.0);
        const double worst = WorstCents(output, expected, 0.4, 1.4);
        Check(worst < 3.0, Num(semitones, 1) + " st lands on " + Num(expected, 1) + " Hz", Num(worst) + " cents worst");
    }

    for (const double rate : {44100.0, 96000.0})
    {
        for (const double semitones : {-12.0, 12.0})
        {
            const auto input = Harmonic(110.0, 1.6, rate);
            const auto output = Shift(input, semitones, rate);
            const double expected = 110.0 * std::exp2(semitones / 12.0);
            const double worst = WorstCents(output, expected, 0.4, 1.4, rate);
            Check(worst < 3.0, Num(semitones, 0) + " st at " + Num(rate, 0) + " Hz", Num(worst) + " cents worst");
        }
    }

    // A low E1 needs the default window: its 24 ms period has to fit the longest jump.
    const auto low = Harmonic(41.2034, 2.0);
    const double lowWorst = WorstCents(Shift(low, -5.0), 41.2034 * std::exp2(-5.0 / 12.0), 0.5, 1.8);
    Check(lowWorst < 3.0, "a low E1 shifted down a fourth stays on pitch", Num(lowWorst) + " cents worst");
}

void TestLevel()
{
    std::cout << "\n--- Level holds through splices ---\n";

    for (const double semitones : {-12.0, -5.0, 7.0, 12.0})
    {
        const auto input = Harmonic(146.83, 1.5);
        const auto output = Shift(input, semitones);
        const double ratioDb = 20.0 * std::log10(Rms(output, 24000, 72000) / Rms(input, 24000, 72000));
        Check(std::abs(ratioDb) < 0.5, "a tone keeps its level at " + Num(semitones, 0) + " st", Num(ratioDb) + " dB");
    }

    // Noise never matches, so every splice uses its longest fade and the power-normalised law.
    for (const double semitones : {-5.0, 5.0})
    {
        const auto input = Noise(2.5, 0.3);
        const auto output = Shift(input, semitones);
        const double overall = Rms(output, 24000, 120000);
        double low = 1.0e9;
        double high = 0.0;

        for (size_t start = 24000; start + 2400 <= 120000; start += 2400)
        {
            const double block = Rms(output, start, start + 2400);
            low = std::min(low, block);
            high = std::max(high, block);
        }

        const double dipDb = 20.0 * std::log10(low / overall);
        const double peakDb = 20.0 * std::log10(high / overall);
        Check(dipDb > -1.0 && peakDb < 1.0, "shifted noise holds its level at " + Num(semitones, 0) + " st",
              Num(dipDb) + " / +" + Num(peakDb) + " dB over 50 ms blocks");
    }
}

/// A sustained note long enough for the tap to drift, then a picked one. Returns how far behind
/// the input the tap is just after the pick, and when the pick arrives in the output (its energy
/// over 0.5 ms first triples the level before it), both in ms.
struct AttackTiming
{
    double tapMs = 1000.0;
    double arrivalMs = 1000.0;
};

AttackTiming TimeAttack(double semitones)
{
    std::vector<guitarfx::test::PhraseNote> notes(2);
    notes[0].start = 0.05;
    notes[0].length = 0.95;
    notes[0].hz = 110.0;
    notes[0].peak = 0.3;
    notes[1].start = 1.0;
    notes[1].length = 0.5;
    notes[1].hz = 146.83;
    notes[1].peak = 0.3;
    const auto input = guitarfx::test::RenderPhrase(notes, 1.4, kSampleRate, 3);
    const auto pick = static_cast<size_t>(1.0 * kSampleRate);

    SpliceTransposer engine;
    engine.Prepare(kSampleRate);
    engine.SetSemitones(semitones, false);
    engine.Reset();
    engine.Engage();
    std::vector<float> output(input.size());
    AttackTiming timing;

    for (size_t i = 0; i < input.size(); ++i)
    {
        float right = 0.0f;
        engine.Write(input[i], input[i]);
        engine.Process(output[i], right);

        if (i == pick + 48)
        {
            timing.tapMs = engine.CurrentDelaySamples() * 1000.0 / kSampleRate;
        }
    }

    const size_t window = 24;
    const double before = Rms(output, pick - 480, pick);

    for (size_t i = pick; i < pick + 2400; ++i)
    {
        if (Rms(output, i, i + window) > 3.0 * before)
        {
            timing.arrivalMs = static_cast<double>(i - pick) * 1000.0 / kSampleRate;
            break;
        }
    }

    return timing;
}

void TestAttacks()
{
    std::cout << "\n--- Attacks arrive on time ---\n";
    const double floorMs = SpliceTransposer::kFloorSeconds * 1000.0;
    const double spanMs = SpliceTransposer::kAttackSpanSeconds * 1000.0;

    for (const double semitones : {-2.0, -5.0, -12.0})
    {
        const auto timing = TimeAttack(semitones);
        // 1 ms after the pick the tap has drifted a little past its landing: (1 - r) ms at most.
        Check(timing.tapMs <= floorMs + spanMs + 1.0,
              "a pick shifted " + Num(semitones, 0) + " st takes the tap to the front",
              Num(timing.tapMs) + " ms behind the input");
        // Measured by energy, the arrival adds the 2 ms crossfade and a rise stretched by the shift.
        Check(timing.arrivalMs <= 10.0, "... and arrives within 10 ms", Num(timing.arrivalMs) + " ms");
    }

    for (const double semitones : {5.0, 12.0})
    {
        const auto timing = TimeAttack(semitones);
        const double cap = (SpliceTransposer::kAttackLatencyCapSeconds + SpliceTransposer::kAttackSpanSeconds) * 1000.0;
        Check(timing.arrivalMs <= cap + 1.0,
              "a pick shifted +" + Num(semitones, 0) + " st arrives within the upshift cap",
              Num(timing.arrivalMs) + " ms");
    }
}

void TestStereo()
{
    std::cout << "\n--- Stereo stays proportional ---\n";
    const auto input = guitarfx::test::RenderPhrase(
        {{0.05, 0.4, 82.41, 0.3}, {0.5, 0.4, 110.0, 0.3}, {1.0, 0.5, 146.83, 0.3}}, 1.6, kSampleRate, 5);
    SpliceTransposer engine;
    engine.Prepare(kSampleRate);
    engine.SetSemitones(-5.0, false);
    engine.Reset();
    engine.Engage();
    double worst = 0.0;

    for (const float x : input)
    {
        float left = 0.0f;
        float right = 0.0f;
        engine.Write(x, 0.5f * x);
        engine.Process(left, right);
        worst = std::max(worst, static_cast<double>(std::abs(right - 0.5f * left)));
    }

    Check(worst < 1.0e-6 && engine.SpliceCount() > 0, "a channel at half level stays half through every splice",
          std::to_string(engine.SpliceCount()) + " splices, worst " + Num(worst, 9));
}

void TestSweep()
{
    std::cout << "\n--- A pedal sweep ---\n";
    const double seconds = 4.0;
    const auto input = Harmonic(220.0, seconds);
    SpliceTransposer engine;
    engine.Prepare(kSampleRate);
    engine.Reset();
    engine.Engage();
    std::vector<float> output(input.size());

    for (size_t i = 0; i < input.size(); ++i)
    {
        // 0 -> +12 -> -12 -> 0, a block of 64 at a time as a host's automation arrives.
        if (i % 64 == 0)
        {
            const double t = static_cast<double>(i) / kSampleRate;
            const double semitones =
                t < 1.0 ? 12.0 * t : (t < 3.0 ? 12.0 - 12.0 * (t - 1.0) : -12.0 + 12.0 * (t - 3.0));
            engine.SetSemitones(semitones, true);
        }

        float right = 0.0f;
        engine.Write(input[i], input[i]);
        engine.Process(output[i], right);
    }

    double inputStep = 0.0;

    for (size_t i = 1; i < input.size(); ++i)
    {
        inputStep = std::max(inputStep, static_cast<double>(std::abs(input[i] - input[i - 1])));
    }

    double worstStep = 0.0;
    double quietest = 1.0e9;
    const double level = Rms(input, 0, input.size());

    for (size_t i = 4800; i < output.size(); ++i)
    {
        worstStep = std::max(worstStep, static_cast<double>(std::abs(output[i] - output[i - 1])));
    }

    for (size_t start = 4800; start + 480 <= output.size(); start += 480)
    {
        quietest = std::min(quietest, Rms(output, start, start + 480));
    }

    // At +12 the tone itself moves twice as fast; a splice or a step would jump by its swing.
    Check(worstStep < 2.5 * inputStep, "no step anywhere in the sweep",
          Num(worstStep, 4) + " against the tone's own " + Num(inputStep, 4));
    Check(quietest > 0.5 * level, "no hole anywhere in the sweep", Num(20.0 * std::log10(quietest / level)) + " dB");
}

void TestHostileInput()
{
    std::cout << "\n--- Hostile input stays finite ---\n";
    const auto tone = Harmonic(110.0, 1.0);
    bool finite = true;

    for (const double semitones : {-36.0, -24.0, 24.0})
    {
        SpliceTransposer engine;
        engine.Prepare(kSampleRate);
        engine.SetSemitones(semitones, false);
        engine.Reset();
        engine.Engage();

        for (size_t i = 0; i < tone.size(); ++i)
        {
            float x = tone[i];

            if (i % 4801 == 100)
            {
                x = std::numeric_limits<float>::quiet_NaN();
            }
            else if (i % 7919 == 50)
            {
                x = std::numeric_limits<float>::infinity();
            }

            if (i == tone.size() / 3)
            {
                engine.SetWindowSeconds(0.06);
            }
            else if (i == tone.size() / 2)
            {
                engine.SetWindowSeconds(0.015);
                engine.SetSemitones(-semitones, false);
            }

            float left = 0.0f;
            float right = 0.0f;
            engine.Write(x, x);
            engine.Process(left, right);
            finite = finite && std::isfinite(left) && std::isfinite(right);
        }
    }

    Check(finite, "NaN and infinite input, +-24 and -36 st and window changes stay finite");
}

void TestDetector()
{
    std::cout << "\n--- Pick attack detection ---\n";
    using guitarfx::test::PhraseNote;
    std::vector<PhraseNote> riff;

    for (int i = 0; i < 24; ++i)
    {
        PhraseNote n;
        n.start = 0.1 + 0.2 * i;
        n.length = (i % 3 == 0) ? 0.18 : 0.1;
        n.hz = (i % 4 == 0) ? 41.2034 : (i % 4 == 1 ? 82.41 : (i % 4 == 2 ? 110.0 : 146.83));
        n.peak = 0.2 + 0.05 * (i % 4);
        riff.push_back(n);
    }

    const auto riffAudio = guitarfx::test::RenderPhrase(riff, 5.0, kSampleRate, 9);
    PickAttackDetector detector;
    detector.Prepare(kSampleRate);
    std::vector<size_t> hits;

    for (size_t i = 0; i < riffAudio.size(); ++i)
    {
        if (detector.Process(riffAudio[i]))
        {
            hits.push_back(i);
        }
    }

    int found = 0;

    for (const auto& note : riff)
    {
        const auto at = static_cast<size_t>(note.start * kSampleRate);
        found += std::any_of(hits.begin(), hits.end(), [&](size_t h) { return h >= at && h < at + 144; }) ? 1 : 0;
    }

    Check(found == static_cast<int>(riff.size()) && hits.size() == riff.size(),
          "every pick of a riff is found within 3 ms, and nothing else",
          std::to_string(found) + " of " + std::to_string(riff.size()) + ", " + std::to_string(hits.size()) +
              " detections");

    // A strummed open E rings for three seconds with its strings beating: one strum, one attack.
    std::vector<float> chord;
    int string = 0;

    for (const double hz : {82.41, 123.47, 164.81, 207.65, 246.94, 329.63})
    {
        PhraseNote n;
        n.start = 0.05 + 0.004 * string++;
        n.length = 3.0;
        n.hz = hz;
        n.peak = 0.15;
        n.muted = false;
        const auto rendered = guitarfx::test::RenderPhrase({n}, 3.0, kSampleRate, static_cast<unsigned>(hz));
        chord.resize(rendered.size(), 0.0f);

        for (size_t i = 0; i < rendered.size(); ++i)
        {
            chord[i] += rendered[i];
        }
    }

    detector.Reset();
    int chordHits = 0;

    for (const float x : chord)
    {
        chordHits += detector.Process(x) ? 1 : 0;
    }

    Check(chordHits == 1, "a sustained chord's beating is not taken for attacks",
          std::to_string(chordHits) + " detections");
}

void TestNoAllocations()
{
    std::cout << "\n--- Audio thread allocations ---\n";
    const auto input = guitarfx::test::RenderPhrase(
        {{0.05, 0.3, 82.41, 0.3}, {0.4, 0.3, 110.0, 0.3}, {0.8, 0.5, 146.83, 0.3}}, 1.5, kSampleRate, 4);
    SpliceTransposer engine;
    engine.Prepare(kSampleRate);
    engine.SetSemitones(-7.0, false);
    engine.Reset();
    engine.Engage();
    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        for (size_t i = 0; i < input.size(); ++i)
        {
            if (i == input.size() / 2)
            {
                engine.SetSemitones(5.0, true);
            }

            float left = 0.0f;
            float right = 0.0f;
            engine.Write(input[i], input[i]);
            engine.Process(left, right);
        }
    });

    Check(allocations.count == 0 && engine.SpliceCount() > 0 && engine.AttackSpliceCount() > 0,
          "splices, attacks and a glide allocate nothing", audio_thread_allocations::Describe(allocations));
}
} // namespace

int main()
{
    std::cout << "=== SpliceTransposer ===\n";
    TestLatency();
    TestPitch();
    TestLevel();
    TestAttacks();
    TestStereo();
    TestSweep();
    TestHostileInput();
    TestDetector();
    TestNoAllocations();
    std::cout << "\n" << (gFailures == 0 ? "All tests passed" : std::to_string(gFailures) + " failure(s)") << std::endl;
    return gFailures == 0 ? 0 : 1;
}
