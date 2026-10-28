// LooperEngine: the block, the state machine's per-sample events and the two taps. See LooperEngine.h and
// docs/plans/looper.md.
//
// Positions. A writer counts the DI it records in `recAbs`, unwrapped. For an overdub that is the
// loop position plus whole loops; for the first layer it counts from the layer's start, which sits at
// `head` in the capture buffer. Index() turns either into a buffer index. The wet sound for a DI
// sample arrives one chain latency later, so a writer puts the wet sample it is handed at
// `recAbs - latency`.
//
// Time. The playhead is the loop position being played. An overdub records at `playhead - D`, where
// D is how much later a note played along with the output reaches the DI tap, so what is played in
// time with the loop lands in time with it.

#include "dsp/looper/LooperEngine.h"
#include "dsp/looper/LooperEngineInternal.h"

#include <algorithm>
#include <cmath>

namespace guitarfx::looper
{
using namespace detail;

LooperEngine::LooperEngine() = default;

void LooperEngine::Prepare(double sampleRate, int maxBlockSize)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;

    // A loop recorded at another rate would play at the wrong pitch; the service saves or drops
    // it before a rate change, and whatever is left goes.
    if (rate != mSampleRate)
    {
        FinishClear();
        mSpare = nullptr;
    }

    mSampleRate = rate;
    mMaxBlock = std::max(1, maxBlockSize);
    mRampSamples = ToSamples(kRampSeconds, rate);
    mPunchSamples = ToSamples(kPunchSeconds, rate);
    mPreRollSamples = ToSamples(kPreRollSeconds, rate);
    mRingOutSamples = ToSamples(kRingOutSeconds, rate);
    mQuietSamples = ToSamples(kRingOutQuietSeconds, rate);
    mArmFloorSamples = ToSamples(kArmFloorSeconds, rate);
    mMinLayerSamples = ToSamples(kMinLayerSeconds, rate);
    mSwapSamples = ToSamples(kSwapSeconds, rate);
    mDiL.assign(static_cast<std::size_t>(mMaxBlock), 0.0f);
    mDiR.assign(static_cast<std::size_t>(mMaxBlock), 0.0f);
    Publish();
}

bool LooperEngine::Enqueue(const LooperCommand& command)
{
    return mCommands.Push(&command, 1) == 1;
}

// ── Audio thread: the block ─────────────────────────────────────────────────────────────

// Both publish at once, so a caller holding the engine (riff capture, under the DSP lock) reads what
// its press did without waiting for a block.
void LooperEngine::Press(LooperAction action)
{
    ApplyAction(action, 0);
    Publish();
}

void LooperEngine::Apply(const LooperCommand& command)
{
    ApplyCommand(command);
    Publish();
}

void LooperEngine::NoteInput(const float* left, const float* right, int numSamples, float calibrationGain) noexcept
{
    const int n = std::min(numSamples, mMaxBlock);

    // Kept even while idle: a press between blocks starts a take with what the last block had.
    mDiChannels = left && right ? 2 : 1;
    mCalibrationGain = calibrationGain;

    // Nothing records this block unless a command still to be applied starts it.
    if (n <= 0 || (IsIdle() && mCommands.AvailableToRead() == 0))
    {
        return;
    }

    const float* l = left ? left : right;

    if (l)
    {
        std::copy_n(l, n, mDiL.data());
    }
    else
    {
        std::fill_n(mDiL.data(), n, 0.0f);
    }

    if (mDiChannels > 1)
    {
        std::copy_n(right, n, mDiR.data());
    }

    mDiNoted = true;
}

