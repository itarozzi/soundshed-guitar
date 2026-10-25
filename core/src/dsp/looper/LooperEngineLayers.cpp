// LooperEngine: the transport, undo and redo, the slots layers live in, and swapping a layer's audio
// or loading a whole loop into them. See LooperEngine.h and docs/plans/looper.md.

#include "dsp/looper/LooperEngine.h"
#include "dsp/looper/LooperEngineInternal.h"

#include <algorithm>

namespace guitarfx::looper
{
using namespace detail;

// ── Transport ───────────────────────────────────────────────────────────────────────────

void LooperEngine::StartPlaying(int at, std::int64_t playhead)
{
    if (mPlaying && !mStopping)
    {
        return;
    }

    mPlaying = true;
    mStopping = false;
    mPlayhead = mLength > 0 ? Wrap(playhead, mLength) : 0;
    mPlayFrom = at;
    mPlayheadAtPlayFrom = mPlayhead;
}

void LooperEngine::BeginStop()
{
    switch (mState)
    {
    case LooperState::Recording:
        if (mFirstBars)
        {
            CancelFirstLayer();
        }
        else
        {
            CloseFirstLayer(0, LooperState::Stopped);
        }

        return;
    case LooperState::CountIn:
    case LooperState::Armed:
        CancelFirstLayer();
        return;
    case LooperState::OverdubArmed:
        DiscardRecording();
        break;
    case LooperState::Overdubbing:
        if (mDeferredOverdub)
        {
            mDeferredOverdub = false;
        }
        else
        {
            CloseOverdub(0, false);
        }

        break;
    default:
        break;
    }

    if (mPlaying)
    {
        mStopping = true;
    }

    mState = mLength > 0 && VisibleCount() > 0 ? LooperState::Stopped : LooperState::Empty;
}

void LooperEngine::BeginClear()
{
    for (auto& w : mWriters)
    {
        w.phase = WriterPhase::Off;
    }

    mRingArmed = false;
    mPendingStart = -1;
    mPendingArm = -1;
    mCountInLeft = 0;
    mDeferredOverdub = false;

    if (mPlaying)
    {
        mClearing = true;
        mState = LooperState::Empty;
        return;
    }

    FinishClear();
}

void LooperEngine::FinishClear()
{
    for (int s = 0; s < kMaxLayers; ++s)
    {
        ReleaseSlot(s, true);
    }

    mHistoryCount = 0;
    mRedoCount = 0;
    mLength = 0;
    mPlayhead = 0;
    mPlaying = false;
    mStopping = false;
    mClearing = false;
    mBarsLoop = false;
    mFirstBars = false;
    mTargetLength = 0;
    mDeferredOverdub = false;
    mRingArmed = false;
    mPendingStart = -1;
    mPendingArm = -1;
    mCountInLeft = 0;
    mWrapPending = false;
    mState = LooperState::Empty;
}

// ── Undo, redo, remove ──────────────────────────────────────────────────────────────────

void LooperEngine::Undo()
{
    if (mHistoryCount == 0)
    {
        return;
    }

    const Entry entry = mHistory[static_cast<std::size_t>(--mHistoryCount)];

    if (entry.kind == EntryKind::Recorded)
    {
        HideSlot(entry.slot);
    }
    else
    {
        ShowSlot(entry.slot);
    }

    mRedo[static_cast<std::size_t>(mRedoCount++)] = entry;
    AfterVisibilityChange();
}

void LooperEngine::Redo()
{
    if (mRedoCount == 0)
    {
        return;
    }

    const Entry entry = mRedo[static_cast<std::size_t>(--mRedoCount)];

    if (entry.kind == EntryKind::Recorded)
    {
        ShowSlot(entry.slot);
    }
    else
    {
        HideSlot(entry.slot);
    }

    mHistory[static_cast<std::size_t>(mHistoryCount++)] = entry;
    AfterVisibilityChange();
}

void LooperEngine::RemoveLayer(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= kMaxLayers)
    {
        return;
    }

    Slot& slot = mSlots[static_cast<std::size_t>(slotIndex)];

    if (slot.status == SlotStatus::Recording)
    {
        mDeferredOverdub = false;
        DiscardRecording();
        mState = mLength > 0 ? LooperState::Playing : LooperState::Empty;
        return;
    }

    if (slot.status != SlotStatus::Visible)
    {
        return;
    }

    ClearRedo();
    HideSlot(slotIndex);
    mHistory[static_cast<std::size_t>(mHistoryCount++)] = {EntryKind::Removed, slotIndex};
    AfterVisibilityChange();
    Raise(kNoticeStateChanged);
}

void LooperEngine::AfterVisibilityChange()
{
    const int visible = VisibleCount();

    if (visible == 0 && (mState == LooperState::Playing || mState == LooperState::Stopped))
    {
        if (mPlaying)
        {
            mStopping = true;
        }

        mState = LooperState::Empty;
    }
    else if (visible > 0 && mState == LooperState::Empty && mLength > 0)
    {
        mState = LooperState::Stopped;
    }
}

void LooperEngine::ClearRedo()
{
    mRedoCount = 0;
    ReleaseUnreferenced();
}

void LooperEngine::HideSlot(int slotIndex)
{
    KillWritersFor(slotIndex);
    mSlots[static_cast<std::size_t>(slotIndex)].status = SlotStatus::Hidden;
}

void LooperEngine::ShowSlot(int slotIndex)
{
    mSlots[static_cast<std::size_t>(slotIndex)].status = SlotStatus::Visible;
}

