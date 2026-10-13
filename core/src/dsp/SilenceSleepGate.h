#pragma once

// SilenceSleepGate — lets the mixer stop running its chains while nothing is being played.
//
// A DAW keeps calling a plugin on a muted track or an empty one, and the standalone keeps
// calling it with the guitar's volume rolled off. Every chain still ran at full cost on that
// silence: NAM inference, convolution and every reverb tank on zeros. The gate watches the
// block's input and the chains' output, and once both have stayed quiet for long enough the
// mixer skips its chains and writes silence, until the input moves again.
//
// It only ever sleeps on silence it has heard: the output has to have been quiet for the
// whole hold as well as the input, so an amp turning input hiss into audible hiss, a
// reverb's tail or a delay's repeats all keep it awake. The hold is longer for a graph that
// can ring out, which covers the gap before a long delay's first repeat. Nothing is reset
// on sleeping or waking: every chain is left as it was, holding only what had already
// decayed below the threshold, and picks up from there on the block that wakes it.
//
// Audio thread only, apart from Prepare().

#include <algorithm>
#include <cmath>

namespace guitarfx
{
/// What a graph means for sleeping, worked out from its node types on the message thread.
struct GraphSleepTraits
{
    /// Has a time-based node (delay, reverb) that can go quiet and then sound again with
    /// no new input, so the gate holds off for longer before sleeping.
    bool canRingOut = false;
    /// Has a node that can sound, or must keep running, with silence at its input — an
    /// opaque hosted plugin or WASM module, or a node that sends MIDI — so never sleep.
    bool keepsAwake = false;
};

class SilenceSleepGate
{
  public:
    /// Input below this (about -80 dBFS) counts as nothing played. Lenient on purpose:
    /// the output test below is what proves the chains have nothing to say about it.
    static constexpr float kQuietInputPeak = 1.0e-4f;
    /// Output below this (about -96 dBFS, under 16-bit resolution) counts as silence.
    static constexpr float kQuietOutputPeak = 1.5e-5f;
    /// While asleep, input wakes the chains once it rises 6 dB above the loudest it was
    /// while the gate was deciding to sleep, or above this floor if that was true silence.
    static constexpr float kWakeFloorPeak = 1.0e-6f;
    static constexpr double kHoldSeconds = 1.0;
    static constexpr double kRingOutHoldSeconds = 5.0;

    void Prepare(double sampleRate)
    {
        mHoldSamples = static_cast<long long>(std::ceil(sampleRate * kHoldSeconds));
        mRingOutHoldSamples = static_cast<long long>(std::ceil(sampleRate * kRingOutHoldSeconds));
        Wake();
    }

    /// Called before the chains run. True to skip them this block. `allowed` false (the
    /// gate is off, a swap is mid-crossfade, a graph keeps it awake) wakes it and keeps it
    /// awake until it has heard a full hold of silence again.
    [[nodiscard]] bool ShouldSkip(float inputPeak, bool allowed) noexcept
    {
        if (!allowed)
        {
            Wake();
            return false;
        }

        if (!mAsleep)
        {
            return false;
        }

        if (inputPeak > mWakePeak)
        {
            Wake();
            return false;
        }

        return true;
    }

    /// Called after a block the chains ran, with that block's input and output peaks.
    void NoteProcessed(float inputPeak, float outputPeak, int numSamples, bool canRingOut) noexcept
    {
        if (inputPeak >= kQuietInputPeak || outputPeak >= kQuietOutputPeak)
        {
            mQuietSamples = 0;
            mQuietInputPeak = 0.0f;
            return;
        }

        mQuietSamples += numSamples;
        mQuietInputPeak = std::max(mQuietInputPeak, inputPeak);

        if (mQuietSamples >= (canRingOut ? mRingOutHoldSamples : mHoldSamples))
        {
            mAsleep = true;
            mWakePeak = std::max(kWakeFloorPeak, 2.0f * mQuietInputPeak);
        }
    }

    [[nodiscard]] bool IsAsleep() const noexcept
    {
        return mAsleep;
    }

    void Wake() noexcept
    {
        mAsleep = false;
        mQuietSamples = 0;
        mQuietInputPeak = 0.0f;
    }

    /// The largest absolute sample of up to two channels. Either may be null.
    [[nodiscard]] static float PeakOf(const float* left, const float* right, int numSamples) noexcept
    {
        float peak = 0.0f;

        for (const float* channel : {left, right})
        {
            if (channel == nullptr)
            {
                continue;
            }

            for (int i = 0; i < numSamples; ++i)
            {
                peak = std::max(peak, std::abs(channel[i]));
            }
        }

        return peak;
    }

  private:
    long long mHoldSamples = 48000;
    long long mRingOutHoldSamples = 240000;
    long long mQuietSamples = 0;
    float mQuietInputPeak = 0.0f;
    float mWakePeak = kWakeFloorPeak;
    bool mAsleep = false;
};
} // namespace guitarfx
