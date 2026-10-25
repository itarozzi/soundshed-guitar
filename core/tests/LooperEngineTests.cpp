// LooperEngine: the looper's realtime half, driven by tests only, with the chain, the clock and the
// latencies injected (LooperTestRig.h).

#include "AudioThreadAllocations.h"
#include "LooperTestRig.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace guitarfx;
using namespace guitarfx::looper;
using namespace looper_test;

namespace
{
/// A free take is the DI as played and the wet sound aligned to it, and plays back exactly.
void TestFreeTakeRecordsAndPlaysBackExactly()
{
    Rig rig;
    const std::int64_t start = 1000;
    const std::int64_t length = 24000;
    rig.di = [&](std::int64_t t) { return t >= start && t < start + length ? Tone(t) : 0.0f; };
    rig.RunTo(start);
    rig.Press(LooperAction::RecPlayDub);
    Check(rig.engine.State() == LooperState::Recording, "a press records at once");
    rig.RunTo(start + length);
    rig.Press(LooperAction::RecPlayDub);
    Check(rig.engine.State() == LooperState::Playing, "the second press closes the loop and plays it");
    rig.Run(3 * length);

    const auto snap = rig.Snap();
    const auto layers = rig.Layers();
    Check(snap.length == length, "the loop is as long as the take");
    Check(layers.size() == 1 && layers[0].firstLayer, "one layer, the first");

    if (layers.size() != 1)
    {
        return;
    }

    const SlotInfo& layer = layers[0];
    Check(layer.final, "the layer is final once its tail is done");
    bool diExact = true;
    bool wetExact = true;

    for (std::int64_t p = 0; p < length; ++p)
    {
        diExact = diExact && Rig::DiAt(layer, length, p) == Tone(start + p);
        wetExact = wetExact && Rig::WetAt(layer, length, p) == 0.5f * Tone(start + p);
    }

    Check(diExact, "the DI is the input as played");
    Check(wetExact, "the wet sound sits at the DI's position, the chain latency taken off");

    bool playbackExact = true;

    for (std::int64_t t = start + length; t < start + 3 * length; ++t)
    {
        const float expected = rig.Wet(t) + Rig::WetAt(layer, length, (t - start - length) % length) * 1.0f;
        playbackExact = playbackExact && rig.outL[static_cast<std::size_t>(t)] == expected;
    }

    Check(playbackExact, "playback restarts the take on the closing press and repeats it exactly");
}

/// An overdub is recorded D behind the playhead, so what is played along with the loop lands in time.
void TestOverdubLandsInTimeWithTheLoop()
{
    Rig rig;
    const int align = 300;
    const std::int64_t length = 24000;
    const std::int64_t close = 1000 + length;
    const std::int64_t q = 5000;
    // Heard at loop position q, played, and reaching the DI tap D later.
    const std::int64_t impulse = close + length + q + align;
    rig.engine.Settings().alignSamples.store(align);
    rig.di = [&](std::int64_t t) { return t == impulse ? 1.0f : 0.0f; };
    rig.RunTo(1000);
    rig.Press(LooperAction::Record);
    rig.RunTo(close);
    rig.Press(LooperAction::Play);
    rig.RunTo(close + length + 100);
    rig.Press(LooperAction::Overdub);
    Check(rig.engine.State() == LooperState::Overdubbing, "overdubbing");
    rig.RunTo(close + length + 20000);
    rig.Press(LooperAction::Play);
    rig.Run(length);

    const auto layers = rig.Layers();
    Check(layers.size() == 2, "two layers");

    if (layers.size() != 2)
    {
        return;
    }

    const SlotInfo& dub = layers[1];
    // Pressed 200 samples before the record wrap: too close for a layer, so it opened on the wrap.
    Check(dub.spanStart == 0, "an overdub pressed just before the wrap opens on it");
    Check(Rig::DiAt(dub, length, q) == 1.0f, "the overdub's DI is at the position the player heard");
    Check(Rig::WetAt(dub, length, q) == 0.5f, "and so is its wet sound");
    Check(Rig::DiAt(dub, length, q + 1) == 0.0f && Rig::DiAt(dub, length, q - 1) == 0.0f, "to the sample");
}

/// Held across the wrap, Overdub stacks one layer per pass, joined to the sample.
void TestOverdubAcrossWrapsMakesALayerPerPass()
{
    Rig rig;
    const std::int64_t length = 4800;
    rig.di = [&](std::int64_t t) { return t >= 1000 + length + 1000 ? 0.25f : 0.0f; };
    rig.RunTo(1000);
    rig.Press(LooperAction::Record);
    rig.RunTo(1000 + length);
    rig.Press(LooperAction::Play);
    rig.RunTo(1000 + length + 1000);
    rig.Press(LooperAction::Overdub);
    // The loop closed at 1000 + length; three wraps on, 3000 samples into the fourth pass.
    rig.RunTo(1000 + 4 * length + 3000);
    rig.Press(LooperAction::Play);
    rig.Run(2 * length);

    const auto layers = rig.Layers();
    Check(layers.size() == 5, "four passes held in Overdub are four layers on the first");

    if (layers.size() != 5)
    {
        return;
    }

    Check(layers[1].spanStart == 1000 && layers[1].spanLength == length - 1000, "the first pass runs to the wrap");
    Check(layers[2].spanStart == 0 && layers[2].spanLength == length, "the next is a whole pass");
    Check(layers[3].spanStart == 0 && layers[3].spanLength == length, "and the next");
    Check(layers[4].spanStart == 0 && layers[4].spanLength == 3000, "the last ends on the press");

    for (int k = 1; k <= 3; ++k)
    {
        Check(Rig::WetAt(layers[static_cast<std::size_t>(k)], length, length - 1) == 0.125f,
              "a pass ends at full level on the wrap " + std::to_string(k));
        Check(Rig::WetAt(layers[static_cast<std::size_t>(k + 1)], length, 0) == 0.125f &&
                  Rig::DiAt(layers[static_cast<std::size_t>(k + 1)], length, 0) == 0.25f,
              "and the next starts at full level, with no ramp " + std::to_string(k));
    }

    Check(Rig::DiAt(layers[1], length, 1000) == 0.0f && Rig::DiAt(layers[1], length, 1000 + 240) == 0.25f,
          "a pressed punch-in ramps in over 5 ms");
    Check(Rig::DiAt(layers[4], length, 2999) == 0.0f, "a pressed punch-out ramps out");
}

/// A loop of one DC layer, and a second whose wet sound is 0.2 across the whole loop.
std::unique_ptr<Rig> MakeTwoLayerRig(std::int64_t length)
{
    auto rig = std::make_unique<Rig>();
    Rig& r = *rig;
    const std::int64_t close = 1000 + length;
    r.di = [close, length](std::int64_t t) {
        if (t >= 1000 && t < close)
        {
            return 0.2f;
        }

        return t >= close + length && t < close + 2 * length ? 0.4f : 0.0f;
    };
    r.RunTo(1000);
    r.Press(LooperAction::Record);
    r.RunTo(close);
    r.Press(LooperAction::Play);
    r.RunTo(close + length);
    r.Press(LooperAction::Overdub);
    r.RunTo(close + 2 * length);
    r.Press(LooperAction::Play);
    r.Run(length);
    return rig;
}

float OutputAt(const Rig& rig, std::int64_t loopPos, std::int64_t length)
{
    // Playback alone (the DI is silent by now), at a loop position in the last pass run.
    const std::int64_t close = 1000 + length;
    const std::int64_t t = rig.now - length + ((loopPos - (rig.now - length - close)) % length + length) % length;
    return rig.outL[static_cast<std::size_t>(t)];
}

void TestUndoRedoAndRemove()
{
    const std::int64_t length = 9600;
    auto rigPtr = MakeTwoLayerRig(length);
    Rig& rig = *rigPtr;
    Check(rig.Layers().size() == 2, "two layers");
    Check(std::abs(OutputAt(rig, length / 2, length) - 0.3f) < 1e-6f, "both layers play");

    rig.Press(LooperAction::Undo);
    rig.Run(length);
    Check(std::abs(OutputAt(rig, length / 2, length) - 0.1f) < 1e-6f, "undo takes the newest layer away");
    Check(rig.Snap().canRedo, "and it can be redone");

    rig.Press(LooperAction::Redo);
    rig.Run(length);
    Check(std::abs(OutputAt(rig, length / 2, length) - 0.3f) < 1e-6f, "redo brings it back");

    // Remove the first layer, not the newest, then undo the removal.
    const int first = [&] {
        const auto snap = rig.Snap();

        for (int s = 0; s < kMaxLayers; ++s)
        {
            if (snap.slots[static_cast<std::size_t>(s)].firstLayer)
            {
                return s;
            }
        }

        return -1;
    }();
    rig.engine.Enqueue({LooperCommand::Type::RemoveLayer, LooperAction::RecPlayDub, first, nullptr});
    rig.Run(length);
    Check(std::abs(OutputAt(rig, length / 2, length) - 0.2f) < 1e-6f, "any layer can be removed");
    rig.Press(LooperAction::Undo);
    rig.Run(length);
    Check(std::abs(OutputAt(rig, length / 2, length) - 0.3f) < 1e-6f, "undo brings a removed layer back");

    // Undo fades over 10 ms rather than stepping.
    rig.Press(LooperAction::Undo);
    const std::size_t before = rig.outL.size();
    rig.Run(1000);
    bool smooth = true;

    for (std::size_t i = before + 1; i < before + 1000; ++i)
    {
        smooth = smooth && std::abs(rig.outL[i] - rig.outL[i - 1]) < 0.001f;
    }

    Check(smooth, "an undone layer fades out");
}

void TestMixBalanceMuteSolo()
{
    const std::int64_t length = 9600;
    auto rigPtr = MakeTwoLayerRig(length);
    Rig& rig = *rigPtr;
    const auto snap = rig.Snap();
    int firstSlot = -1;
    int dubSlot = -1;

    for (int s = 0; s < kMaxLayers; ++s)
    {
        const auto& slot = snap.slots[static_cast<std::size_t>(s)];

        if (slot.status == SlotStatus::Visible)
        {
            (slot.firstLayer ? firstSlot : dubSlot) = s;
        }
    }

    Check(std::abs(OutputAt(rig, length / 2, length) - 0.3f) < 1e-6f &&
              std::abs(rig.outR.back() - rig.outL.back()) < 1e-7f,
          "balance is unity on both sides at centre");

    rig.engine.Mix(dubSlot).balance.store(1.0f);
    rig.Run(length);
    Check(std::abs(OutputAt(rig, length / 2, length) - 0.1f) < 1e-6f &&
              std::abs(rig.outR[rig.outR.size() - length / 2] - 0.3f) < 1e-6f,
          "turned fully right, the left side loses the layer and the right keeps it at unity");
    rig.engine.Mix(dubSlot).balance.store(0.0f);

    rig.engine.Mix(firstSlot).muted.store(true);
    rig.Run(length);
    Check(std::abs(OutputAt(rig, length / 2, length) - 0.2f) < 1e-6f, "a muted layer is not heard");
    rig.engine.Mix(firstSlot).muted.store(false);

    rig.engine.Mix(firstSlot).solo.store(true);
    rig.Run(length);
    Check(std::abs(OutputAt(rig, length / 2, length) - 0.1f) < 1e-6f, "with one soloed, only it is heard");
    rig.engine.Mix(firstSlot).solo.store(false);

    rig.engine.Mix(dubSlot).level.store(0.5f);
    rig.Run(length);
    Check(std::abs(OutputAt(rig, length / 2, length) - 0.2f) < 1e-6f, "a layer's level scales it");
}

/// Armed waits for the first note over a hum louder than the default threshold, and starts the take
/// 10 ms before it.
void TestArmedStartsOnTheFirstNoteWithPreRoll()
{
    Rig rig;
    rig.engine.Settings().startArmed.store(true);
    const std::int64_t note = 30000;
    rig.di = [&](std::int64_t t) {
        // -45 dBFS of hum, then a note.
        const float hum = 0.0056f * static_cast<float>(std::sin(static_cast<double>(t) * 0.0785));
        return t < note ? hum : 0.4f;
    };
    rig.RunTo(1000);
    rig.Press(LooperAction::Record);
    Check(rig.engine.State() == LooperState::Armed, "armed");
    rig.RunTo(note);
    Check(rig.engine.State() == LooperState::Armed, "hum 12 dB under the measured floor's trigger does not start it");
    rig.Run(64);
    Check(rig.engine.State() == LooperState::Recording, "the note starts it");
    rig.RunTo(note + 24000);
    rig.Press(LooperAction::RecPlayDub);
    rig.Run(1000);

    const auto layers = rig.Layers();
    Check(layers.size() == 1, "one layer");

    if (layers.size() != 1)
    {
        return;
    }

    const std::int64_t preRoll = 480;
    const std::int64_t length = rig.Snap().length;
    Check(length == note + 24000 - (note - preRoll), "the take runs from the pre-roll to the press");
    Check(Rig::DiAt(layers[0], length, preRoll) == 0.4f && Rig::DiAt(layers[0], length, preRoll - 1) != 0.4f,
          "the note sits 10 ms into the take");
}

/// Bars: the press counts in, records from the downbeat (D later), and closes on the exact sample.
void TestBarsCountsInAndClosesOnTheSample()
{
    struct Case
    {
        double rate;
        double bpm;
        int num;
        int den;
    };

    for (const Case& c : {Case{48000.0, 120.0, 4, 4}, Case{44100.0, 40.0, 4, 4}, Case{48000.0, 240.0, 3, 4},
                          Case{96000.0, 120.0, 6, 8}, Case{44100.0, 120.0, 7, 8}})
    {
        Rig rig(c.rate, 128, 64, 12.0);
        const double spb = c.rate * 60.0 / c.bpm * 4.0 / c.den * c.num;
        const std::string name = std::to_string(c.num) + "/" + std::to_string(c.den) + " at " +
                                 std::to_string(static_cast<int>(c.bpm)) + " BPM, " +
                                 std::to_string(static_cast<int>(c.rate)) + " Hz";
        rig.clock.samplesPerBar.store(spb);
        rig.clock.samplesPerBeat.store(spb / c.num);
        rig.engine.Settings().lengthBars.store(true);
        rig.engine.Settings().bars.store(1);
        rig.engine.Settings().countInBars.store(1);
        const int align = 200;
        rig.engine.Settings().alignSamples.store(align);
        const std::int64_t press = 1000;
        const auto bar = static_cast<std::int64_t>(std::llround(spb));
        rig.RunTo(press);
        rig.Press(LooperAction::RecPlayDub);
        rig.Run(1);
        Check(rig.engine.State() == LooperState::CountIn, name + ": counting in");
        Check(!rig.metronomeResets.empty() && rig.metronomeResets.front() == press,
              name + ": the press restarts the metronome's bar on its sample");
        const std::int64_t recordStart = press + bar + align;
        rig.RunTo(recordStart);
        Check(rig.engine.State() == LooperState::CountIn, name + ": still counting in at the downbeat + D");
        rig.Run(1);
        Check(rig.engine.State() == LooperState::Recording, name + ": recording D after the downbeat");
        rig.RunTo(recordStart + bar);
        Check(rig.engine.State() == LooperState::Recording, name + ": still recording on its last sample");
        rig.Run(1);
        Check(rig.engine.State() == LooperState::Playing, name + ": closed on the exact sample");
        const auto snap = rig.Snap();
        Check(snap.length == bar, name + ": exactly one bar long");
        Check(snap.barsLoop, name + ": a Bars loop");
        // The playhead stays on the bar line: it was at D when the take closed.
        Check(snap.playhead == (align + 1) % bar, name + ": the playhead runs on the bar grid");
        rig.metronomeResets.clear();
        rig.Run(bar);
        Check(rig.metronomeResets.size() == 1 && rig.metronomeResets[0] == recordStart + 2 * bar - align,
              name + ": the wrap re-phases the metronome to bar 1");
    }
}

/// Bars + armed with the click running snaps the take to the nearest bar line, either way.
void TestBarsArmedSnapsToTheNearestBarLine()
{
    for (const int offset : {150, -150})
    {
        Rig rig;
        const double spb = 48000.0;
        rig.clock.samplesPerBar.store(spb);
        rig.clock.running.store(true);
        rig.barOrigin = 0.0;
        rig.engine.Settings().lengthBars.store(true);
        rig.engine.Settings().bars.store(1);
        rig.engine.Settings().startArmed.store(true);
        const std::int64_t barLine = 96000;
        const std::int64_t note = barLine + offset;
        // Stopping before the take closes, so the seam adds nothing onto the start.
        rig.di = [&](std::int64_t t) { return t >= note && t < note + 40000 ? 0.4f : 0.0f; };
        rig.RunTo(1000);
        rig.Press(LooperAction::Record);
        rig.RunTo(note + 48000 + 1000);
        const auto layers = rig.Layers();
        Check(layers.size() == 1, "one layer");

        if (layers.size() != 1)
        {
            continue;
        }

        if (offset > 0)
        {
            Check(Rig::DiAt(layers[0], 48000, offset) == 0.4f && Rig::DiAt(layers[0], 48000, offset - 1) == 0.0f,
                  "a note just after the bar line: the take starts on the line, behind it");
        }
        else
        {
            Check(Rig::DiAt(layers[0], 48000, 0) == 0.4f, "a note just before the bar line: the take starts on it");
        }
    }
}

/// The ring-out: a tail played past the end is written onto the first layer's start, and stops when
/// it dies away, when the player plays on, or after 5 s.
void TestRingOut()
{
    // Dies away.
    {
        Rig rig;
        const std::int64_t length = 48000;
        const std::int64_t close = 1000 + length;
        rig.di = [&](std::int64_t t) { return t >= 1000 && t < close ? 0.2f : 0.0f; };
        rig.extraWet = [&](std::int64_t t) {
            return t >= close ? static_cast<float>(0.3 * std::exp(-static_cast<double>(t - close) / 2400.0)) : 0.0f;
        };
        rig.RunTo(1000);
        rig.Press(LooperAction::Record);
        rig.RunTo(close);
        rig.Press(LooperAction::Play);
        rig.Run(2 * length);
        const auto layers = rig.Layers();

        if (layers.size() == 1)
        {
            const SlotInfo& layer = layers[0];
            // 0.3 e^(-x/2400) is under -70 dBFS after about 16,460 samples, plus 20 ms of quiet.
            Check(layer.ringOut > 16000 && layer.ringOut < 18500, "a decaying tail rings out until it is quiet");
            const std::int64_t k = 1000;
            const float tail = static_cast<float>(0.3 * std::exp(-static_cast<double>(rig.latency + k) / 2400.0));
            Check(Rig::WetAt(layer, length, k) == 0.1f + tail, "the tail is added onto the loop's start");
            Check(layer.final, "and the layer is final after it");
        }
        else
        {
            Check(false, "one layer");
        }
    }

    // The player plays on.
    {
        Rig rig;
        const std::int64_t length = 48000;
        rig.di = [&](std::int64_t t) { return t >= 1000 ? 0.2f : 0.0f; };
        rig.RunTo(1000);
        rig.Press(LooperAction::Record);
        rig.RunTo(1000 + length);
        rig.Press(LooperAction::Play);
        rig.Run(4800);
        const auto layers = rig.Layers();
        Check(layers.size() == 1 && layers[0].ringOut == 240, "playing on stops the ring-out with a 5 ms fade");
    }

    // At most 5 s.
    {
        Rig rig(48000.0, 256, 100, 10.0);
        const std::int64_t length = 8 * 48000;
        const std::int64_t close = 1000 + length;
        rig.di = [&](std::int64_t t) { return t >= 1000 && t < close ? 0.2f : 0.0f; };
        rig.extraWet = [&](std::int64_t t) { return t >= close ? 0.01f : 0.0f; };
        rig.RunTo(1000);
        rig.Press(LooperAction::Record);
        rig.RunTo(close);
        rig.Press(LooperAction::Play);
        rig.Run(6 * 48000);
        const auto layers = rig.Layers();
        Check(layers.size() == 1 && layers[0].ringOut == 5 * 48000, "a tail that never dies rings out for 5 s");
    }
}

/// The tuner's muted mode punches a take out and keeps the loop in time, silently.
void TestTunerMutePunchesOutAndKeepsTime()
{
    Rig rig;
    const std::int64_t length = 9600;
    rig.di = [&](std::int64_t t) { return t >= 1000 && t < 1000 + length ? 0.2f : 0.0f; };
    rig.RunTo(1000);
    rig.Press(LooperAction::Record);
    rig.RunTo(1000 + length);
    rig.Press(LooperAction::Play);
    rig.Run(length);
    rig.Press(LooperAction::Overdub);
    rig.Run(4000);
    rig.muted = true;
    rig.Run(64);
    Check(rig.engine.State() == LooperState::Playing, "muting punches the overdub out");
    const auto playheadBefore = rig.Snap().playhead;
    const std::size_t from = rig.outL.size();
    rig.Run(1000);
    bool silent = true;

    for (std::size_t i = from; i < rig.outL.size(); ++i)
    {
        silent = silent && rig.outL[i] == 0.0f;
    }

    Check(silent, "nothing plays while muted");
    Check(rig.Snap().playhead == (playheadBefore + 1000) % length, "but the loop keeps time");
    rig.muted = false;
    Check(rig.Layers().size() == 2, "the punched-out take is a layer");
}

/// Stop fades and returns to the top; Clear frees every slot.
void TestStopAndClear()
{
    Rig rig;
    const std::int64_t length = 9600;
    rig.di = [&](std::int64_t t) { return t >= 1000 && t < 1000 + length ? 0.2f : 0.0f; };
    rig.RunTo(1000);
    rig.Press(LooperAction::Record);
    rig.RunTo(1000 + length);
    rig.Press(LooperAction::Play);
    rig.Run(3000);
    rig.Press(LooperAction::Stop);
    Check(rig.engine.State() == LooperState::Stopped, "stopped");
    rig.Run(1000);
    Check(!rig.Snap().playing && rig.Snap().playhead == 0, "after the fade, the loop is back at the top");
    Check(rig.outL.back() == 0.0f, "and silent");
    Check(rig.engine.WantsChainsAwake() == false, "a stopped loop does not hold the chains awake");
    rig.Press(LooperAction::Play);
    rig.Run(100);
    Check(rig.Snap().playing && rig.Snap().playhead == 100, "play starts from the top");
    rig.Press(LooperAction::Clear);
    rig.Run(1000);
    const auto snap = rig.Snap();
    Check(snap.state == LooperState::Empty && snap.length == 0, "clear empties the loop");
    bool allFree = true;

    for (const auto& slot : snap.slots)
    {
        allFree = allFree && slot.status == SlotStatus::Free && slot.audio == nullptr;
    }

    Check(allFree, "and lets go of every buffer");
    Check(snap.capture == &rig.capture->audio, "the capture buffer stays");
}

/// A free take played to a running click: the loop starts where the player heard the bar, D before
/// its first sample reached the DI tap, and plays back on that grid. The click is held while the loop
/// is stopped, and restarted on the loop's place in the bar when it plays and when it wraps.
void TestFreeTakeOnTheClickKeepsItsPlaceInTheBar()
{
    Rig rig;
    const double spb = 48000.0;
    const int align = 300;
    rig.clock.samplesPerBar.store(spb);
    rig.clock.samplesPerBeat.store(spb / 4.0);
    rig.clock.running.store(true);
    rig.engine.Settings().alignSamples.store(align);
    const std::int64_t press = 10048;
    const std::int64_t length = 70016;
    rig.RunTo(press);
    rig.Press(LooperAction::Record);
    rig.RunTo(press + length);
    rig.Press(LooperAction::Play);
    rig.Run(64);
    const auto snap = rig.Snap();
    const double clickBar = static_cast<double>(press - align) / spb;
    const auto frac = [](double x) { return x - std::floor(x); };
    Check(snap.length == length && std::abs(snap.clickBar - clickBar) < 1e-9,
          "the loop starts where the bar was as the player heard it: " + std::to_string(snap.clickBar));
    Check(snap.playhead == align + 64, "a take played to the click plays back D in, as a Bars take does");
    Check(!rig.metronomeResets.empty() && rig.metronomeResets.back() == press + length &&
              std::abs(rig.metronomeResetBars.back() - frac(clickBar + align / spb)) < 1e-9,
          "the click is put on the loop's place as it closes");

    rig.Press(LooperAction::Stop);
    rig.Run(2000);
    rig.Run(64);
    Check(rig.lastClickSync.following && rig.lastClickSync.silentUntil >= 64 && rig.lastClickSync.resetAt < 0,
          "the click is held while the loop is stopped");

    rig.metronomeResets.clear();
    rig.metronomeResetBars.clear();
    rig.Press(LooperAction::Play);
    rig.Run(64);
    Check(rig.lastClickSync.following && rig.lastClickSync.silentUntil == 0 && rig.lastClickSync.resetAt == 0 &&
              std::abs(rig.lastClickSync.resetBar - clickBar) < 1e-9,
          "Play restarts the click on the loop's start");
    Check(std::abs(rig.lastClickSync.barAtStart - clickBar) < 1e-9, "and keeps it there");
    rig.Run(length - 64);
    rig.Run(64);
    Check(rig.metronomeResets.size() == 2 && rig.metronomeResets.back() == rig.now - 64 &&
              std::abs(rig.metronomeResetBars.back() - clickBar) < 1e-9,
          "a wrap of a loop that is not whole bars puts it back on the loop's start");
}

/// Nothing the audio thread does for any command allocates.
void TestNoAllocationOnTheAudioThread()
{
    LooperEngine engine;
    TransportClock clock;
    clock.samplesPerBar.store(24000.0);
    engine.Prepare(48000.0, 64);
    engine.SetTransportClock(&clock);
    Audio capture(4 * 48000, 1);
    Audio spareA(4 * 48000, 1);
    Audio spareB(24000, 1);
    engine.Enqueue({LooperCommand::Type::SetCapture, LooperAction::RecPlayDub, -1, &capture.audio});
    engine.Enqueue({LooperCommand::Type::SetSpare, LooperAction::RecPlayDub, -1, &spareA.audio});
    std::vector<float> di(64, 0.3f);
    std::vector<float> l(64, 0.1f);
    std::vector<float> r(64, 0.1f);

    const auto allocations = audio_thread_allocations::CountOnAudioThread([&] {
        const auto run = [&](int blocks) {
            for (int b = 0; b < blocks; ++b)
            {
                engine.NoteInput(di.data(), nullptr, 64, 1.0f);
                engine.Process(l.data(), r.data(), 64, false);
            }
        };

        for (const auto action : {LooperAction::RecPlayDub, LooperAction::RecPlayDub, LooperAction::Overdub,
                                  LooperAction::Play, LooperAction::Undo, LooperAction::Redo, LooperAction::Stop,
                                  LooperAction::Play, LooperAction::Clear})
        {
            engine.Press(action);
            run(400);
        }

        engine.Settings().lengthBars.store(true);
        engine.Settings().startArmed.store(true);
        engine.Press(LooperAction::Record);
        run(1200);
        engine.Press(LooperAction::Overdub);
        run(400);
        engine.Press(LooperAction::Clear);
        run(10);
    });

    Check(allocations.count == 0, "the engine allocates nothing on the audio thread: " +
                                      audio_thread_allocations::Describe(allocations));
}
} // namespace

int main()
{
    TestFreeTakeRecordsAndPlaysBackExactly();
    TestOverdubLandsInTimeWithTheLoop();
    TestOverdubAcrossWrapsMakesALayerPerPass();
    TestUndoRedoAndRemove();
    TestMixBalanceMuteSolo();
    TestArmedStartsOnTheFirstNoteWithPreRoll();
    TestBarsCountsInAndClosesOnTheSample();
    TestBarsArmedSnapsToTheNearestBarLine();
    TestRingOut();
    TestTunerMutePunchesOutAndKeepsTime();
    TestStopAndClear();
    TestFreeTakeOnTheClickKeepsItsPlaceInTheBar();
    TestNoAllocationOnTheAudioThread();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "LooperEngineTests passed" << std::endl;
    return 0;
}
