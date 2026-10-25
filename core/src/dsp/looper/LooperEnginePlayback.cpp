// LooperEngine: what it publishes for the message thread, and playback. See LooperEngine.h and docs/plans/looper.md.

#include "dsp/looper/LooperEngine.h"
#include "dsp/looper/LooperEngineInternal.h"

#include <algorithm>

namespace guitarfx::looper
{
using namespace detail;

bool LooperEngine::ReadSnapshot(LooperSnapshot& out) const
{
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const std::uint64_t before = mSequence.load(std::memory_order_acquire);

        if ((before & 1u) != 0)
        {
            continue;
        }

        out.state = mPublishedState.load(std::memory_order_relaxed);
        out.length = mPublishedLength.load(std::memory_order_relaxed);
        out.playhead = mPublishedPlayhead.load(std::memory_order_relaxed);
        out.playing = mPublishedPlaying.load(std::memory_order_relaxed);
        out.barsLoop = mPublishedBars.load(std::memory_order_relaxed);
        out.canUndo = mPublishedCanUndo.load(std::memory_order_relaxed);
        out.canRedo = mPublishedCanRedo.load(std::memory_order_relaxed);
        out.visibleLayers = mPublishedVisible.load(std::memory_order_relaxed);
        out.capture = mPublishedCapture.load(std::memory_order_relaxed);
        out.spare = mPublishedSpare.load(std::memory_order_relaxed);

        for (std::size_t i = 0; i < out.slots.size(); ++i)
        {
            const auto& p = mPublishedSlots[i];
            auto& s = out.slots[i];
            s.status = p.status.load(std::memory_order_relaxed);
            s.audio = p.audio.load(std::memory_order_relaxed);
            s.head = p.head.load(std::memory_order_relaxed);
            s.serial = p.serial.load(std::memory_order_relaxed);
            s.spanStart = p.spanStart.load(std::memory_order_relaxed);
            s.spanLength = p.spanLength.load(std::memory_order_relaxed);
            s.ringOut = p.ringOut.load(std::memory_order_relaxed);
            s.latencySamples = p.latency.load(std::memory_order_relaxed);
            s.calibrationGain = p.calibrationGain.load(std::memory_order_relaxed);
            s.diChannels = p.diChannels.load(std::memory_order_relaxed);
            s.firstLayer = p.firstLayer.load(std::memory_order_relaxed);
            s.final = p.final.load(std::memory_order_relaxed);
            s.fadingFrom = p.fadingFrom.load(std::memory_order_relaxed);
        }

        std::atomic_thread_fence(std::memory_order_acquire);

        if (mSequence.load(std::memory_order_relaxed) == before)
        {
            out.generation = before / 2;
            return true;
        }
    }

    return false;
}

void LooperEngine::Publish()
{
    const std::uint64_t sequence = mSequence.load(std::memory_order_relaxed);
    mSequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    mPublishedState.store(mState, std::memory_order_relaxed);
    mPublishedLength.store(mLength, std::memory_order_relaxed);
    mPublishedPlayhead.store(mPlayhead, std::memory_order_relaxed);
    mPublishedPlaying.store(mPlaying, std::memory_order_relaxed);
    mPublishedBars.store(mBarsLoop, std::memory_order_relaxed);
    mPublishedCanUndo.store(mHistoryCount > 0, std::memory_order_relaxed);
    mPublishedCanRedo.store(mRedoCount > 0, std::memory_order_relaxed);
    mPublishedVisible.store(VisibleCount(), std::memory_order_relaxed);
    mPublishedCapture.store(mCapture, std::memory_order_relaxed);
    mPublishedSpare.store(mSpare, std::memory_order_relaxed);

    for (std::size_t i = 0; i < mSlots.size(); ++i)
    {
        const Slot& s = mSlots[i];
        auto& p = mPublishedSlots[i];
        p.status.store(s.status, std::memory_order_relaxed);
        p.audio.store(s.audio, std::memory_order_relaxed);
        p.head.store(s.head, std::memory_order_relaxed);
        p.serial.store(s.serial, std::memory_order_relaxed);
        p.spanStart.store(s.spanStart, std::memory_order_relaxed);
        p.spanLength.store(s.spanLength, std::memory_order_relaxed);
        p.ringOut.store(s.ringOut, std::memory_order_relaxed);
        p.latency.store(s.latency, std::memory_order_relaxed);
        p.calibrationGain.store(s.calibrationGain, std::memory_order_relaxed);
        p.diChannels.store(s.diChannels, std::memory_order_relaxed);
        p.firstLayer.store(s.firstLayer, std::memory_order_relaxed);
        p.final.store(s.final, std::memory_order_relaxed);
        p.fadingFrom.store(s.fadeFrom, std::memory_order_relaxed);
    }

    mPublishedCountIn.store(mState == LooperState::CountIn ? mCountInLeft : 0, std::memory_order_relaxed);
    mPublishedArmWait.store(
        (mState == LooperState::Armed || mState == LooperState::OverdubArmed) ? mArmWait : 0,
        std::memory_order_relaxed);
    mSequence.store(sequence + 2, std::memory_order_release);
}

// ── Playback ───────────────────────────────────────────────────────────────────────────

