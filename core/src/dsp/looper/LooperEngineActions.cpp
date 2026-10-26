// LooperEngine: what a press does, and how layers open, close, undo and go. See LooperEngine.h.
// Everything here runs on the audio thread, at a sample of the block (`at`): a press at the block's
// first, a take that reaches its length or the loop's wrap wherever that falls.

#include "dsp/looper/LooperEngine.h"

#include "dsp/looper/LooperEngineInternal.h"

#include <algorithm>
#include <cmath>

namespace guitarfx::looper
{
using namespace detail;

namespace
{
bool StartsTake(LooperAction action) noexcept
{
    return action == LooperAction::RecPlayDub || action == LooperAction::Record || action == LooperAction::Overdub;
}
} // namespace

void LooperEngine::ApplyCommand(const LooperCommand& command)
{
    switch (command.type)
    {
    case LooperCommand::Type::Action:
        ApplyAction(command.action, 0);
        break;
    case LooperCommand::Type::SetCapture:
    {
        // Only while nothing records into, or plays from, the one there now. The service sees
        // the published capture and asks again if this was refused.
        const bool inUse = mRingArmed || mPendingStart >= 0 || mPendingArm >= 0 ||
                           std::any_of(mSlots.begin(), mSlots.end(), [this](const Slot& s) {
                               return s.status != SlotStatus::Free && s.audio == mCapture;
                           });

        if (!inUse)
        {
            mCapture = command.audio;
            mRingW = 0;
            mRingWritten = 0;
        }

        break;
    }
    case LooperCommand::Type::SetSpare:
        mSpare = command.audio;
        break;
    case LooperCommand::Type::RemoveLayer:
        RemoveLayer(command.slot);
        break;
    case LooperCommand::Type::DropRedo:
        ClearRedo();
        break;
    case LooperCommand::Type::ReplaceAudio:
        ReplaceAudio(command);
        break;
    case LooperCommand::Type::LoadLoop:
        LoadLoop(command.loop);
        break;
    case LooperCommand::Type::AddLayer:
        AddLayer(command.loop);
        break;
    }
}

void LooperEngine::ApplyAction(LooperAction action, int at)
{
    // A record press during a clear's fade does not wait for it.
    if (mClearing && StartsTake(action))
    {
        FinishClear();
    }

    switch (mState)
    {
    case LooperState::Empty:
        if (action == LooperAction::RecPlayDub || action == LooperAction::Record)
        {
            StartFirstLayer(at);
        }
        else if (action == LooperAction::Undo)
        {
            Undo();
        }
        else if (action == LooperAction::Redo)
        {
            Redo();
        }
        else if (action == LooperAction::Clear)
        {
            BeginClear();
        }

        break;

    case LooperState::CountIn:
    case LooperState::Armed:
        if (StartsTake(action) && mState == LooperState::Armed && mPendingStart < 0)
        {
            // A press while Armed starts the take at once.
            StartFirstWriter(at, 0);
            mState = LooperState::Recording;
        }
        else if (action == LooperAction::Stop || action == LooperAction::Undo || action == LooperAction::Clear)
        {
            CancelFirstLayer();
        }

        break;

    case LooperState::Recording:
        if (action == LooperAction::Stop)
        {
            BeginStop();
        }
        else if (action == LooperAction::Undo || action == LooperAction::Clear)
        {
            CancelFirstLayer();
        }
        else if (!mFirstBars)
        {
            // A Bars take closes itself on its last bar; a free one on this press.
            if (action == LooperAction::RecPlayDub)
            {
                const bool overdub = mSettings.afterRecordOverdub.load(std::memory_order_relaxed);
                CloseFirstLayer(at, overdub ? LooperState::Overdubbing : LooperState::Playing);
            }
            else if (action == LooperAction::Record || action == LooperAction::Overdub)
            {
                CloseFirstLayer(at, LooperState::Overdubbing);
            }
            else if (action == LooperAction::Play)
            {
                CloseFirstLayer(at, LooperState::Playing);
            }
        }

        break;

    case LooperState::Playing:
        if (StartsTake(action))
        {
            RequestOverdub(at);
        }
        else if (action == LooperAction::Stop)
        {
            BeginStop();
        }
        else if (action == LooperAction::Undo)
        {
            Undo();
        }
        else if (action == LooperAction::Redo)
        {
            Redo();
        }
        else if (action == LooperAction::Clear)
        {
            BeginClear();
        }

        break;

    case LooperState::OverdubArmed:
        if (StartsTake(action))
        {
            TriggerArmedOverdub(at);
        }
        else if (action == LooperAction::Play || action == LooperAction::Undo)
        {
            DiscardRecording();
            mState = LooperState::Playing;
        }
        else if (action == LooperAction::Stop)
        {
            BeginStop();
        }
        else if (action == LooperAction::Clear)
        {
            BeginClear();
        }

        break;

    case LooperState::Overdubbing:
        if (action == LooperAction::RecPlayDub || action == LooperAction::Play)
        {
            if (mDeferredOverdub)
            {
                mDeferredOverdub = false;
                mState = LooperState::Playing;
            }
            else
            {
                CloseOverdub(at, false);
            }
        }
        else if (action == LooperAction::Undo)
        {
            mDeferredOverdub = false;
            DiscardRecording();
            mState = LooperState::Playing;
        }
        else if (action == LooperAction::Stop)
        {
            BeginStop();
        }
        else if (action == LooperAction::Clear)
        {
            BeginClear();
        }

        break;

    case LooperState::Stopped:
        if (action == LooperAction::RecPlayDub || action == LooperAction::Play || action == LooperAction::Record ||
            action == LooperAction::Overdub)
        {
            // From the top, even while the stop is still fading.
            mStopping = false;
            mPlaying = false;
            StartPlaying(at, 0);
            mState = LooperState::Playing;

            if (action == LooperAction::Record || action == LooperAction::Overdub)
            {
                RequestOverdub(at);
            }
        }
        else if (action == LooperAction::Undo)
        {
            Undo();
        }
        else if (action == LooperAction::Redo)
        {
            Redo();
        }
        else if (action == LooperAction::Clear)
        {
            BeginClear();
        }

        break;
    }

    Raise(kNoticeStateChanged);
}

// ── The first layer ─────────────────────────────────────────────────────────────────────

void LooperEngine::StartFirstLayer(int at)
{
    // A new loop: whatever the last one left behind (layers undone back to Empty) goes.
    FinishClear();

    const bool playback = mSettings.playbackEnabled.load(std::memory_order_relaxed);

    if (!mCapture || mCapture->capacity <= 2 * mMinLayerSamples || !mCapture->di[0] ||
        (playback && (!mCapture->wet[0] || !mCapture->wet[1])))
    {
        Raise(kNoticeNoCapture);
        return;
    }

    mFirstBars = mSettings.lengthBars.load(std::memory_order_relaxed);
    mTargetLength = 0;
    mAlign = std::max(0, mSettings.alignSamples.load(std::memory_order_relaxed));
    mRingLatency = std::max(0, mSettings.chainLatencySamples.load(std::memory_order_relaxed));
    const double samplesPerBar = mClock ? mClock->samplesPerBar.load(std::memory_order_relaxed) : 0.0;

    if (mFirstBars)
    {
        if (samplesPerBar <= 1.0)
        {
            Raise(kNoticeNoTempo);
            return;
        }

        const int bars = std::max(1, mSettings.bars.load(std::memory_order_relaxed));
        mTargetLength = static_cast<std::int64_t>(std::llround(static_cast<double>(bars) * samplesPerBar));

        if (mTargetLength > mCapture->capacity)
        {
            mTargetLength = 0;
            Raise(kNoticeTooLong);
            return;
        }
    }

    const bool armed = mSettings.startArmed.load(std::memory_order_relaxed);
    const bool everyTake = mSettings.countInEveryTake.load(std::memory_order_relaxed);
    const int countInBars = std::clamp(mSettings.countInBars.load(std::memory_order_relaxed), 0, 16);
    const std::int64_t countIn =
        samplesPerBar > 1.0
            ? static_cast<std::int64_t>(std::llround(static_cast<double>(countInBars) * samplesPerBar))
            : 0;
    mPublishedArmLead.store(0, std::memory_order_relaxed);
    mClosePlayhead = 0;

    if (armed && everyTake && countIn > 0)
    {
        // Counted in, then listening from the downbeat.
        mClickSync.resetAt = at;
        mClickSync.resetBar = 0.0;
        mCountInLeft = countIn;
        mPendingArm = countIn;
        mState = LooperState::CountIn;
    }
    else if (armed)
    {
        mState = LooperState::Armed;
        mRingArmed = true;
        mRingWritten = 0;
        mPendingStart = -1;
        ResetArmDetector();
    }
    else if (mFirstBars || (everyTake && countIn > 0))
    {
        // The press restarts the metronome's bar, counts in, and records from the downbeat; the
        // record window sits D later, where a note played on that downbeat arrives.
        mClickSync.resetAt = at;
        mClickSync.resetBar = 0.0;
        mCountInLeft = countIn;
        mClosePlayhead = mAlign;
        mPendingStart = countIn + mAlign;
        mState = LooperState::CountIn;
    }
    else
    {
        StartFirstWriter(at, 0);
        mState = LooperState::Recording;
    }
}

void LooperEngine::StartFirstWriter(int at, std::int64_t recAbs)
{
    // Where the loop starts in the metronome's bar: the grid as the player heard it when they played
    // its first sample, D before it reached the DI tap (bar 1 for a counted-in take, the snapped beat
    // for an armed one). A take played to the click is played back on it: its playhead starts D in.
    mClickBar = 0.0;

    if (mClock && mClock->running.load(std::memory_order_relaxed))
    {
        const double samplesPerBar = mClock->samplesPerBar.load(std::memory_order_relaxed);

        if (samplesPerBar > 1.0)
        {
            const double since = mClock->samplesSinceBar.load(std::memory_order_relaxed) + static_cast<double>(at) -
                                 static_cast<double>(recAbs) - static_cast<double>(mAlign);
            const double bar = since / samplesPerBar;
            mClickBar = bar - std::floor(bar);
            mClosePlayhead = mAlign;
        }
    }

    const int slotIndex = FindFreeSlot();
    Writer* w = NewWriter();

    if (slotIndex < 0 || !w)
    {
        Raise(kNoticeNoSlot);
        CancelFirstLayer();
        return;
    }

    Slot& slot = mSlots[static_cast<std::size_t>(slotIndex)];
    slot = Slot{};
    slot.status = SlotStatus::Recording;
    slot.audio = mCapture;
    slot.capacity = mCapture->capacity;
    slot.head = Wrap(mRingW - recAbs, mCapture->capacity);
    slot.serial = mNextSerial++;
    slot.latency = mRingLatency;
    slot.calibrationGain = mCalibrationGain;
    slot.diChannels = std::min(mDiChannels, mCapture->DiChannels());
    slot.firstLayer = true;

    *w = Writer{};
    w->phase = WriterPhase::Recording;
    w->slot = slotIndex;
    w->recAbs = recAbs;
    w->spanStartAbs = 0;
    w->spanEndAbs = kOpenEnd;
    w->ringOutAdds = true;
    mRingArmed = false;
    mPendingStart = -1;

    // The waveform so far, pre-roll included.
    ResetLive(mTargetLength > 0 ? mTargetLength : slot.capacity);

    for (std::int64_t k = 0; k < recAbs; ++k)
    {
        const auto idx = static_cast<std::size_t>(Index(slot, k));
        const float di = std::max(std::abs(mCapture->di[0][idx]), mCapture->di[1] ? std::abs(mCapture->di[1][idx]) : 0.0f);
        NoteLivePeak(true, k, di);
    }
}

void LooperEngine::TriggerFirstLayer(int at)
{
    std::int64_t back = std::min(mPreRollSamples, mRingWritten);
    mClosePlayhead = 0;

    // With the click running, the take starts on the metronome beat nearest the first note, on the
    // grid as the player hears it (D later at the DI tap): back to it for a note played late, waiting
    // for it for one played early. A Bars take is whole bars from there; the loop keeps which beat
    // it starts on (clickBar), and the click follows it.
    if (mSettings.armedOnTheBeat.load(std::memory_order_relaxed) && mClock &&
        mClock->running.load(std::memory_order_relaxed))
    {
        const double samplesPerBeat = mClock->samplesPerBeat.load(std::memory_order_relaxed);

        if (samplesPerBeat > 1.0)
        {
            const double since =
                mClock->samplesSinceBar.load(std::memory_order_relaxed) + static_cast<double>(at) - mAlign;
            const double x = std::fmod(std::fmod(since, samplesPerBeat) + samplesPerBeat, samplesPerBeat);
            const auto behind = static_cast<std::int64_t>(std::llround(x));
            const auto ahead = static_cast<std::int64_t>(std::llround(samplesPerBeat - x));
            mClosePlayhead = mTargetLength > 0 ? Wrap(mAlign, mTargetLength) : mAlign;

            if (x <= 0.5 * samplesPerBeat && behind <= mRingWritten)
            {
                back = behind;
            }
            else if (ahead > 0 && (mTargetLength == 0 || ahead < mTargetLength))
            {
                // Counted from the next sample: this one's countdown has already run.
                mPendingStart = ahead - 1;
                mPublishedArmLead.store(mRingWritten + ahead, std::memory_order_relaxed);
                return;
            }
            else
            {
                back = 0;
            }
        }
    }

    mPublishedArmLead.store(mRingWritten - back, std::memory_order_relaxed);
    StartFirstWriter(at, back);
    mState = LooperState::Recording;
    Raise(kNoticeStateChanged);
}

void LooperEngine::CloseFirstLayer(int at, LooperState next)
{
    Writer* w = RecordingWriter();

    if (!w || !mSlots[static_cast<std::size_t>(w->slot)].firstLayer)
    {
        return;
    }

    Slot& slot = mSlots[static_cast<std::size_t>(w->slot)];
    const std::int64_t length = w->recAbs;

    if (length < mMinLayerSamples)
    {
        CancelFirstLayer();
        Raise(kNoticeTooShort);
        return;
    }

    mLength = length;
    slot.status = SlotStatus::Visible;
    slot.spanStart = 0;
    slot.spanLength = length;
    slot.audibleFrom = at;
    w->phase = WriterPhase::Tail;
    w->spanEndAbs = length;
    w->ringOutCap = std::min(mRingOutSamples, length);
    w->diSeamLeft = std::min(mPunchSamples, length);
    mBarsLoop = mFirstBars;
    mHistory[static_cast<std::size_t>(mHistoryCount++)] = {EntryKind::Recorded, w->slot};
    SnapGains(w->slot);
    Raise(kNoticeLayerClosed);

    // Never played (a riff take): nothing to ring out into, nothing to smooth at the seam.
    if (!mSettings.playbackEnabled.load(std::memory_order_relaxed))
    {
        w->butted = true;
        w->diSeamLeft = 0;
        next = LooperState::Stopped;
    }

    if (next == LooperState::Stopped)
    {
        mState = LooperState::Stopped;
        mPlaying = false;
        mPlayhead = 0;
        return;
    }

    StartPlaying(at, mClosePlayhead);
    mState = LooperState::Playing;

    if (next == LooperState::Overdubbing)
    {
        RequestOverdub(at);

        // An overdub opened on this very sample records the tail itself.
        if (RecordingWriter())
        {
            w->butted = true;
        }
    }
}

void LooperEngine::CancelFirstLayer()
{
    if (Writer* w = RecordingWriter(); w && mSlots[static_cast<std::size_t>(w->slot)].firstLayer)
    {
        const int slot = w->slot;
        ReleaseSlot(slot, true);
    }

    mRingArmed = false;
    mPendingStart = -1;
    mPendingArm = -1;
    mCountInLeft = 0;
    mTargetLength = 0;
    mState = mLength > 0 ? (mPlaying ? LooperState::Playing : LooperState::Stopped) : LooperState::Empty;
}

// ── Overdubs ────────────────────────────────────────────────────────────────────────────

void LooperEngine::RequestOverdub(int at)
{
    if (mLength <= 0)
    {
        return;
    }

    if (!mPlaying)
    {
        StartPlaying(at, 0);
    }

    mAlign = std::max(0, mSettings.alignSamples.load(std::memory_order_relaxed));
    const bool armed = mSettings.startArmed.load(std::memory_order_relaxed);
    const std::int64_t recordPos = Wrap(mPlayhead - mAlign, mLength);

    // Too close to the wrap to be worth a layer of its own: it opens on the wrap.
    if (!armed && mLength - recordPos < mMinLayerSamples && mLength > 2 * mMinLayerSamples)
    {
        mDeferredOverdub = true;
        mState = LooperState::Overdubbing;
        return;
    }

    OpenOverdub(at, recordPos, true, armed);
}

bool LooperEngine::OpenOverdub(int at, std::int64_t recordPos, bool fadeIn, bool armed)
{
    if (!armed)
    {
        ClearRedo();
    }

    const int slotIndex = FindFreeSlot();
    Writer* w = slotIndex >= 0 ? NewWriter() : nullptr;

    if (!w)
    {
        Raise(kNoticeNoSlot);
        mState = LooperState::Playing;
        return false;
    }

    // At least the loop's length: the spare kept before the length is known is capture-sized.
    if (!mSpare || mSpare->capacity < mLength || !mSpare->di[0] || !mSpare->wet[0] || !mSpare->wet[1])
    {
        Raise(kNoticeNoSpare);
        mState = LooperState::Playing;
        return false;
    }

    Slot& slot = mSlots[static_cast<std::size_t>(slotIndex)];
    slot = Slot{};
    slot.status = SlotStatus::Recording;
    slot.audio = mSpare;
    slot.capacity = mLength;
    slot.head = 0;
    slot.serial = mNextSerial++;
    slot.latency = std::max(0, mSettings.chainLatencySamples.load(std::memory_order_relaxed));
    slot.calibrationGain = mCalibrationGain;
    slot.diChannels = std::min(mDiChannels, mSpare->DiChannels());
    slot.audibleFrom = at;
    mSpare = nullptr;
    Raise(kNoticeSpareTaken);

    *w = Writer{};
    w->slot = slotIndex;
    w->recAbs = recordPos;
    w->armStartAbs = recordPos;
    w->spanStartAbs = recordPos;
    w->spanEndAbs = kOpenEnd;
    w->fadeIn = fadeIn;

    if (armed)
    {
        w->phase = WriterPhase::Armed;
        ResetArmDetector();
        mState = LooperState::OverdubArmed;
    }
    else
    {
        w->phase = WriterPhase::Recording;
        ResetLive(mLength);
        mState = LooperState::Overdubbing;
    }

    return true;
}

void LooperEngine::TriggerArmedOverdub(int at)
{
    (void)at;
    Writer* w = nullptr;

    for (auto& candidate : mWriters)
    {
        if (candidate.phase == WriterPhase::Armed)
        {
            w = &candidate;
        }
    }

    if (!w)
    {
        return;
    }

    Slot& slot = mSlots[static_cast<std::size_t>(w->slot)];
    const std::int64_t cycleStart = w->recAbs - Wrap(w->recAbs, mLength);
    const std::int64_t start = std::max({w->recAbs - mPreRollSamples, cycleStart, w->armStartAbs});
    w->spanStartAbs = start;
    w->phase = WriterPhase::Recording;
    w->fadeIn = true;
    ClearRedo();
    ResetLive(mLength);

    // The pre-roll is already written: give it the punch-in ramp now.
    const LayerAudio& a = *slot.audio;

    for (std::int64_t abs = start; abs < w->recAbs; ++abs)
    {
        const auto idx = static_cast<std::size_t>(Index(slot, abs));
        const float g = PunchRamp(abs - start);

        for (float* channel : {a.di[0], a.di[1]})
        {
            if (channel)
            {
                channel[idx] *= g;
            }
        }

        if (abs < w->recAbs - slot.latency)
        {
            a.wet[0][idx] *= g;
            a.wet[1][idx] *= g;
        }
    }

    mState = LooperState::Overdubbing;
    Raise(kNoticeStateChanged);
}

void LooperEngine::CloseOverdub(int at, bool butted)
{
    Writer* w = RecordingWriter();

    if (!w || mSlots[static_cast<std::size_t>(w->slot)].firstLayer)
    {
        mState = LooperState::Playing;
        return;
    }

    Slot& slot = mSlots[static_cast<std::size_t>(w->slot)];
    const std::int64_t spanLength = w->recAbs - w->spanStartAbs;

    if (spanLength < mMinLayerSamples)
    {
        DiscardRecording();
        mState = LooperState::Playing;

        if (butted)
        {
            OpenOverdub(at, Wrap(w->recAbs, mLength), false, false);
        }

        return;
    }

    slot.status = SlotStatus::Visible;
    slot.spanStart = Wrap(w->spanStartAbs, mLength);
    slot.spanLength = spanLength;
    slot.audibleFrom = at;
    w->spanEndAbs = w->recAbs;
    w->phase = WriterPhase::Tail;
    mHistory[static_cast<std::size_t>(mHistoryCount++)] = {EntryKind::Recorded, w->slot};
    SnapGains(w->slot);
    Raise(kNoticeLayerClosed);

    // Held across the wrap: the next pass is a new layer, joined on this sample, and it records the
    // tail itself. If it cannot open, this one punches out as a press would.
    if (butted && OpenOverdub(at, Wrap(w->recAbs, mLength), false, false))
    {
        w->butted = true;
        return;
    }

    // Punched out: the DI ramps down; the wet sound rings on past the span instead.
    const LayerAudio& a = *slot.audio;
    const std::int64_t ramp = std::min(mPunchSamples, spanLength);

    for (std::int64_t k = 0; k < ramp; ++k)
    {
        const auto idx = static_cast<std::size_t>(Index(slot, w->spanEndAbs - 1 - k));
        const float g = static_cast<float>(k) / static_cast<float>(mPunchSamples);

        for (float* channel : {a.di[0], a.di[1]})
        {
            if (channel)
            {
                channel[idx] *= g;
            }
        }
    }

    w->ringOutCap = std::min(mRingOutSamples, mLength - spanLength);
    mState = LooperState::Playing;
}

void LooperEngine::DiscardRecording()
{
    for (auto& w : mWriters)
    {
        if (w.phase != WriterPhase::Armed && w.phase != WriterPhase::Recording)
        {
            continue;
        }

        const int slotIndex = w.slot;
        Slot& slot = mSlots[static_cast<std::size_t>(slotIndex)];

        if (slot.firstLayer)
        {
            CancelFirstLayer();
            continue;
        }

        // The buffer goes back as the spare: nothing outside a span is ever played.
        if (!mSpare && slot.audio && slot.audio->capacity >= mLength)
        {
            mSpare = slot.audio;
        }

        w.phase = WriterPhase::Off;
        slot = Slot{};
    }
}

} // namespace guitarfx::looper
