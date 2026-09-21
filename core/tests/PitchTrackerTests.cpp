/**
 * @file PitchTrackerTests.cpp
 * @brief Tests for the real-time pitch tracker (dsp/PitchTracker.h).
 *
 *   - sines, sawtooths and plucked tones with a strong second harmonic, 46 Hz to 1.48 kHz, read
 *     within a few cents at 22.05 to 192 kHz
 *   - a note change is followed within 50 ms, vibrato is followed continuously, and the last
 *     pitch is held through silence
 *   - white noise never produces a pitch, and a NaN on the input does not stick
 *   - on the DI guitar recording, the decimated tracker agrees with brute-force full-rate YIN
 *     frame by frame
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "dsp/BiquadDesign.h"
#include "dsp/IRWavLoader.h"
#include "dsp/PitchTracker.h"

#ifndef GUITARFX_DEMO_AUDIO_DIR
    #error "GUITARFX_DEMO_AUDIO_DIR must be defined"
#endif

namespace
{
constexpr double kPi = 3.14159265358979323846;

using guitarfx::PitchTracker;

int gFailures = 0;
int gChecks = 0;

void Check(bool condition, const std::string& what, const std::string& detail = "")
{
    ++gChecks;
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

double Cents(double frequency, double reference)
{
    return 1200.0 * std::log2(frequency / reference);
}

enum class Kind
{
    Sine,
    Saw,
    Pluck
};

/// A test tone at `frequency`. The pluck's second harmonic is louder than its fundamental and its
/// upper harmonics decay faster, which is what tempts a tracker an octave high.
std::vector<float> Tone(double frequency, double seconds, double sampleRate, Kind kind, double vibratoCents = 0.0)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));
    double phase = 0.0;

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        const double t = static_cast<double>(n) / sampleRate;
        const double f = frequency * std::exp2(vibratoCents / 1200.0 * std::sin(2.0 * kPi * 5.0 * t));
        double value = 0.0;

        if (kind == Kind::Sine)
        {
            value = std::sin(phase);
        }
        else if (kind == Kind::Saw)
        {
            for (int k = 1; k * frequency < 0.5 * sampleRate; ++k)
            {
                value += 0.5 * std::sin(k * phase) / k;
            }
        }
        else
        {
            constexpr double kAmplitudes[] = {1.0, 1.4, 0.9, 0.6, 0.4, 0.3, 0.2, 0.15};

            for (int k = 1; k <= 8 && k * frequency < 0.5 * sampleRate; ++k)
            {
                value += 0.25 * kAmplitudes[k - 1] * std::exp(-1.5 * k * t) * std::sin(k * phase + 0.3 * k);
            }
        }

        signal[n] = static_cast<float>(0.3 * value);
        phase += 2.0 * kPi * f / sampleRate;
    }

    return signal;
}

void TestAccuracy()
{
    std::cout << "\nAccuracy after 0.4 s" << std::endl;
    constexpr double kFrequencies[] = {46.2, 55.0, 82.41, 110.0, 196.0, 329.63, 659.26, 1000.0, 1318.5, 1480.0};
    double worst = 0.0;
    std::string where;

    for (const double sampleRate : {44100.0, 48000.0, 96000.0})
    {
        for (const Kind kind : {Kind::Sine, Kind::Saw, Kind::Pluck})
        {
            for (const double frequency : kFrequencies)
            {
                PitchTracker tracker;
                tracker.Prepare(sampleRate);
                const auto tone = Tone(frequency, 0.4, sampleRate, kind);
                tracker.Process(tone.data(), static_cast<int>(tone.size()));
                const double error =
                    tracker.FrequencyHz() > 0.0 ? std::abs(Cents(tracker.FrequencyHz(), frequency)) : 1.0e9;

                if (error > worst)
                {
                    worst = error;
                    where = Num(frequency, 1) + " Hz at " + Num(sampleRate, 0);
                }
            }
        }
    }

    Check(worst < 3.0, "every tone from 46 Hz to 1.48 kHz reads within 3 cents",
          "worst " + Num(worst) + " cents, " + where);

    bool otherRates = true;

    for (const double sampleRate : {22050.0, 32000.0, 88200.0, 192000.0})
    {
        PitchTracker tracker;
        tracker.Prepare(sampleRate);
        const auto tone = Tone(110.0, 0.4, sampleRate, Kind::Pluck);
        tracker.Process(tone.data(), static_cast<int>(tone.size()));
        otherRates = otherRates && tracker.FrequencyHz() > 0.0 && std::abs(Cents(tracker.FrequencyHz(), 110.0)) < 3.0;
    }

    Check(otherRates, "and at 22.05, 32, 88.2 and 192 kHz");
}

/// Samples from the start of `second` until the tracker reads it within 20 cents.
int SamplesToFollow(PitchTracker& tracker, const std::vector<float>& second, double frequency)
{
    for (std::size_t n = 0; n < second.size(); ++n)
    {
        tracker.Push(second[n]);

        if (tracker.FrequencyHz() > 0.0 && std::abs(Cents(tracker.FrequencyHz(), frequency)) < 20.0)
        {
            return static_cast<int>(n);
        }
    }

    return -1;
}

void TestResponse()
{
    std::cout << "\nResponse" << std::endl;
    constexpr double kSampleRate = 48000.0;
    constexpr double kChanges[][2] = {
        {110.0, 164.81}, {82.41, 329.63}, {659.26, 82.41}, {392.0, 196.0}, {1318.5, 110.0}};
    int slowest = 0;

    for (const auto& change : kChanges)
    {
        PitchTracker tracker;
        tracker.Prepare(kSampleRate);
        const auto first = Tone(change[0], 0.5, kSampleRate, Kind::Pluck);
        tracker.Process(first.data(), static_cast<int>(first.size()));
        const int samples = SamplesToFollow(tracker, Tone(change[1], 0.3, kSampleRate, Kind::Pluck), change[1]);
        slowest = samples < 0 ? 1 << 30 : std::max(slowest, samples);
    }

    Check(slowest < 0.05 * kSampleRate, "a note change is followed within 50 ms",
          "slowest " + Num(slowest / 48.0, 1) + " ms");

    PitchTracker fresh;
    fresh.Prepare(kSampleRate);
    const int firstNote = SamplesToFollow(fresh, Tone(196.0, 0.3, kSampleRate, Kind::Pluck), 196.0);
    Check(firstNote >= 0 && firstNote < 0.04 * kSampleRate, "a first note within 40 ms",
          Num(firstNote / 48.0, 1) + " ms");

    // 50 cents of vibrato at 5 Hz, either way: the tracked pitch should cover most of it.
    PitchTracker vibrato;
    vibrato.Prepare(kSampleRate);
    const auto wobble = Tone(220.0, 1.5, kSampleRate, Kind::Saw, 50.0);
    double lowest = 1.0e9;
    double highest = -1.0e9;

    for (std::size_t n = 0; n < wobble.size(); ++n)
    {
        vibrato.Push(wobble[n]);

        if (n > wobble.size() / 3 && vibrato.FrequencyHz() > 0.0)
        {
            lowest = std::min(lowest, Cents(vibrato.FrequencyHz(), 220.0));
            highest = std::max(highest, Cents(vibrato.FrequencyHz(), 220.0));
        }
    }

    Check(highest - lowest > 80.0 && highest - lowest < 105.0, "vibrato of +/-50 cents is followed",
          Num(lowest, 1) + " to " + Num(highest, 1) + " cents");

    // A note stopping, abruptly or over a release, then silence: the pitch is held, and held
    // right, rather than read off the window as the note dies away.
    double worstHeld = 0.0;
    bool quiet = true;

    for (const double frequency : {82.41, 146.83, 329.63, 659.26})
    {
        for (const double releaseMs : {0.0, 5.0, 10.0, 20.0, 40.0})
        {
            PitchTracker held;
            held.Prepare(kSampleRate);
            auto note = Tone(frequency, 0.7, kSampleRate, Kind::Pluck);

            for (std::size_t n = static_cast<std::size_t>(0.3 * kSampleRate); n < note.size(); ++n)
            {
                const double t = static_cast<double>(n) / kSampleRate - 0.3;
                note[n] *= releaseMs > 0.0 ? static_cast<float>(std::exp(-1000.0 * t / releaseMs)) : 0.0f;
            }

            held.Process(note.data(), static_cast<int>(note.size()));
            worstHeld = std::max(worstHeld, std::abs(Cents(held.FrequencyHz(), frequency)));
            quiet = quiet && !held.HasPitch();
        }
    }

    Check(worstHeld < 4.0 && quiet, "after a note stops, abruptly or over a 5-40 ms release, its pitch is held",
          "worst " + Num(worstHeld) + " cents");
}

void TestRejection()
{
    std::cout << "\nNoise and bad input" << std::endl;
    PitchTracker tracker;
    tracker.Prepare(48000.0);
    std::uint32_t seed = 1u;
    int found = 0;
    std::uint64_t seen = 0;

    for (int n = 0; n < 48000 * 3; ++n)
    {
        seed = seed * 1664525u + 1013904223u;
        tracker.Push(static_cast<float>(0.3 * (static_cast<double>(seed >> 8) / 8388608.0 - 1.0)));

        if (tracker.DetectionCount() != seen)
        {
            seen = tracker.DetectionCount();
            found += tracker.HasPitch() ? 1 : 0;
        }
    }

    Check(found == 0 && tracker.FrequencyHz() == 0.0, "three seconds of white noise produce no pitch",
          std::to_string(found) + " detections found one");

    PitchTracker poisoned;
    poisoned.Prepare(48000.0);
    auto tone = Tone(220.0, 0.6, 48000.0, Kind::Saw);
    tone[4800] = std::nanf("");
    poisoned.Process(tone.data(), static_cast<int>(tone.size()));
    Check(std::abs(Cents(poisoned.FrequencyHz(), 220.0)) < 3.0, "a NaN on the input does not stick");
}

/// Brute-force full-rate YIN with the tracker's own rules, over the newest samples before `end`.
double ReferenceYin(const std::vector<float>& signal, std::size_t end, double sampleRate)
{
    const int maxLag = static_cast<int>(std::ceil(sampleRate / PitchTracker::kMinHz)) + 1;
    const int minLag = std::max(2, static_cast<int>(sampleRate / PitchTracker::kMaxHz));
    const int size = 2 * maxLag + 2;

    if (end < static_cast<std::size_t>(size))
    {
        return 0.0;
    }

    const float* w = signal.data() + end - size;
    const int base = size - maxLag;
    double energy = 0.0;

    for (int j = base; j < size; ++j)
    {
        energy += static_cast<double>(w[j]) * w[j];
    }

    if (energy < 0.0017783 * 0.0017783 * maxLag)
    {
        return 0.0;
    }

    std::vector<double> normalised(static_cast<std::size_t>(maxLag + 2), 1.0);
    double running = 0.0;

    for (int tau = 1; tau <= maxLag + 1; ++tau)
    {
        double sum = 0.0;

        for (int j = base; j < size; ++j)
        {
            const double delta = static_cast<double>(w[j]) - w[j - tau];
            sum += delta * delta;
        }

        running += sum;
        normalised[static_cast<std::size_t>(tau)] = running > 0.0 ? sum * tau / running : 1.0;
    }

    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (normalised[static_cast<std::size_t>(tau)] < 0.2)
        {
            while (tau < maxLag &&
                   normalised[static_cast<std::size_t>(tau + 1)] < normalised[static_cast<std::size_t>(tau)])
            {
                ++tau;
            }

            const double left = normalised[static_cast<std::size_t>(tau - 1)];
            const double middle = normalised[static_cast<std::size_t>(tau)];
            const double right = normalised[static_cast<std::size_t>(tau + 1)];
            const double curvature = left - 2.0 * middle + right;
            const double offset =
                std::abs(curvature) > 1.0e-12 ? std::clamp(0.5 * (left - right) / curvature, -1.0, 1.0) : 0.0;
            const double frequency = sampleRate / (tau + offset);
            return (frequency >= PitchTracker::kMinHz && frequency <= PitchTracker::kMaxHz) ? frequency : 0.0;
        }
    }

    return 0.0;
}

void TestRealGuitar()
{
    std::cout << "\nDI guitar recording against full-rate YIN" << std::endl;
    const std::filesystem::path path = std::filesystem::path(GUITARFX_DEMO_AUDIO_DIR) / "DI_Guitar_L.wav";
    guitarfx::IRWavData data;

    if (!guitarfx::irwav::LoadWavFile(path, data))
    {
        std::cout << "  [SKIP] " << path.string() << " not found" << std::endl;
        return;
    }

    std::vector<float> guitar;
    guitarfx::irwav::DownmixToMono(data, guitar);
    const double sampleRate = data.sampleRate;
    guitar.resize(std::min(guitar.size(), static_cast<std::size_t>(40.0 * sampleRate)));

    // The reference hears what the tracker hears: the same 2 kHz low-pass.
    std::vector<float> filtered(guitar.size());
    const auto low0 = guitarfx::biquad::LowPass(2000.0, guitarfx::biquad::ButterworthSectionQ(4, 0), sampleRate);
    const auto low1 = guitarfx::biquad::LowPass(2000.0, guitarfx::biquad::ButterworthSectionQ(4, 1), sampleRate);
    guitarfx::biquad::State state0;
    guitarfx::biquad::State state1;

    for (std::size_t n = 0; n < guitar.size(); ++n)
    {
        filtered[n] = static_cast<float>(state1.Process(low1, state0.Process(low0, guitar[n])));
    }

    PitchTracker tracker;
    tracker.Prepare(sampleRate);
    std::uint64_t seen = 0;
    int compared = 0;
    int trackerFound = 0;
    int referenceFound = 0;
    int both = 0;
    int within10 = 0;
    int octaves = 0;

    for (std::size_t n = 0; n < guitar.size(); ++n)
    {
        tracker.Push(guitar[n]);

        if (tracker.DetectionCount() == seen || (tracker.DetectionCount() % 4) != 0)
        {
            seen = tracker.DetectionCount();
            continue;
        }

        seen = tracker.DetectionCount();
        const double reference = ReferenceYin(filtered, n + 1, sampleRate);
        const double estimate = tracker.RawFrequencyHz();
        ++compared;
        trackerFound += estimate > 0.0 ? 1 : 0;
        referenceFound += reference > 0.0 ? 1 : 0;

        if (estimate > 0.0 && reference > 0.0)
        {
            ++both;
            const double difference = std::abs(Cents(estimate, reference));
            within10 += difference < 10.0 ? 1 : 0;
            octaves += std::abs(difference - 1200.0) < 60.0 ? 1 : 0;
        }
    }

    // The tracker deliberately skips frames where a note is dying away (the newest quarter of the
    // window under a quarter of its power), which YIN would read sharp; this staccato recording
    // has a good many. It should never report a pitch YIN does not see.
    const double agreement = both > 0 ? 100.0 * within10 / both : 0.0;
    Check(both > compared / 5, "the recording has plenty of pitched frames to compare",
          std::to_string(both) + " of " + std::to_string(compared));
    Check(trackerFound >= referenceFound * 8 / 10 && trackerFound <= referenceFound + compared / 50,
          "the tracker finds a pitch in most frames YIN does, less the dying notes",
          std::to_string(trackerFound) + " vs " + std::to_string(referenceFound));
    Check(agreement > 99.0 && octaves == 0, "and within 10 cents of it, never an octave out",
          Num(agreement) + "%, " + std::to_string(octaves) + " octave errors");
}

void ReportCost()
{
    std::cout << "\nCost per 64-sample block at 48 kHz (informational)" << std::endl;
    const auto tone = Tone(82.41, 3.0, 48000.0, Kind::Pluck);
    PitchTracker tracker;
    tracker.Prepare(48000.0);
    const auto begin = std::chrono::steady_clock::now();

    for (std::size_t start = 0; start + 64 <= tone.size(); start += 64)
    {
        tracker.Process(tone.data() + start, 64);
    }

    const std::chrono::duration<double, std::micro> elapsed = std::chrono::steady_clock::now() - begin;
    std::cout << "  mean " << Num(elapsed.count() / (tone.size() / 64.0)) << " us" << std::endl;
}
} // namespace

int main()
{
    std::cout << "=== PitchTrackerTests ===" << std::endl;

    TestAccuracy();
    TestResponse();
    TestRejection();
    TestRealGuitar();
    ReportCost();

    std::cout << "\n" << (gChecks - gFailures) << "/" << gChecks << " checks passed" << std::endl;
    return gFailures == 0 ? 0 : 1;
}