bool LooperEngine::Process(float* outL, float* outR, int numSamples, bool muted) noexcept
{
    const int n = std::min(numSamples, mMaxBlock);

    if (n <= 0)
    {
        return false;
    }

    // No loop and nothing armed: a session that never uses the looper pays for this check and no more.
    if (IsIdle())
    {
        bool applied = false;
        LooperCommand command;

        while (mCommands.Pop(&command, 1) == 1)
        {
            ApplyCommand(command);
            applied = true;
        }

        if (IsIdle())
        {
            mDiNoted = false;

            if (applied)
            {
                Publish();
            }

            mBlocksProcessed.fetch_add(1, std::memory_order_release);
            return false;
        }

        // A command started something: this block runs from its first sample, as it would have.
    }

    if (!mDiNoted)
    {
        std::fill_n(mDiL.data(), n, 0.0f);
        std::fill_n(mDiR.data(), n, 0.0f);
    }

    mDiNoted = false;
    BeginClickSync();
    mPlayFrom = mPlaying ? 0 : -1;
    mPlayheadAtPlayFrom = mPlayhead;

    for (auto& slot : mSlots)
    {
        slot.audibleFrom = 0;
    }

    LooperCommand command;

    while (mCommands.Pop(&command, 1) == 1)
    {
        ApplyCommand(command);
    }

    // A take started between blocks (a press) learns its DI's channels from the blocks it records.
    for (const auto& w : mWriters)
    {
        if (w.phase == WriterPhase::Armed || w.phase == WriterPhase::Recording)
        {
            Slot& slot = mSlots[static_cast<std::size_t>(w.slot)];
            slot.diChannels = std::max(slot.diChannels, std::min(mDiChannels, slot.audio->DiChannels()));
        }
    }

    // The tuner's muted mode: a take punches out, the loop keeps time silently.
    if (muted)
    {
        if (mState == LooperState::Recording)
        {
            ApplyAction(LooperAction::Play, 0);
        }
        else if (mState == LooperState::Overdubbing)
        {
            ApplyAction(LooperAction::Play, 0);
        }
    }

    for (int i = 0; i < n; ++i)
    {
        const float wetL = outL ? outL[i] : (outR ? outR[i] : 0.0f);
        const float wetR = outR ? outR[i] : wetL;
        StepSample(i, wetL, wetR, muted);
    }

    bool added = false;

    if (mPlayFrom >= 0 && mLength > 0 && !muted && mSettings.playbackEnabled.load(std::memory_order_relaxed) &&
        (outL || outR))
    {
        added = MixPlayback(outL, outR, n);
    }
    else
    {
        std::array<float, kMaxLayers> targetsL{};
        std::array<float, kMaxLayers> targetsR{};
        UpdateSlotTargets(targetsL.data(), targetsR.data());

        for (std::size_t s = 0; s < mSlots.size(); ++s)
        {
            mSlots[s].gainL = targetsL[s];
            mSlots[s].gainR = targetsR[s];
            // Nothing heard, so a swap's crossfade has nothing to smooth.
            mSlots[s].fadeFrom = nullptr;
            mSlots[s].fadeLeft = 0;
        }
    }

    // A stop or a clear finishes once every layer has faded out.
    bool silent = true;

    for (const auto& slot : mSlots)
    {
        if (slot.status != SlotStatus::Free && (slot.gainL > 0.0f || slot.gainR > 0.0f))
        {
            silent = false;
        }
    }

    if (silent && mClearing)
    {
        FinishClear();
    }
    else if (silent && mStopping)
    {
        mStopping = false;
        mPlaying = false;
        mPlayhead = 0;
    }

    for (std::size_t s = 0; s < mSlots.size(); ++s)
    {
        Slot& slot = mSlots[s];

        if (slot.freeWhenSilent && slot.gainL <= 0.0f && slot.gainR <= 0.0f)
        {
            ReleaseSlot(static_cast<int>(s), true);
        }
    }

    Publish();
    mBlocksProcessed.fetch_add(1, std::memory_order_release);
    return added;
}

