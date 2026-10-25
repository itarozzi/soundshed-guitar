#pragma once

// TransportClock — where the bar grid is, for anything on the audio thread that has to land on it.
//
// The metronome owns the grid: it writes the bar's length and how far into the bar the next block
// starts, once per block, after it renders. The looper reads it at the start of its own block (the
// mixer runs ahead of the metronome in the same callback), to size a Bars loop and to snap an armed
// take to the nearest bar line. Both are on the audio thread, so relaxed atomics are enough; the
// message thread reads it only for display.
//
// The tempo is known even while the click is off: `samplesPerBar` and `samplesPerBeat` always hold
// the current tempo and meter, and `running` says whether the grid is being clicked.

#include <atomic>

namespace guitarfx
{
struct TransportClock
{
    /// One bar at the current tempo and time signature, in samples. 0 until a tempo is known.
    std::atomic<double> samplesPerBar{0.0};
    /// One beat (the time signature's denominator) in samples.
    std::atomic<double> samplesPerBeat{0.0};
    /// Samples from the last bar line to the start of the next block, in [0, samplesPerBar).
    std::atomic<double> samplesSinceBar{0.0};
    /// Whether the metronome is clicking this grid. With it off, nothing is in phase with it.
    std::atomic<bool> running{false};
};
} // namespace guitarfx
