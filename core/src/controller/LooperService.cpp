// LooperService: lifecycle, buffers, the metronome link and the UI feeds. The settings and the
// messages are in LooperServiceMessages.cpp. See LooperService.h.

#include "controller/LooperService.h"

#include "IPluginHost.h"
#include "controller/MetronomeService.h"
#include "dsp/MultiPresetMixer.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace guitarfx
{
using looper::LooperAction;
using looper::LooperCommand;
using looper::LooperEngine;
using looper::LooperSnapshot;
using looper::LooperState;
using looper::SlotStatus;

namespace
{
/// Idle ticks (about 60 a second) between re-offers of a capture buffer the engine refused.
constexpr int kReofferTicks = 30;
/// Idle ticks between position updates, and between live waveforms, while the loop runs.
constexpr int kPositionTicks = 4;
constexpr int kLiveWaveformTicks = 15;

bool Takes(LooperState state)
{
    return state == LooperState::CountIn || state == LooperState::Armed || state == LooperState::Recording;
}
} // namespace

LooperService::Buffer::Buffer(std::int64_t capacity, int diChannels, bool wet)
    : di0(static_cast<std::size_t>(capacity)), di1(diChannels > 1 ? static_cast<std::size_t>(capacity) : 0),
      wet0(wet ? static_cast<std::size_t>(capacity) : 0), wet1(wet ? static_cast<std::size_t>(capacity) : 0)
{
    audio.di[0] = di0.data();
    audio.di[1] = di1.empty() ? nullptr : di1.data();
    audio.wet[0] = wet0.empty() ? nullptr : wet0.data();
    audio.wet[1] = wet1.empty() ? nullptr : wet1.data();
    audio.capacity = capacity;
}

std::size_t LooperService::Buffer::Bytes() const noexcept
{
    return (di0.size() + di1.size() + wet0.size() + wet1.size()) * sizeof(float);
}

LooperService::LooperService(IPluginHost& host, MultiPresetMixer& mixer, MetronomeService& metronome,
                             std::mutex& dspMutex, nlohmann::json& appSettings, std::function<void()> saveSettings,
                             std::function<Tone()> currentTone, std::function<void()> metronomeStateChanged,
                             SendMessageFn sendMessage)
    : mHost(host), mMixer(mixer), mEngine(mixer.Looper()), mMetronome(metronome), mDSPMutex(dspMutex),
      mAppSettings(appSettings), mSaveSettings(std::move(saveSettings)), mCurrentTone(std::move(currentTone)),
      mMetronomeStateChanged(std::move(metronomeStateChanged)), mSendMessage(std::move(sendMessage))
{
    // The engine reads the bar grid the metronome writes; both are on the audio thread.
    mEngine.SetTransportClock(&mMetronome.Clock());
    ApplySettingsToEngine();
}

LooperService::~LooperService() = default;

std::string LooperService::LayerId(std::uint32_t serial)
{
    return "L" + std::to_string(serial);
}

bool LooperService::Active() const noexcept
{
    return mSettings.enabled && mHost.IsStandalone() && mSampleRate.load(std::memory_order_relaxed) > 0.0;
}

void LooperService::Prepare(double sampleRate, int maxBlockSize)
{
    (void)maxBlockSize;
    // The mixer's Prepare() has prepared the engine. A new rate wants a capture buffer sized for it,
    // which OnIdle() sees by its length.
    mSampleRate.store(sampleRate, std::memory_order_relaxed);
}

std::size_t LooperService::HeldBytes() const noexcept
{
    std::size_t bytes = 0;

    for (const auto& buffer : mBuffers)
    {
        bytes += buffer->Bytes();
    }

    return bytes;
}

std::shared_ptr<LooperService::Buffer> LooperService::Allocate(std::int64_t capacity, int diChannels)
{
    const auto budget = static_cast<std::size_t>(std::max(64, mSettings.memoryBudgetMb)) * 1024u * 1024u;
    const auto bytes = static_cast<std::size_t>(capacity) * static_cast<std::size_t>(diChannels + 2) * sizeof(float);

    if (capacity <= 0 || HeldBytes() + bytes > budget)
    {
        return nullptr;
    }

    try
    {
        auto buffer = std::make_shared<Buffer>(capacity, diChannels, true);
        mBuffers.push_back(buffer);
        return buffer;
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
}

void LooperService::Offer(LooperCommand::Type type, const std::shared_ptr<Buffer>& buffer)
{
    const std::uint64_t applied = mEngine.BlocksProcessed() + 2;

    if (buffer)
    {
        buffer->appliedAfter = applied;
    }

    if (type == LooperCommand::Type::SetCapture)
    {
        mCaptureOfferApplied = applied;
        mCaptureOffered = true;
    }

    mEngine.Enqueue({type, LooperAction::RecPlayDub, -1, buffer ? &buffer->audio : nullptr});
}

void LooperService::OnIdle()
{
    ++mIdleTicks;
    const bool active = Active();

    // Switched off (or the device went away): the loop is cleared and every buffer goes back.
    if (mWasActive && !active)
    {
        mEngine.Enqueue({LooperCommand::Type::Action, LooperAction::Clear, -1, nullptr});
        Offer(LooperCommand::Type::SetSpare, nullptr);
        mWantedCapture.reset();
        mOfferedSpare.reset();
        mCaptureOffered = false;
    }

    mWasActive = active;

    // Read the block count first: a snapshot read after it shows at least what that block left.
    const std::uint64_t blocks = mEngine.BlocksProcessed();
    LooperSnapshot snapshot;

    if (!mEngine.ReadSnapshot(snapshot))
    {
        return;
    }

    mSnapshot = snapshot;
    mHaveSnapshot = true;

    if (active)
    {
        // What the latency compensation needs: the chains' latency for the wet tap, and D, how much
        // later a note played along with the output reaches the input.
        const double rate = mSampleRate.load(std::memory_order_relaxed);
        auto& engineSettings = mEngine.Settings();
        engineSettings.chainLatencySamples.store(std::max(0, mMixer.GetTotalLatencySamples()),
                                                 std::memory_order_relaxed);
        const auto offset = static_cast<int>(std::lround(mSettings.alignOffsetMs * rate / 1000.0));
        engineSettings.alignSamples.store(std::max(0, mHost.GetDeviceLatencySamples() + offset),
                                          std::memory_order_relaxed);
    }

    ManageBuffers(snapshot);

    // Off the engine's hands: what no snapshot refers to and no offer still in flight could hand it.
    const auto referenced = [&snapshot](const looper::LayerAudio* audio) {
        if (snapshot.capture == audio || snapshot.spare == audio)
        {
            return true;
        }

        return std::any_of(snapshot.slots.begin(), snapshot.slots.end(),
                           [audio](const looper::SlotInfo& slot) { return slot.audio == audio; });
    };

    mBuffers.erase(std::remove_if(mBuffers.begin(), mBuffers.end(),
                                  [&](const std::shared_ptr<Buffer>& buffer) {
                                      return buffer != mWantedCapture && buffer != mOfferedSpare &&
                                             blocks >= buffer->appliedAfter && !referenced(&buffer->audio);
                                  }),
                   mBuffers.end());

    TrackLayers(snapshot);
    LinkMetronome(snapshot);

    if (const std::uint32_t notices = mEngine.TakeNotices(); notices != 0)
    {
        const std::pair<std::uint32_t, const char*> names[] = {
            {LooperEngine::kNoticeNoCapture, "noCapture"}, {LooperEngine::kNoticeTooLong, "tooLong"},
            {LooperEngine::kNoticeNoSpare, "noSpare"},     {LooperEngine::kNoticeNoSlot, "noLayerFree"},
            {LooperEngine::kNoticeNoTempo, "noTempo"},     {LooperEngine::kNoticeTooShort, "tooShort"},
        };

        for (const auto& [bit, name] : names)
        {
            if ((notices & bit) != 0)
            {
                mPendingNotice = name;
            }
        }
    }

    SendFeeds(snapshot);
}

void LooperService::ManageBuffers(const LooperSnapshot& snapshot)
{
    const std::uint64_t blocks = mEngine.BlocksProcessed();

    if (!Active())
    {
        // Until the engine has let go of the capture buffer, keep asking.
        if (snapshot.capture && (!mCaptureOffered || blocks >= mCaptureOfferApplied) &&
            (!mCaptureOffered || mIdleTicks % kReofferTicks == 0))
        {
            Offer(LooperCommand::Type::SetCapture, nullptr);
        }

        return;
    }

    // The capture buffer: the longest first layer, and the Armed pre-roll ring. Two DI channels, so a
    // stereo input records both.
    const auto captureLength =
        static_cast<std::int64_t>(std::llround(mSampleRate.load(std::memory_order_relaxed) * mSettings.maxSeconds));

    if (!mWantedCapture || mWantedCapture->audio.capacity != captureLength)
    {
        mWantedCapture = Allocate(captureLength, 2);
        mCaptureOffered = false;

        if (!mWantedCapture)
        {
            mPendingNotice = "memory";
            return;
        }
    }

    // Offered until adopted: the engine refuses one while a loop still plays from the old.
    if (snapshot.capture != &mWantedCapture->audio && (!mCaptureOffered || blocks >= mCaptureOfferApplied) &&
        (!mCaptureOffered || mIdleTicks % kReofferTicks == 0))
    {
        Offer(LooperCommand::Type::SetCapture, mWantedCapture);
    }

    // The spare: capture-sized before the loop has a length, so an overdub can open the moment a free
    // first layer closes; then exactly the loop's length.
    if (mOfferedSpare && (snapshot.spare == &mOfferedSpare->audio || blocks >= mOfferedSpare->appliedAfter))
    {
        mOfferedSpare.reset();
    }

    if (mOfferedSpare)
    {
        return;
    }

    const std::int64_t length = snapshot.length;
    const std::int64_t wanted = length > 0 ? length : captureLength;
    const looper::LayerAudio* spare = snapshot.spare;
    const bool fits = spare && spare->capacity >= wanted && (length <= 0 || spare->capacity <= length + length / 2);

    if (fits)
    {
        mRedoDropped = false;
        return;
    }

    const bool mono = mMixer.GetEffectiveInputMode() == MultiPresetMixer::InputMode::Mono;
    auto buffer = Allocate(wanted, length > 0 && mono ? 1 : 2);

    if (!buffer)
    {
        // Over the memory budget: the redo stack goes first, then the next layer is refused.
        if (!mRedoDropped)
        {
            mEngine.Enqueue({LooperCommand::Type::DropRedo, LooperAction::RecPlayDub, -1, nullptr});
            mRedoDropped = true;
        }
        else
        {
            mPendingNotice = "memory";
        }

        return;
    }

    Offer(LooperCommand::Type::SetSpare, buffer);
    mOfferedSpare = buffer;
}

void LooperService::TrackLayers(const LooperSnapshot& snapshot)
{
    if (snapshot.length == 0 && snapshot.state == LooperState::Empty)
    {
        mLayerNumber = 0;
    }

    std::map<std::uint32_t, LayerMeta> kept;

    for (const auto& slot : snapshot.slots)
    {
        if (slot.status == SlotStatus::Free)
        {
            continue;
        }

        auto it = mLayers.find(slot.serial);

        if (it == mLayers.end())
        {
            LayerMeta meta;
            meta.title = "Layer " + std::to_string(++mLayerNumber);
            meta.tone = mCurrentTone ? mCurrentTone() : Tone{};
            it = mLayers.emplace(slot.serial, std::move(meta)).first;
        }

        // Set when the preset or scene changed while it recorded.
        if (slot.status == SlotStatus::Recording && mCurrentTone)
        {
            const Tone now = mCurrentTone();
            it->second.toneChanged = it->second.toneChanged || now.presetId != it->second.tone.presetId ||
                                     now.sceneId != it->second.tone.sceneId;
        }

        kept.insert(*it);
    }

    mLayers.swap(kept);
}

void LooperService::LinkMetronome(const LooperSnapshot& snapshot)
{
    // A Bars loop holds the tempo it was made at, from the press that starts it to Clear.
    const bool locked = snapshot.barsLoop || (mSettings.lengthBars && Takes(snapshot.state));

    if (locked != mTempoLocked)
    {
        mTempoLocked = locked;
        mMetronome.SetTempoLocked(locked);

        if (mMetronomeStateChanged)
        {
            mMetronomeStateChanged();
        }
    }

    // The count-in click played for a take with the metronome off ends with the take.
    if (mGuidanceForCountIn && !Takes(snapshot.state))
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mMetronome.DeactivateGuidance(false);
        mGuidanceForCountIn = false;
    }
}

void LooperService::AfterMixer() noexcept
{
    if (const int reset = mEngine.TakeMetronomeReset(); reset >= 0)
    {
        mMetronome.RequestReset(reset);
    }

    const LooperState state = mEngine.State();
    const bool taking = state == LooperState::Recording || state == LooperState::Overdubbing;
    mMetronome.SetClickSuppressed(taking && !mClickWhileRecording.load(std::memory_order_relaxed));
}

void LooperService::SendFeeds(const LooperSnapshot& snapshot)
{
    if (!mSendMessage)
    {
        return;
    }

    nlohmann::json state = BuildState();
    const bool running = snapshot.playing || Takes(snapshot.state);
    nlohmann::json structural = state;
    structural.erase("positionSec");
    structural.erase("armWaitingSec");
    const std::string key = structural.dump();

    if (key != mLastState || (running && mIdleTicks % kPositionTicks == 0 && snapshot.playhead != mLastPositionSent))
    {
        mLastState = key;
        mLastPositionSent = snapshot.playhead;
        state["type"] = "looperState";
        mSendMessage(state.dump());
        mPendingNotice.clear();
    }

    nlohmann::json layers = BuildLayers();
    const std::string layersKey = layers.dump();

    if (layersKey != mLastLayers)
    {
        mLastLayers = layersKey;
        nlohmann::json msg;
        msg["type"] = "looperLayers";
        msg["layers"] = std::move(layers);
        mSendMessage(msg.dump());
    }

    SendWaveforms(snapshot);
}

void LooperService::SendWaveforms(const LooperSnapshot& snapshot)
{
    const std::int64_t length = snapshot.length;
    nlohmann::json layers = nlohmann::json::object();
    nlohmann::json live;

    for (const auto& slot : snapshot.slots)
    {
        auto meta = mLayers.find(slot.serial);

        if (meta == mLayers.end() || !slot.audio)
        {
            continue;
        }

        if (slot.status == SlotStatus::Recording)
        {
            if (mIdleTicks % kLiveWaveformTicks == 0)
            {
                nlohmann::json peaks = nlohmann::json::array();

                for (int b = 0; b < looper::kPeakBuckets; ++b)
                {
                    peaks.push_back(mEngine.LivePeak(false, b));
                }

                live = {{"layerId", LayerId(slot.serial)},
                        {"bucketSamples", mEngine.LiveBucketSamples()},
                        {"recordedSamples", mEngine.LiveRecordedSamples()},
                        {"peaks", std::move(peaks)}};
            }

            continue;
        }

        if (slot.status != SlotStatus::Visible || !slot.final || meta->second.peaksSent || length <= 0)
        {
            continue;
        }

        // A finished layer is never written again, so its buffer is read here without a lock.
        const std::int64_t valid = slot.firstLayer ? length : std::min(length, slot.spanLength + slot.ringOut);
        nlohmann::json peaksL = nlohmann::json::array();
        nlohmann::json peaksR = nlohmann::json::array();
        const float* wetL = slot.audio->wet[0];
        const float* wetR = slot.audio->wet[1];

        for (int b = 0; b < looper::kPeakBuckets; ++b)
        {
            const std::int64_t from = length * b / looper::kPeakBuckets;
            const std::int64_t to = length * (b + 1) / looper::kPeakBuckets;
            float left = 0.0f;
            float right = 0.0f;

            for (std::int64_t p = from; p < to; ++p)
            {
                const std::int64_t rel = ((p - slot.spanStart) % length + length) % length;

                if (rel >= valid)
                {
                    continue;
                }

                const auto index = static_cast<std::size_t>((slot.head + p) % slot.audio->capacity);
                left = std::max(left, std::abs(wetL[index]));
                right = std::max(right, std::abs(wetR[index]));
            }

            peaksL.push_back(left);
            peaksR.push_back(right);
        }

        layers[LayerId(slot.serial)] = {{"peaksL", std::move(peaksL)}, {"peaksR", std::move(peaksR)}};
        meta->second.peaksSent = true;
    }

    if (layers.empty() && live.is_null())
    {
        return;
    }

    nlohmann::json msg;
    msg["type"] = "looperWaveforms";
    msg["layers"] = std::move(layers);
    msg["generation"] = snapshot.generation;

    if (!live.is_null())
    {
        msg["live"] = std::move(live);
    }

    mSendMessage(msg.dump());
}

int LooperService::FindSlot(const std::string& layerId) const
{
    if (!mHaveSnapshot)
    {
        return -1;
    }

    for (int s = 0; s < looper::kMaxLayers; ++s)
    {
        const auto& slot = mSnapshot.slots[static_cast<std::size_t>(s)];

        if (slot.status != SlotStatus::Free && LayerId(slot.serial) == layerId)
        {
            return s;
        }
    }

    return -1;
}
} // namespace guitarfx