void LooperEngine::StepSample(int i, float wetL, float wetR, bool muted)
{
    const float diL = mDiL[static_cast<std::size_t>(i)];
    const float diR = mDiChannels > 1 ? mDiR[static_cast<std::size_t>(i)] : diL;

    // The loop wrapped at the end of the last sample: the click goes back to the loop's start, which
    // keeps it in step with a loop that is not whole bars (a Bars loop is on bar 1 again).
    if (mWrapPending)
    {
        mWrapPending = false;

        if (const double bar = ClickBarAt(0); bar >= 0.0 && mClickSync.resetAt < 0)
        {
            mClickSync.resetAt = i;
            mClickSync.resetBar = bar;
        }
    }

    // A counted-in armed take starts listening on the downbeat.
    if (mPendingArm >= 0)
    {
        if (mPendingArm == 0)
        {
            mPendingArm = -1;
            mState = LooperState::Armed;
            mRingArmed = true;
            mRingWritten = 0;
            ResetArmDetector();
            Raise(kNoticeStateChanged);
        }
        else
        {
            --mPendingArm;
        }
    }

    // A count-in, or an armed take snapped to a bar line still ahead.
    if (mPendingStart >= 0)
    {
        if (mPendingStart == 0)
        {
            mPendingStart = -1;
            StartFirstWriter(i, 0);
            mState = LooperState::Recording;
            Raise(kNoticeStateChanged);
        }
        else
        {
            --mPendingStart;
        }
    }

    if (mCountInLeft > 0)
    {
        --mCountInLeft;
    }

    // An overdub asked for just before the record wrap opens on it.
    if (mDeferredOverdub && mPlaying && mLength > 0 && Wrap(mPlayhead - mAlign, mLength) == 0)
    {
        mDeferredOverdub = false;
        OpenOverdub(i, 0, false, false);
    }

    // A take that has reached its length.
    for (int wi = 0; wi < kMaxWriters; ++wi)
    {
        Writer& w = mWriters[static_cast<std::size_t>(wi)];

        if (w.phase != WriterPhase::Recording)
        {
            continue;
        }

        const Slot& slot = mSlots[static_cast<std::size_t>(w.slot)];

        if (slot.firstLayer)
        {
            const bool full = mTargetLength > 0 ? w.recAbs >= mTargetLength : w.recAbs >= slot.capacity;

            if (full)
            {
                const bool overdub = mFirstBars && mSettings.afterRecordOverdub.load(std::memory_order_relaxed);
                CloseFirstLayer(i, overdub ? LooperState::Overdubbing : LooperState::Playing);
            }
        }
        else if (mLength > 0 && w.recAbs > w.spanStartAbs && Wrap(w.recAbs, mLength) == 0)
        {
            // Overdub across the wrap: this pass is a layer, the next starts on the same sample.
            CloseOverdub(i, true);
        }
    }

    // Armed: the first note starts the take.
    const bool armedFirst = mState == LooperState::Armed && mPendingStart < 0;
    const bool armedOverdub = mState == LooperState::OverdubArmed;

    if ((armedFirst || armedOverdub) && !muted)
    {
        ++mArmWait;

        if (CheckArmTrigger(PeakOf(diL, diR)))
        {
            if (armedFirst)
            {
                TriggerFirstLayer(i);
            }
            else
            {
                TriggerArmedOverdub(i);
            }
        }
    }

    if (mRingArmed)
    {
        WriteArmedRing(diL, diR, wetL, wetR);
    }

    mAnyRecordingWet = false;

    for (const auto& w : mWriters)
    {
        if (w.phase == WriterPhase::Recording &&
            w.recAbs - mSlots[static_cast<std::size_t>(w.slot)].latency >= w.spanStartAbs)
        {
            mAnyRecordingWet = true;
        }
    }

    for (auto& w : mWriters)
    {
        if (w.phase != WriterPhase::Off)
        {
            WriteSample(w, diL, diR, wetL, wetR);
        }
    }

    if (mPlaying && mLength > 0 && ++mPlayhead >= mLength)
    {
        mPlayhead = 0;
        mWrapPending = true;
    }
}

void LooperEngine::WriteArmedRing(float diL, float diR, float wetL, float wetR)
{
    const LayerAudio& c = *mCapture;
    const auto di = static_cast<std::size_t>(mRingW);
    c.di[0][di] = diL;

    if (c.di[1])
    {
        c.di[1][di] = diR;
    }

    if (c.wet[0])
    {
        const auto wet = static_cast<std::size_t>(Wrap(mRingW - mRingLatency, c.capacity));
        c.wet[0][wet] = wetL;
        c.wet[1][wet] = wetR;
    }

    mRingW = mRingW + 1 == c.capacity ? 0 : mRingW + 1;
    ++mRingWritten;
}

