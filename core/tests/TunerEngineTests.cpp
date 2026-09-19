#include "dsp/MultiPresetMixer.h"
#include "dsp/TunerEngine.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

using namespace guitarfx;

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
    return 0;
}
