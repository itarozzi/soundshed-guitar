#include "dsp/MultiPresetMixer.h"
#include "dsp/TunerEngine.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

using namespace guitarfx;

namespace
{
constexpr double kPi = 3.14159265358979323846;

/// `seconds` of a tone at `frequency` (0 for silence), with uniform white noise of RMS `noiseRms`.
std::vector<float> Tone(double sampleRate, double frequency, double seconds, double noiseRms = 0.0)
{
    std::vector<float> signal(static_cast<std::size_t>(seconds * sampleRate));
    std::uint32_t seed = 3u;

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        seed = seed * 1664525u + 1013904223u;
        const double noise = noiseRms * std::sqrt(3.0) * (static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
        signal[n] =
            static_cast<float>(0.3 * std::sin(2.0 * kPi * frequency * static_cast<double>(n) / sampleRate) + noise);
    }

    return signal;
}

/// The tuner's readings of `signal`, fed one reading's worth (2048 samples at 48 kHz) at a time.
/// Each is waited for, so a newer one cannot replace it before the worker takes it. One can still be
/// lost to the audio thread's try-lock; it is kept as a placeholder (debugRms -1), so the rest stay
/// at their own index, and the checks skip it.
std::vector<TunerEngine::Result> Readings(double sampleRate, const std::vector<float>& signal, double reference = 440.0)
{
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<TunerEngine::Result> results;
    TunerEngine tuner;
    tuner.Prepare(sampleRate);
    tuner.SetReferenceFrequency(reference);
    tuner.SetCallback([&](const TunerEngine::Result& result) {
        std::lock_guard<std::mutex> lock(mutex);
        results.push_back(result);
        cv.notify_one();
    });
    tuner.SetEnabled(true);
    const auto chunk = static_cast<std::size_t>(std::lround(sampleRate * 2048.0 / 48000.0));

    for (std::size_t start = 0; start + chunk <= signal.size(); start += chunk)
    {
        std::size_t before = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            before = results.size();
        }
        tuner.Process(signal.data() + start, static_cast<int>(chunk));
        std::unique_lock<std::mutex> lock(mutex);

        if (!cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return results.size() > before; }))
        {
            TunerEngine::Result lost;
            lost.debugRms = -1.0;
            results.push_back(lost);
        }
    }

    tuner.SetEnabled(false);
    std::lock_guard<std::mutex> lock(mutex);
    return results;
}

/// The last reading that was not lost.
TunerEngine::Result Last(const std::vector<TunerEngine::Result>& readings)
{
    for (auto it = readings.rbegin(); it != readings.rend(); ++it)
    {
        if (it->debugRms >= 0.0)
        {
            return *it;
        }
    }

    return {};
}

double Cents(double frequency, double reference)
{
    return 1200.0 * std::log2(frequency / reference);
}

/// The detected readings from `from` onwards, as cents from `truth`: mean and spread.
bool Steady(const std::vector<TunerEngine::Result>& readings, std::size_t from, double truth, double& mean,
            double& spread)
{
    std::vector<double> cents;

    for (std::size_t i = from; i < readings.size(); ++i)
    {
        if (readings[i].debugRms < 0.0)
        {
            continue;
        }

        if (!readings[i].detected)
        {
            return false;
        }

        cents.push_back(Cents(readings[i].frequency, truth));
    }

    if (cents.size() < 10)
    {
        return false;
    }

    mean = 0.0;
    spread = 0.0;

    for (const double c : cents)
    {
        mean += c / static_cast<double>(cents.size());
    }

    for (const double c : cents)
    {
        spread += (c - mean) * (c - mean) / static_cast<double>(cents.size());
    }

    spread = std::sqrt(spread);
    return true;
}