void LooperEngine::WriteSample(Writer& w, float diL, float diR, float wetL, float wetR)
{
    Slot& slot = mSlots[static_cast<std::size_t>(w.slot)];
    const LayerAudio& a = *slot.audio;
    const float diRight = slot.diChannels > 1 ? diR : diL;

    // DI.
    if (w.phase == WriterPhase::Armed || w.phase == WriterPhase::Recording)
    {
        const auto idx = static_cast<std::size_t>(Index(slot, w.recAbs));
        const float g = (w.phase == WriterPhase::Recording && w.fadeIn) ? PunchRamp(w.recAbs - w.spanStartAbs) : 1.0f;
        a.di[0][idx] = diL * g;

        if (a.di[1])
        {
            a.di[1][idx] = diRight * g;
        }

        if (w.phase == WriterPhase::Recording)
        {
            NoteLivePeak(true, slot.firstLayer ? w.recAbs : Wrap(w.recAbs, mLength), PeakOf(diL, diRight));
            mLiveRecorded.store(w.recAbs - w.spanStartAbs + 1, std::memory_order_relaxed);
        }
    }
    else if (w.diSeamLeft > 0)
    {
        // The first layer's seam: what was played just after the close is faded onto its start,
        // so a re-amp runs on across the loop point instead of stepping.
        const auto idx = static_cast<std::size_t>(Index(slot, w.recAbs));
        const float g = static_cast<float>(w.diSeamLeft) / static_cast<float>(mPunchSamples + 1);
        a.di[0][idx] += diL * g;

        if (a.di[1])
        {
            a.di[1][idx] += diRight * g;
        }

        --w.diSeamLeft;
    }

    // Wet. A capture with none records the DI alone.
    const std::int64_t wAbs = w.recAbs - slot.latency;
    bool done = false;

    if (!a.wet[0])
    {
        done = w.phase == WriterPhase::Tail;
    }
    else if (w.phase == WriterPhase::Armed)
    {
        const auto idx = static_cast<std::size_t>(Index(slot, wAbs));
        a.wet[0][idx] = wetL;
        a.wet[1][idx] = wetR;
    }
    else if (wAbs >= w.spanStartAbs)
    {
        if (wAbs < w.spanEndAbs)
        {
            const auto idx = static_cast<std::size_t>(Index(slot, wAbs));
            const float g = w.fadeIn ? PunchRamp(wAbs - w.spanStartAbs) : 1.0f;
            a.wet[0][idx] = wetL * g;
            a.wet[1][idx] = wetR * g;

            if (w.phase == WriterPhase::Recording)
            {
                NoteLivePeak(false, slot.firstLayer ? wAbs : Wrap(wAbs, mLength), PeakOf(wetL, wetR) * g);
            }
        }
        else if (w.butted || w.ringOutCap <= 0)
        {
            done = true;
        }
        else
        {
            // The ring-out: the tail of the last notes, written on past the span (onto the first
            // layer's own start) until it dies away, the player plays on, or another layer takes it.
            const std::int64_t k = wAbs - w.spanEndAbs;
            const std::int64_t fadeLength = std::min(mPunchSamples, w.ringOutCap);

            if (!w.ringOutStopping)
            {
                w.quietRun = PeakOf(wetL, wetR) < kRingOutQuietPeak ? w.quietRun + 1 : 0;
                const bool playingOn = PeakOf(diL, diR) >= mSettings.armThreshold.load(std::memory_order_relaxed);

                if (w.quietRun >= mQuietSamples || playingOn || mAnyRecordingWet || k >= w.ringOutCap - fadeLength)
                {
                    w.ringOutStopping = true;
                }
            }

            float g = 1.0f;

            if (w.ringOutStopping)
            {
                g = static_cast<float>(fadeLength - 1 - w.ringOutFade) / static_cast<float>(fadeLength);
                done = ++w.ringOutFade >= fadeLength;
            }

            const auto idx = static_cast<std::size_t>(Index(slot, wAbs));

            if (w.ringOutAdds)
            {
                a.wet[0][idx] += wetL * g;
                a.wet[1][idx] += wetR * g;
            }
            else
            {
                a.wet[0][idx] = wetL * g;
                a.wet[1][idx] = wetR * g;
            }

            slot.ringOut = k + 1;
        }
    }

    ++w.recAbs;

    if (done && w.diSeamLeft <= 0)
    {
        w.phase = WriterPhase::Off;
        slot.final = true;
        Raise(kNoticeLayerFinal);
    }
}

