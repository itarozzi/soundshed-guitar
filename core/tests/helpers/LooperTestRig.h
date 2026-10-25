#pragma once

// A LooperEngine for tests, its buffers, and a stand-in chain: the wet sound is half the DI,
// `latency` samples later, so where a layer puts each recording can be checked to the sample.
// Include it in one translation unit of a test executable (it defines the failure counter).

#include "dsp/TransportClock.h"
#include "dsp/looper/LooperEngine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace looper_test
{
using namespace guitarfx;
using namespace guitarfx::looper;

inline int gFailures = 0;

inline void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

struct Audio
{
    std::vector<float> di0, di1, wet0, wet1;
    LayerAudio audio;

    Audio(std::int64_t capacity, int diChannels)
        : di0(static_cast<std::size_t>(capacity)), di1(diChannels > 1 ? static_cast<std::size_t>(capacity) : 0),
          wet0(static_cast<std::size_t>(capacity)), wet1(static_cast<std::size_t>(capacity))
    {
        audio.di[0] = di0.data();
        audio.di[1] = diChannels > 1 ? di1.data() : nullptr;
        audio.wet[0] = wet0.data();
        audio.wet[1] = wet1.data();
        audio.capacity = capacity;
    }
};

using Signal = std::function<float(std::int64_t)>;

inline float Zero(std::int64_t)
{
    return 0.0f;
}

inline float Tone(std::int64_t t)
{
    return static_cast<float>(0.3 * std::sin(static_cast<double>(t) * 0.01));
}

/// An engine, its buffers, and a stand-in chain: wet(t) = chainGain * di(t - latency) + extraWet(t).
struct Rig
{
    double sampleRate;
    int block;
    int latency;
    LooperEngine engine;
    TransportClock clock;
    std::vector<std::unique_ptr<Audio>> owned;
    Audio* capture = nullptr;
    std::int64_t now = 0;
    Signal di = Zero;
    Signal extraWet = Zero;
    float chainGain = 0.5f;
    bool autoSpare = true;
    bool muted = false;
    double barOrigin = 0.0; // when the clock runs, a bar line sits here
    std::vector<float> outL, outR;
    std::vector<int> metronomeResets; // absolute sample of each requested restart
    std::vector<double> metronomeResetBars; // ...and where in the bar it lands
    LoopClickSync lastClickSync;          // the last block's

    Rig(double rate = 48000.0, int blockSize = 64, int chainLatency = 100, double captureSeconds = 4.0)
        : sampleRate(rate), block(blockSize), latency(chainLatency)
    {
        engine.Prepare(rate, blockSize);
        engine.SetTransportClock(&clock);
        engine.Settings().chainLatencySamples.store(chainLatency);
        const auto capacity = static_cast<std::int64_t>(captureSeconds * rate);
        capture = Own(capacity);
        engine.Enqueue({LooperCommand::Type::SetCapture, LooperAction::RecPlayDub, -1, &capture->audio});
        engine.Enqueue({LooperCommand::Type::SetSpare, LooperAction::RecPlayDub, -1, &Own(capacity)->audio});
    }

    Audio* Own(std::int64_t capacity)
    {
        owned.push_back(std::make_unique<Audio>(capacity, 1));
        return owned.back().get();
    }

    [[nodiscard]] float Wet(std::int64_t t) const
    {
        const float dry = t - latency >= 0 ? di(t - latency) : 0.0f;
        return chainGain * dry + extraWet(t);
    }

    void Run(std::int64_t samples)
    {
        std::vector<float> d(static_cast<std::size_t>(block));
        std::vector<float> l(static_cast<std::size_t>(block));
        std::vector<float> r(static_cast<std::size_t>(block));

        while (samples > 0)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(block, samples));

            for (int i = 0; i < n; ++i)
            {
                d[static_cast<std::size_t>(i)] = di(now + i);
                l[static_cast<std::size_t>(i)] = muted ? 0.0f : Wet(now + i);
                r[static_cast<std::size_t>(i)] = l[static_cast<std::size_t>(i)];
            }

            const double spb = clock.samplesPerBar.load();

            if (spb > 0.0)
            {
                clock.samplesSinceBar.store(std::fmod(std::fmod(static_cast<double>(now) - barOrigin, spb) + spb, spb));
            }

            engine.NoteInput(d.data(), nullptr, n, 1.0f);
            engine.Process(l.data(), r.data(), n, muted);

            lastClickSync = engine.TakeClickSync();

            if (lastClickSync.resetAt >= 0)
            {
                metronomeResets.push_back(static_cast<int>(now) + lastClickSync.resetAt);
                metronomeResetBars.push_back(lastClickSync.resetBar);
            }

            outL.insert(outL.end(), l.begin(), l.begin() + n);
            outR.insert(outR.end(), r.begin(), r.begin() + n);
            now += n;

            // As the metronome does: ready for the next block, and for a press between blocks.
            if (spb > 0.0)
            {
                clock.samplesSinceBar.store(std::fmod(std::fmod(static_cast<double>(now) - barOrigin, spb) + spb, spb));
            }
            samples -= n;

            if (autoSpare)
            {
                const LooperSnapshot s = Snap();

                if (!s.spare && s.length > 0)
                {
                    engine.Enqueue({LooperCommand::Type::SetSpare, LooperAction::RecPlayDub, -1, &Own(s.length)->audio});
                }
            }
        }
    }

    void RunTo(std::int64_t t)
    {
        Run(t - now);
    }

    void Press(LooperAction action)
    {
        engine.Press(action);
    }

    LooperSnapshot Snap() const
    {
        LooperSnapshot s;
        Check(engine.ReadSnapshot(s), "a snapshot reads consistently");
        return s;
    }

    /// Slots holding layers, oldest first.
    std::vector<SlotInfo> Layers() const
    {
        std::vector<SlotInfo> layers;

        for (const auto& slot : Snap().slots)
        {
            if (slot.status == SlotStatus::Visible || slot.status == SlotStatus::Hidden)
            {
                layers.push_back(slot);
            }
        }

        std::sort(layers.begin(), layers.end(), [](const SlotInfo& a, const SlotInfo& b) { return a.serial < b.serial; });
        return layers;
    }

    static float WetAt(const SlotInfo& slot, std::int64_t length, std::int64_t pos)
    {
        const std::int64_t index = ((slot.head + pos % length) % slot.audio->capacity + slot.audio->capacity) %
                                   slot.audio->capacity;
        return slot.audio->wet[0][index];
    }

    static float DiAt(const SlotInfo& slot, std::int64_t length, std::int64_t pos)
    {
        const std::int64_t index = ((slot.head + pos % length) % slot.audio->capacity + slot.audio->capacity) %
                                   slot.audio->capacity;
        return slot.audio->di[0][index];
    }
};
} // namespace looper_test