/// What moving the tuner onto the shared PitchTracker changed. Returns false on a failure.
bool TestPitchTracker()
{
    // 192 kHz: the old detector's longest period no longer fit its 4096-sample window, and it
    // found nothing at all.
    double mean = 0.0;
    double spread = 0.0;
    auto readings = Readings(192000.0, Tone(192000.0, 110.0, 1.0));

    if (!Steady(readings, 8, 110.0, mean, spread) || std::abs(mean) > 1.0 || Last(readings).noteName != "A" ||
        Last(readings).octave != 2)
    {
        std::cerr << "Tuner did not read A2 at 192 kHz\n";
        return false;
    }

    // Above about 1.1 kHz the old detector read an octave low (its normalisation began at the
    // shortest period); E6, the 24th fret of the high E, now reads E6.
    readings = Readings(48000.0, Tone(48000.0, 1318.51, 1.0));

    if (!Steady(readings, 8, 1318.51, mean, spread) || std::abs(mean) > 1.0 || Last(readings).noteName != "E" ||
        Last(readings).octave != 6)
    {
        std::cerr << "Tuner did not read E6: " << Last(readings).noteName << Last(readings).octave << "\n";
        return false;
    }

    // Each reading averages the tracker's 5 ms detections over its 43 ms. With white noise 20 dB
    // below a held A2, readings scatter by a fraction of a cent; the old single 85 ms window
    // scattered by 2.6 cents and read 1.7 cents sharp.
    readings = Readings(48000.0, Tone(48000.0, 110.0, 2.0, 0.0211));

    if (!Steady(readings, 8, 110.0, mean, spread) || std::abs(mean) > 0.5 || spread > 1.0)
    {
        std::cerr << "Tuner readings in noise: mean " << mean << ", spread " << spread << " cents\n";
        return false;
    }

    std::cout << "Noisy A2: mean " << mean << " cents, spread " << spread << " cents\n";

    // The reference pitch: A4 = 432 Hz reads in tune against 432 and 31.8 cents flat against 440.
    const auto tone432 = Tone(48000.0, 432.0, 1.0);
    const auto at432 = Readings(48000.0, tone432, 432.0);
    const auto at440 = Readings(48000.0, tone432, 440.0);

    if (!Steady(at432, 8, 432.0, mean, spread) || !Steady(at440, 8, 432.0, mean, spread) ||
        Last(at432).noteName != "A" || std::abs(Last(at432).centOffset) > 0.5 || Last(at440).noteName != "A" ||
        std::abs(Last(at440).centOffset + 31.77) > 0.5)
    {
        std::cerr << "Tuner reference pitch not honoured\n";
        return false;
    }

    // Silence reports no note, and a new note is reported within three readings (130 ms).
    auto change = Tone(48000.0, 0.0, 0.5);
    const auto a2 = Tone(48000.0, 110.0, 0.8);
    const auto d3 = Tone(48000.0, 146.83, 0.5);
    change.insert(change.end(), a2.begin(), a2.end());
    const std::size_t firstD3 = (change.size() + 2047) / 2048;
    change.insert(change.end(), d3.begin(), d3.end());
    readings = Readings(48000.0, change);
    bool silent = readings.size() > firstD3 + 3;

    for (std::size_t i = 0; i < 10 && i < readings.size(); ++i)
    {
        silent = silent && !readings[i].detected;
    }

    bool followed = false;

    for (std::size_t i = firstD3; i < firstD3 + 3 && i < readings.size(); ++i)
    {
        followed = followed || (readings[i].detected && std::abs(Cents(readings[i].frequency, 146.83)) < 5.0);
    }

    if (!silent || !followed)
    {
        std::cerr << "Tuner silence/note change: silent " << silent << ", followed " << followed << "\n";
        return false;
    }

    return true;
}
} // namespace

int main()
{
    constexpr double sampleRate = 48000.0;
    constexpr double frequency = 110.0;
    constexpr int blockSize = 64;
    constexpr double pi = 3.14159265358979323846;

    TunerEngine tuner;
    tuner.Prepare(sampleRate);
    tuner.SetReferenceFrequency(440.0);
    tuner.SetLiveMode(false);
    if (tuner.IsLiveMode() || tuner.GetReferenceFrequency() != 440.0)
    {
        std::cerr << "Tuner settings were not retained\n";
        return 1;
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::optional<TunerEngine::Result> detected;
    tuner.SetCallback([&](const TunerEngine::Result& result) {
        if (result.detected)
        {
            std::lock_guard<std::mutex> lock(mutex);
            detected = result;
            cv.notify_one();
        }
    });
    tuner.SetEnabled(true);

    std::vector<float> block(blockSize);
    for (int start = 0; start < 16384; start += blockSize)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            block[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 * std::sin(2.0 * pi * frequency * (start + i) / sampleRate));
        }
        tuner.Process(block.data(), blockSize);
    }

    TunerEngine::Result result;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(3), [&] { return detected.has_value(); }))
        {
            std::cerr << "Tuner did not detect a sustained A2 input\n";
            return 1;
        }
        result = *detected;
    }

    if (result.noteName != "A" || result.octave != 2 ||
        std::abs(result.frequency - frequency) > 3.0 || result.debugRms < 0.1)
    {
        std::cerr << "Unexpected tuner result: " << result.noteName << result.octave << " at "
                  << result.frequency << " Hz\n";
        return 1;
    }

    tuner.SetEnabled(false);
    if (tuner.IsEnabled())
    {
        std::cerr << "Tuner remained enabled\n";
        return 1;
    }

    // The active worker must remain bound to its engine when the owning mixer moves.
    MultiPresetMixer source;
    source.Prepare(sampleRate, blockSize);
    source.SetTunerReferenceFrequency(432.0);
    source.SetLiveTunerMode(false);
    source.SetTunerEnabled(true);
    MultiPresetMixer moved(std::move(source));
    if (!moved.IsTunerEnabled() || moved.IsLiveTunerMode() || moved.GetTunerReferenceFrequency() != 432.0)
    {
        std::cerr << "Mixer move lost active tuner state\n";
        return 1;
    }
    moved.SetTunerEnabled(false);
    return TestPitchTracker() ? 0 : 1;
}