void LooperEngine::NoteLivePeak(bool di, std::int64_t rel, float peak) noexcept
{
    const std::int64_t bucketSize = mLiveBucketSamples.load(std::memory_order_relaxed);
    const std::int64_t bucket =
        std::clamp<std::int64_t>(rel / std::max<std::int64_t>(1, bucketSize), 0, kPeakBuckets - 1);
    auto& cell = (di ? mLivePeaksDi : mLivePeaksWet)[static_cast<std::size_t>(bucket)];

    if (peak > cell.load(std::memory_order_relaxed))
    {
        cell.store(peak, std::memory_order_relaxed);
    }
}

void LooperEngine::ResetLive(std::int64_t layerSamples) noexcept
{
    for (auto& cell : mLivePeaksDi)
    {
        cell.store(0.0f, std::memory_order_relaxed);
    }

    for (auto& cell : mLivePeaksWet)
    {
        cell.store(0.0f, std::memory_order_relaxed);
    }

    mLiveBucketSamples.store(std::max<std::int64_t>(1, (layerSamples + kPeakBuckets - 1) / kPeakBuckets),
                             std::memory_order_relaxed);
    mLiveRecorded.store(0, std::memory_order_relaxed);
}

bool LooperEngine::CheckArmTrigger(float peak)
{
    if (mArmFloorLeft > 0)
    {
        --mArmFloorLeft;
        mArmFloor = std::max(mArmFloor, peak);

        if (mArmFloorLeft == 0)
        {
            mArmThreshold = std::max(mArmFloor * kArmOverFloor, mSettings.armThreshold.load(std::memory_order_relaxed));
        }

        // Plainly playing while the floor is still being measured.
        return peak >= kArmFloorTriggerPeak;
    }

    return peak >= mArmThreshold;
}

void LooperEngine::ResetArmDetector()
{
    mArmFloorLeft = mArmFloorSamples;
    mArmFloor = 0.0f;
    mArmThreshold = mSettings.armThreshold.load(std::memory_order_relaxed);
    mArmWait = 0;
}

std::int64_t LooperEngine::Index(const Slot& slot, std::int64_t abs) const noexcept
{
    const std::int64_t pos = mLength > 0 ? Wrap(abs, mLength) : abs;
    return Wrap(slot.head + pos, slot.capacity);
}

float LooperEngine::PunchRamp(std::int64_t rel) const noexcept
{
    return rel >= mPunchSamples ? 1.0f : static_cast<float>(rel) / static_cast<float>(mPunchSamples);
}

bool LooperEngine::IsIdle() const noexcept
{
    return mState == LooperState::Empty && !mPlaying && !mClearing && !mRingArmed && mPendingStart < 0 &&
           mPendingArm < 0 &&
           std::none_of(mWriters.begin(), mWriters.end(), [](const Writer& w) { return w.phase != WriterPhase::Off; });
}

bool LooperEngine::WantsChainsAwake() const noexcept
{
    if (mState == LooperState::CountIn || mState == LooperState::Armed || mState == LooperState::Recording ||
        mState == LooperState::OverdubArmed || mState == LooperState::Overdubbing)
    {
        return true;
    }

    return std::any_of(mWriters.begin(), mWriters.end(), [](const Writer& w) { return w.phase != WriterPhase::Off; });
}
} // namespace guitarfx::looper
