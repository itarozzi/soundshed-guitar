// RiffRecorder: riff capture on the looper's recorder. See RiffRecorder.h.

#include "controller/RiffRecorder.h"

#include <algorithm>

namespace guitarfx
{
using looper::LooperAction;
using looper::LooperCommand;
using looper::LooperSnapshot;
using looper::LooperState;
using looper::SlotStatus;

void RiffRecorder::Audio::Rotate()
{
    const auto capacity = static_cast<std::int64_t>(left.size());

    if (capacity == 0 || length <= 0 || right.size() != left.size())
    {
        left.clear();
        right.clear();
        length = 0;
        return;
    }

    const auto first = static_cast<std::ptrdiff_t>(((head % capacity) + capacity) % capacity);
    std::rotate(left.begin(), left.begin() + first, left.end());
    std::rotate(right.begin(), right.begin() + first, right.end());
    const auto kept = static_cast<std::size_t>(std::min(length, capacity));
    left.resize(kept);
    right.resize(kept);
    head = 0;
}

void RiffRecorder::Prepare(double sampleRate, int maxBlockSize)
{
    // A take in flight at another rate is dropped with the engine's state.
    if (sampleRate != mSampleRate && HasTake())
    {
        (void)Release();
    }

    mSampleRate = sampleRate;
    mEngine.Prepare(sampleRate, maxBlockSize);
    mEngine.SetTransportClock(&mClock);

    // One layer, never played: no loop, no tail, no alignment, and the chains' latency is nothing to
    // it, since it records no wet sound.
    auto& settings = mEngine.Settings();
    settings.playbackEnabled.store(false);
    settings.lengthBars.store(false);
    settings.countInEveryTake.store(true);
    settings.afterRecordOverdub.store(false);
    settings.alignSamples.store(0);
    settings.chainLatencySamples.store(0);
    settings.armThreshold.store(kArmThreshold);
}

bool RiffRecorder::Recording(LooperState state) noexcept
{
    return state == LooperState::CountIn || state == LooperState::Armed || state == LooperState::Recording;
}

bool RiffRecorder::Busy() const noexcept
{
    return Recording(mEngine.State());
}

void RiffRecorder::Start(std::vector<float> left, std::vector<float> right, bool armed, double samplesPerBar,
                         int countInBars)
{
    (void)Release();
    mLeft = std::move(left);
    mRight = std::move(right);
    mCapture = {};
    mCapture.di[0] = mLeft.data();
    mCapture.di[1] = mRight.data();
    mCapture.capacity = static_cast<std::int64_t>(std::min(mLeft.size(), mRight.size()));
    mClock.samplesPerBar.store(samplesPerBar);
    mEngine.Settings().startArmed.store(armed);
    mEngine.Settings().countInBars.store(std::max(0, countInBars));
    mEngine.Apply({LooperCommand::Type::SetCapture, LooperAction::RecPlayDub, -1, &mCapture});
    mEngine.Press(LooperAction::Record);
    mWasRecording = Busy();
}

bool RiffRecorder::Close()
{
    if (mEngine.State() == LooperState::Recording)
    {
        mEngine.Press(LooperAction::Stop);
    }

    // Closed with something in it; a take under the shortest the recorder keeps is dropped.
    return mEngine.State() == LooperState::Stopped;
}

void RiffRecorder::Drop()
{
    (void)Release();
}

RiffRecorder::Audio RiffRecorder::Release()
{
    Audio audio;
    LooperSnapshot snapshot;

    if (mEngine.ReadSnapshot(snapshot))
    {
        for (const auto& slot : snapshot.slots)
        {
            if (slot.firstLayer && slot.status == SlotStatus::Visible)
            {
                audio.head = slot.head;
                audio.length = slot.spanLength;
            }
        }
    }

    mEngine.Press(LooperAction::Clear);
    mEngine.Apply({LooperCommand::Type::SetCapture, LooperAction::RecPlayDub, -1, nullptr});
    audio.left = std::move(mLeft);
    audio.right = std::move(mRight);
    mLeft.clear();
    mRight.clear();
    mCapture = {};
    mWasRecording = false;
    return audio;
}

bool RiffRecorder::Process(float** inputs, int numSamples) noexcept
{
    if (mLeft.empty())
    {
        return false;
    }

    const float* left = inputs ? inputs[0] : nullptr;
    const float* right = inputs && inputs[1] ? inputs[1] : left;
    mEngine.NoteInput(left, right, numSamples, 1.0f);
    mEngine.Process(nullptr, nullptr, numSamples, false);
    const bool recording = Busy();
    const bool ended = mWasRecording && !recording;
    mWasRecording = recording;
    return ended;
}

RiffRecorder::Status RiffRecorder::Poll() const
{
    Status status;
    LooperSnapshot snapshot;

    if (!mEngine.ReadSnapshot(snapshot))
    {
        return status;
    }

    for (const auto& slot : snapshot.slots)
    {
        if (slot.firstLayer && (slot.status == SlotStatus::Recording || slot.status == SlotStatus::Visible))
        {
            status.started = true;
            status.finished = slot.status == SlotStatus::Visible;
        }
    }

    status.recorded = mEngine.LiveRecordedSamples();
    status.armLead = mEngine.ArmLeadSamples();
    return status;
}
} // namespace guitarfx
