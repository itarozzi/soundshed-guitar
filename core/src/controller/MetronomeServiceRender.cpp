// MetronomeService: the audio thread's half, the click itself (Render) and the bar grid it
// publishes for the looper. See MetronomeService.h.

#include "controller/MetronomeService.h"

#include "IPluginHost.h"
#include "controller/internal/ControllerUtils.h"
#include "controller/internal/MetronomeSupport.h"

#include <algorithm>
#include <cmath>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
/// Pitch offset for the synthesised fallback beep, so accents still read as
/// accents on an install whose samples are missing.
double FallbackClickFrequency(ClickVoice voice)
{
    switch (voice)
    {
    case ClickVoice::High:
        return kMetronomeClickFrequencyHz * 1.5;
    case ClickVoice::Sub:
        return kMetronomeClickFrequencyHz * 1.25;
    case ClickVoice::Low:
    default:
        return kMetronomeClickFrequencyHz;
    }
}
} // namespace

void MetronomeService::Render(float** outputs, int numSamples)
{
    if (!outputs || !outputs[0] || !outputs[1])
    {
        return;
    }

    if (!mHost.IsStandalone())
    {
        return;
    }

    const bool guidanceActive = mGuidanceActive;
    const bool running = guidanceActive || mEnabled.load(std::memory_order_relaxed);
    const auto plan =
        guidanceActive ? mGuidanceBarPlan : std::atomic_load_explicit(&mBarPlan, std::memory_order_acquire);
    const double sampleRate = mHost.GetSampleRate();

    if (!plan || plan->beats.empty() || sampleRate <= 0.0)
    {
        mClock.running.store(false, std::memory_order_relaxed);
        return;
    }

    const double bpm =
        guidanceActive ? ClampValue(mGuidanceBpm, kMetronomeMinBpm, kMetronomeMaxBpm) : EffectiveTempoBpm();
    const auto beatsPerBar = static_cast<int>(plan->beats.size());
    const int ticksPerBeat = std::max(1, plan->ticksPerBeat);
    const double samplesPerBeat = sampleRate * (60.0 / std::max(1.0, bpm)) * std::max(0.125, plan->beatScale);
    const double samplesPerTick = samplesPerBeat / static_cast<double>(ticksPerBeat);
    const double samplesPerBar = samplesPerBeat * static_cast<double>(beatsPerBar);

    // The tempo is published whether or not the click runs: a Bars loop is sized by it.
    mClock.samplesPerBeat.store(samplesPerBeat, std::memory_order_relaxed);
    mClock.samplesPerBar.store(samplesPerBar, std::memory_order_relaxed);

    if (!running)
    {
        mClock.running.store(false, std::memory_order_relaxed);
        return;
    }

    // A restart lands on the sample it was asked for; one past this block waits for the next.
    int resetAt = -1;

    if (mResetPending.exchange(false, std::memory_order_acq_rel))
    {
        const int offset = mResetOffset.load(std::memory_order_relaxed);

        if (offset >= numSamples)
        {
            RequestReset(offset - numSamples);
        }
        else
        {
            resetAt = offset;
        }
    }

    if (resetAt == 0)
    {
        ResetCursor();
    }
    const int clickSamples = std::max(1, static_cast<int>(sampleRate * kMetronomeClickSeconds));

    const double volumeDb =
        ClampValue(mVolumeDb.load(std::memory_order_relaxed), kMetronomeMinVolumeDb, kMetronomeMaxVolumeDb);
    const double volume = ClampValue(LinearFromDb(volumeDb), 0.0, LinearFromDb(kMetronomeMaxVolumeDb));
    const double pan = ClampValue(mPan.load(std::memory_order_relaxed), -1.0, 1.0);
    const double panAngle = (pan + 1.0) * (kTwoPi / 8.0);
    const double panLeft = std::cos(panAngle);
    const double panRight = std::sin(panAngle);

    const auto clickSampleSet =
        guidanceActive ? mGuidanceClickSamples : std::atomic_load_explicit(&mClickSamples, std::memory_order_acquire);
    const bool hasSampleClick = clickSampleSet && !clickSampleSet->Empty();

    const float audible = mClickSuppressed.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
    const auto gainLeft = static_cast<float>(volume * panLeft) * audible;
    const auto gainRight = static_cast<float>(volume * panRight) * audible;

    for (int frame = 0; frame < numSamples; ++frame)
    {
        if (frame == resetAt && frame > 0)
        {
            ResetCursor();
        }

        if (mSamplesUntilTick <= 0.0)
        {
            const bool onBeat = (mTickIndex == 0);
            const int beatIndex = std::min(mBeatIndex, beatsPerBar - 1);
            const auto& beat = plan->beats[static_cast<std::size_t>(beatIndex)];
            const bool silent = onBeat && beat.silent;

            mTickVoice = onBeat ? beat.voice : ClickVoice::Sub;
            mTickGain = onBeat ? beat.gain : plan->subGain;

            if (onBeat)
            {
                mBeatPulse.store(PackBeatPulse(++mBeatPulseSeq, beatIndex, beatsPerBar, beat.level),
                                 std::memory_order_release);
            }

            if (silent)
            {
                mClickSamplesRemaining = 0;
            }
            else if (hasSampleClick)
            {
                const auto* selected = clickSampleSet->Voice(mTickVoice);
                mClickSamplesRemaining = selected ? static_cast<int>(selected->front().size()) : 0;
                mClickSamplePosition = 0;
            }
            else
            {
                mClickSamplesRemaining = clickSamples;
                mClickPhase = 0.0;
                mClickPhaseIncrement = kTwoPi * FallbackClickFrequency(mTickVoice) / sampleRate;
            }

            ++mTickIndex;

            if (mTickIndex >= ticksPerBeat)
            {
                mTickIndex = 0;
                mBeatIndex = (beatIndex + 1) % beatsPerBar;
            }

            mSamplesUntilTick += samplesPerTick;
        }

        float clickSampleL = 0.0f;
        float clickSampleR = 0.0f;

        if (mClickSamplesRemaining > 0)
        {
            if (hasSampleClick)
            {
                const auto* selected = clickSampleSet->Voice(mTickVoice);

                if (selected != nullptr)
                {
                    const auto index = static_cast<std::size_t>(mClickSamplePosition);

                    if (mClickSamplePosition >= 0 && index < selected->front().size())
                    {
                        clickSampleL = (*selected)[0][index];
                        clickSampleR = selected->size() > 1 ? (*selected)[1][index] : clickSampleL;
                    }
                }

                ++mClickSamplePosition;
                --mClickSamplesRemaining;
            }
            else
            {
                const double envelope = static_cast<double>(mClickSamplesRemaining) / static_cast<double>(clickSamples);
                const auto clickSample = static_cast<float>(std::sin(mClickPhase) * envelope);
                clickSampleL = clickSample;
                clickSampleR = clickSample;
                mClickPhase += mClickPhaseIncrement;

                if (mClickPhase >= kTwoPi)
                {
                    mClickPhase -= kTwoPi;
                }

                --mClickSamplesRemaining;
            }

            clickSampleL *= mTickGain;
            clickSampleR *= mTickGain;
        }

        outputs[0][frame] += clickSampleL * gainLeft;
        outputs[1][frame] += clickSampleR * gainRight;
        mSamplesUntilTick -= 1.0;
    }

    // Where the next block starts in the bar: the next tick's place, less the time until it.
    const double nextTick = static_cast<double>(mBeatIndex * ticksPerBeat + mTickIndex) * samplesPerTick;
    const double since = std::fmod(std::fmod(nextTick - mSamplesUntilTick, samplesPerBar) + samplesPerBar, samplesPerBar);
    mClock.samplesSinceBar.store(since, std::memory_order_relaxed);
    mClock.running.store(true, std::memory_order_relaxed);
}

void MetronomeService::ResetCursor()
{
    mSamplesUntilTick = 0.0;
    mClickSamplesRemaining = 0;
    mClickPhase = 0.0;
    mBeatIndex = 0;
    mTickIndex = 0;
    mClickSamplePosition = 0;
    mTickVoice = ClickVoice::Low;
    mTickGain = 1.0f;
}
} // namespace guitarfx