void LooperEngine::ReleaseSlot(int slotIndex, bool immediately)
{
    KillWritersFor(slotIndex);
    Slot& slot = mSlots[static_cast<std::size_t>(slotIndex)];

    if (slot.status == SlotStatus::Free)
    {
        return;
    }

    if (!immediately && mPlaying && (slot.gainL > 0.0f || slot.gainR > 0.0f))
    {
        slot.status = SlotStatus::Hidden;
        slot.freeWhenSilent = true;
        return;
    }

    slot = Slot{};
}

void LooperEngine::ReleaseUnreferenced()
{
    for (int s = 0; s < kMaxLayers; ++s)
    {
        const Slot& slot = mSlots[static_cast<std::size_t>(s)];

        if (slot.status == SlotStatus::Hidden && !slot.freeWhenSilent && !IsReferenced(s))
        {
            ReleaseSlot(s, false);
        }
    }
}

bool LooperEngine::IsReferenced(int slotIndex) const
{
    for (int e = 0; e < mHistoryCount; ++e)
    {
        if (mHistory[static_cast<std::size_t>(e)].slot == slotIndex)
        {
            return true;
        }
    }

    for (int e = 0; e < mRedoCount; ++e)
    {
        if (mRedo[static_cast<std::size_t>(e)].slot == slotIndex)
        {
            return true;
        }
    }

    return false;
}

int LooperEngine::FindFreeSlot() const
{
    for (int s = 0; s < kMaxLayers; ++s)
    {
        if (mSlots[static_cast<std::size_t>(s)].status == SlotStatus::Free)
        {
            return s;
        }
    }

    return -1;
}

int LooperEngine::VisibleCount() const
{
    return static_cast<int>(
        std::count_if(mSlots.begin(), mSlots.end(), [](const Slot& s) { return s.status == SlotStatus::Visible; }));
}

LooperEngine::Writer* LooperEngine::RecordingWriter()
{
    for (auto& w : mWriters)
    {
        if (w.phase == WriterPhase::Recording)
        {
            return &w;
        }
    }

    return nullptr;
}

LooperEngine::Writer* LooperEngine::NewWriter()
{
    for (auto& w : mWriters)
    {
        if (w.phase == WriterPhase::Off)
        {
            return &w;
        }
    }

    return nullptr;
}

void LooperEngine::KillWritersFor(int slotIndex)
{
    for (auto& w : mWriters)
    {
        if (w.phase != WriterPhase::Off && w.slot == slotIndex)
        {
            w.phase = WriterPhase::Off;
            mSlots[static_cast<std::size_t>(slotIndex)].final = true;
        }
    }
}

void LooperEngine::SnapGains(int slotIndex)
{
    std::array<float, kMaxLayers> targetsL{};
    std::array<float, kMaxLayers> targetsR{};
    UpdateSlotTargets(targetsL.data(), targetsR.data());
    Slot& slot = mSlots[static_cast<std::size_t>(slotIndex)];
    slot.gainL = targetsL[static_cast<std::size_t>(slotIndex)];
    slot.gainR = targetsR[static_cast<std::size_t>(slotIndex)];
}

void LooperEngine::ReplaceAudio(int slotIndex, std::uint32_t serial, const LayerAudio* audio, bool crossfade)
{
    if (slotIndex < 0 || slotIndex >= kMaxLayers || !audio || !audio->wet[0] || audio->capacity < mLength)
    {
        return;
    }

    Slot& slot = mSlots[static_cast<std::size_t>(slotIndex)];

    // Only a finished layer: nothing writes to it any more.
    if (slot.status == SlotStatus::Free || slot.status == SlotStatus::Recording || !slot.final ||
        slot.serial != serial)
    {
        return;
    }

    slot.fadeFrom = nullptr;
    slot.fadeLeft = 0;

    if (crossfade && mPlaying && slot.audio)
    {
        slot.fadeFrom = slot.audio;
        slot.fadeFromHead = slot.head;
        slot.fadeFromCapacity = slot.capacity;
        slot.fadeLeft = mSwapSamples;
    }

    slot.audio = audio;
    slot.head = 0;
    slot.capacity = mLength;
}

void LooperEngine::LoadLoop(const LoopImage* image)
{
    if (!image || image->length <= 0)
    {
        return;
    }

    FinishClear();
    mLength = image->length;
    mBarsLoop = image->barsLoop;

    for (int i = 0; i < std::min(image->layerCount, kMaxLayers); ++i)
    {
        const LoopLayerImage& layer = image->layers[static_cast<std::size_t>(i)];

        if (!layer.audio || !layer.audio->wet[0] || layer.audio->capacity < mLength)
        {
            continue;
        }

        Slot& slot = mSlots[static_cast<std::size_t>(i)];
        slot = Slot{};
        slot.status = SlotStatus::Visible;
        slot.audio = layer.audio;
        slot.capacity = mLength;
        slot.serial = mNextSerial++;
        slot.spanStart = std::clamp<std::int64_t>(layer.spanStart, 0, mLength - 1);
        slot.spanLength = std::clamp<std::int64_t>(layer.spanLength, 1, mLength);
        slot.ringOut = std::clamp<std::int64_t>(layer.ringOut, 0, mLength);
        slot.latency = layer.latencySamples;
        slot.calibrationGain = layer.calibrationGain;
        slot.diChannels = layer.diChannels;
        slot.firstLayer = layer.firstLayer;
        slot.final = true;
        LayerMix& mix = mMix[static_cast<std::size_t>(i)];
        mix.level.store(layer.level, std::memory_order_relaxed);
        mix.balance.store(layer.balance, std::memory_order_relaxed);
        mix.muted.store(layer.muted, std::memory_order_relaxed);
        mix.solo.store(layer.solo, std::memory_order_relaxed);
    }

    if (VisibleCount() == 0)
    {
        FinishClear();
        return;
    }

    mState = LooperState::Stopped;
    Raise(kNoticeStateChanged);
}
} // namespace guitarfx::looper