void LooperEngine::UpdateSlotTargets(float* targetsL, float* targetsR) const
{
    bool anySolo = false;

    for (std::size_t s = 0; s < mSlots.size(); ++s)
    {
        if (mSlots[s].status == SlotStatus::Visible && mMix[s].solo.load(std::memory_order_relaxed))
        {
            anySolo = true;
        }
    }

    const float transport = (mStopping || mClearing) ? 0.0f : mSettings.level.load(std::memory_order_relaxed);

    for (std::size_t s = 0; s < mSlots.size(); ++s)
    {
        const LayerMix& mix = mMix[s];
        const bool audible = mSlots[s].status == SlotStatus::Visible && !mix.muted.load(std::memory_order_relaxed) &&
                             (!anySolo || mix.solo.load(std::memory_order_relaxed));

        if (!audible)
        {
            targetsL[s] = 0.0f;
            targetsR[s] = 0.0f;
            continue;
        }

        // Balance, not pan: unity at centre, and turning to one side only lowers the other.
        const float balance = std::clamp(mix.balance.load(std::memory_order_relaxed), -1.0f, 1.0f);
        const float level = std::max(0.0f, mix.level.load(std::memory_order_relaxed)) * transport;
        targetsL[s] = level * (balance > 0.0f ? 1.0f - balance : 1.0f);
        targetsR[s] = level * (balance < 0.0f ? 1.0f + balance : 1.0f);
    }
}

bool LooperEngine::MixPlayback(float* outL, float* outR, int numSamples)
{
    std::array<float, kMaxLayers> targetsL{};
    std::array<float, kMaxLayers> targetsR{};
    UpdateSlotTargets(targetsL.data(), targetsR.data());
    const float step = 1.0f / static_cast<float>(mRampSamples);
    bool added = false;

    for (std::size_t s = 0; s < mSlots.size(); ++s)
    {
        Slot& slot = mSlots[s];
        const float tl = targetsL[s];
        const float tr = targetsR[s];

        if (!slot.audio || (slot.status != SlotStatus::Visible && slot.status != SlotStatus::Hidden) ||
            (slot.gainL <= 0.0f && slot.gainR <= 0.0f && tl <= 0.0f && tr <= 0.0f))
        {
            continue;
        }

        const int from = std::max(mPlayFrom, slot.audibleFrom);
        std::int64_t pos = Wrap(mPlayheadAtPlayFrom + (from - mPlayFrom), mLength);
        const float* wetL = slot.audio->wet[0];
        const float* wetR = slot.audio->wet[1] ? slot.audio->wet[1] : wetL;
        const std::int64_t validLength =
            slot.firstLayer ? mLength : std::min(mLength, slot.spanLength + slot.ringOut);
        float gl = slot.gainL;
        float gr = slot.gainR;
        // A ReplaceAudio crossfade: the old audio and the new, mixed sample by sample.
        const float* oldL = slot.fadeFrom ? slot.fadeFrom->wet[0] : nullptr;
        const float* oldR = slot.fadeFrom && slot.fadeFrom->wet[1] ? slot.fadeFrom->wet[1] : oldL;

        for (int i = from; i < numSamples;)
        {
            // A run of samples that stays inside (or outside) what the layer holds, and on one side
            // of the loop point and of the buffer's end.
            const std::int64_t rel = Wrap(pos - slot.spanStart, mLength);
            const bool valid = rel < validLength;
            std::int64_t run = std::min<std::int64_t>(numSamples - i, mLength - pos);
            run = std::min(run, valid ? validLength - rel : mLength - rel);
            const std::int64_t index = Wrap(slot.head + pos, slot.capacity);
            run = std::max<std::int64_t>(1, std::min(run, slot.capacity - index));
            const int count = static_cast<int>(run);

            if (!valid)
            {
                gl = MoveTowards(gl, tl, step * static_cast<float>(count));
                gr = MoveTowards(gr, tr, step * static_cast<float>(count));
            }
            else if (oldL && slot.fadeLeft > 0)
            {
                const std::int64_t oldIndex = Wrap(slot.fadeFromHead + pos, slot.fadeFromCapacity);

                for (int k = 0; k < count; ++k)
                {
                    gl = MoveTowards(gl, tl, step);
                    gr = MoveTowards(gr, tr, step);
                    float l = wetL[index + k];
                    float r = wetR[index + k];

                    if (slot.fadeLeft > 0)
                    {
                        const float x = 1.0f - static_cast<float>(slot.fadeLeft) / static_cast<float>(mSwapSamples);
                        const auto o = static_cast<std::size_t>(Wrap(oldIndex + k, slot.fadeFromCapacity));
                        l = l * x + oldL[o] * (1.0f - x);
                        r = r * x + oldR[o] * (1.0f - x);
                        --slot.fadeLeft;
                    }

                    if (outL)
                    {
                        outL[i + k] += l * gl;
                    }

                    if (outR)
                    {
                        outR[i + k] += r * gr;
                    }
                }

                added = true;

                if (slot.fadeLeft <= 0)
                {
                    slot.fadeFrom = nullptr;
                    oldL = nullptr;
                }
            }
            else if (gl == tl && gr == tr)
            {
                const float* l = wetL + index;
                const float* r = wetR + index;

                if (outL)
                {
                    for (int k = 0; k < count; ++k)
                    {
                        outL[i + k] += l[k] * gl;
                    }
                }

                if (outR)
                {
                    for (int k = 0; k < count; ++k)
                    {
                        outR[i + k] += r[k] * gr;
                    }
                }

                added = true;
            }
            else
            {
                for (int k = 0; k < count; ++k)
                {
                    gl = MoveTowards(gl, tl, step);
                    gr = MoveTowards(gr, tr, step);

                    if (outL)
                    {
                        outL[i + k] += wetL[index + k] * gl;
                    }

                    if (outR)
                    {
                        outR[i + k] += wetR[index + k] * gr;
                    }
                }

                added = true;
            }

            i += count;
            pos += run;

            if (pos >= mLength)
            {
                pos -= mLength;
            }
        }

        slot.gainL = gl;
        slot.gainR = gr;
    }

    return added;
}
} // namespace guitarfx::looper
