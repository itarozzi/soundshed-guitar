#pragma once

// LooperEngine — the looper's realtime half. docs/plans/looper.md is the design.
//
// A loop is a stack of layers. Each layer holds two recordings made together: the DI (what came in
// from the guitar, before input calibration) and the wet sound (what the chains made of it, moved
// back by the chain latency so a note's DI and wet sit at the same loop position). Playback is a sum
// of the wet recordings with a level and a balance each, so no layer runs a chain.
//
// The mixer calls NoteInput() at its DI tap and Process() at the end of the block, ahead of master
// gain. The engine is JUCE-free, and on the audio thread it never allocates, frees, locks or blocks.
// It owns no layer audio: LooperService owns every buffer and hands the engine raw pointers
// (LayerAudio). The engine publishes which buffers it still points at (ReadSnapshot); a buffer that
// no published snapshot references is the service's to free, because the engine only drops a
// pointer and never takes one back.
//
// The first layer records into the capture buffer, which while the looper is Armed is also its
// pre-roll ring; a layer there is a circular window of it, so a pre-rolled or bar-snapped start needs
// no copy. Every later layer records into the spare the service keeps ready at the loop's length.
//
// Commands from the message thread go through Enqueue() and apply at the start of the next block.
// MIDI and DAW automation already run on the audio thread, between blocks, and call Press().

#include "dsp/TransportClock.h"
#include "util/SpscRingBuffer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace guitarfx::looper
{
inline constexpr int kMaxLayers = 16;
inline constexpr int kPeakBuckets = 256;

/// One layer's audio, or the capture buffer. Owned by LooperService; the engine holds raw pointers.
/// `di[1]` is null for a one-channel DI. Every channel holds `capacity` samples. A capture buffer
/// with no wet channels records the DI alone (a riff take); a layer that plays needs both.
struct LayerAudio
{
    float* di[2] = {nullptr, nullptr};
    float* wet[2] = {nullptr, nullptr};
    std::int64_t capacity = 0;

    [[nodiscard]] int DiChannels() const noexcept
    {
        return di[1] ? 2 : (di[0] ? 1 : 0);
    }
};

enum class LooperState : std::uint8_t
{
    Empty,        ///< No loop.
    CountIn,      ///< Bars + press: counting in, then recording on the downbeat.
    Armed,        ///< Waiting for the first note to start the first layer.
    Recording,    ///< Recording the first layer, which sets the loop's length.
    Playing,      ///< The loop plays.
    OverdubArmed, ///< The loop plays, waiting for the first note to start a layer.
    Overdubbing,  ///< The loop plays while a layer records.
    Stopped,      ///< A loop exists, not playing.
};

enum class LooperAction : std::uint8_t
{
    RecPlayDub, ///< The one-button gesture: record, then play, then overdub, then play...
    Record,
    Play,
    Overdub,
    Stop,
    Undo,
    Redo,
    Clear,
};

struct LooperCommand
{
    enum class Type : std::uint8_t
    {
        Action,      ///< `action`
        SetCapture,  ///< `audio` becomes the capture buffer, once nothing records into the old one
        SetSpare,    ///< `audio` becomes the spare layer buffer (at least the loop's length)
        RemoveLayer, ///< `slot` is hidden, as an undoable step
        DropRedo,    ///< The redo stack is dropped, to give its memory back
    };

    Type type = Type::Action;
    LooperAction action = LooperAction::RecPlayDub;
    int slot = -1;
    const LayerAudio* audio = nullptr;
};

/// Read on the audio thread when a take starts, written from anywhere.
struct LooperSettings
{
    std::atomic<bool> startArmed{false};         ///< looper.start == "armed"
    std::atomic<bool> lengthBars{false};         ///< looper.length == "bars"
    std::atomic<int> bars{4};                    ///< looper.bars
    std::atomic<int> countInBars{1};             ///< looper.countInBars
    /// Count in before every first layer, free and armed ones too, not only a Bars take started on
    /// a press. Riff capture counts in this way; an armed take then listens from the downbeat.
    std::atomic<bool> countInEveryTake{false};
    std::atomic<bool> afterRecordOverdub{false}; ///< looper.afterRecord == "overdub"
    std::atomic<float> armThreshold{0.00316f};   ///< looper.armThresholdDb as a peak (-50 dBFS)
    /// D: how much later a note played in time with the output reaches the DI tap (the device's
    /// output and input latency, plus looper.alignOffsetMs). Overdubs and Bars takes are moved by it.
    std::atomic<int> alignSamples{0};
    /// The chains' latency (MultiPresetMixer::GetTotalLatencySamples), set by the message thread.
    /// A layer keeps the one it started with.
    std::atomic<int> chainLatencySamples{0};
    std::atomic<float> level{1.0f}; ///< looper.level: every layer
    /// Off for a riff take: one layer recorded and never played back, so it closes Stopped, with no
    /// tail written on and nothing faded onto its start.
    std::atomic<bool> playbackEnabled{true};
};

/// A layer's mix, written from anywhere and ramped by the engine. Not part of the audio.
struct LayerMix
{
    std::atomic<float> level{1.0f};
    std::atomic<float> balance{0.0f}; ///< -1 left, +1 right; unity at centre
    std::atomic<bool> muted{false};
    std::atomic<bool> solo{false};
};

enum class SlotStatus : std::uint8_t
{
    Free,      ///< Nothing in it.
    Recording, ///< Being recorded (or armed); not heard.
    Visible,   ///< Part of the loop.
    Hidden,    ///< Undone or removed: on the undo or redo stack, or fading out to be freed.
};

struct SlotInfo
{
    SlotStatus status = SlotStatus::Free;
    const LayerAudio* audio = nullptr;
    std::int64_t head = 0; ///< Where loop position 0 is in `audio` (a window of the capture buffer)
    std::uint32_t serial = 0;
    std::int64_t spanStart = 0; ///< The recorded span, in loop positions
    std::int64_t spanLength = 0;
    std::int64_t ringOut = 0; ///< Wet samples written after the span (its tail)
    int latencySamples = 0;
    float calibrationGain = 1.0f;
    int diChannels = 1;
    bool firstLayer = false;
    bool final = false; ///< Nothing will write to it again
};

struct LooperSnapshot
{
    LooperState state = LooperState::Empty;
    std::int64_t length = 0;
    std::int64_t playhead = 0;
    bool playing = false;
    bool barsLoop = false;
    bool canUndo = false;
    bool canRedo = false;
    int visibleLayers = 0;
    const LayerAudio* capture = nullptr;
    const LayerAudio* spare = nullptr;
    std::array<SlotInfo, kMaxLayers> slots{};
    std::uint64_t generation = 0;
};

class LooperEngine
{
  public:
    // Notices, taken by the service (TakeNotices).
    static constexpr std::uint32_t kNoticeStateChanged = 1u << 0;
    static constexpr std::uint32_t kNoticeLayerClosed = 1u << 1;
    static constexpr std::uint32_t kNoticeLayerFinal = 1u << 2;
    static constexpr std::uint32_t kNoticeNoCapture = 1u << 3;
    static constexpr std::uint32_t kNoticeTooLong = 1u << 4;
    static constexpr std::uint32_t kNoticeNoSpare = 1u << 5;
    static constexpr std::uint32_t kNoticeNoSlot = 1u << 6;
    static constexpr std::uint32_t kNoticeNoTempo = 1u << 7;
    static constexpr std::uint32_t kNoticeTooShort = 1u << 8;
    static constexpr std::uint32_t kNoticeSpareTaken = 1u << 9;

    // Times, in seconds.
    static constexpr double kRampSeconds = 0.010;     ///< Level, mute, undo, stop and clear fades
    static constexpr double kPunchSeconds = 0.005;    ///< Punch-in and punch-out ramps; ring-out's end
    static constexpr double kPreRollSeconds = 0.010;  ///< An armed take starts this far before the note
    static constexpr double kRingOutSeconds = 5.0;    ///< The longest a tail is written on
    static constexpr double kRingOutQuietSeconds = 0.020;
    static constexpr float kRingOutQuietPeak = 3.16e-4f; ///< -70 dBFS
    static constexpr double kArmFloorSeconds = 0.200;    ///< Arming measures the input floor this long
    static constexpr float kArmFloorTriggerPeak = 0.063f; ///< -24 dBFS: plainly playing, even mid-measure
    static constexpr float kArmOverFloor = 3.98f;         ///< +12 dB
    static constexpr double kMinLayerSeconds = 0.050;

    LooperEngine();

    /// Message thread, with the audio stopped: sizes the engine's own buffers.
    void Prepare(double sampleRate, int maxBlockSize);
    /// The clock the metronome writes. Not owned; set before the audio starts.
    void SetTransportClock(const TransportClock* clock) noexcept
    {
        mClock = clock;
    }

    // ── Message thread ──────────────────────────────────────────────────────────────
    /// False when the queue is full; the command is dropped.
    bool Enqueue(const LooperCommand& command);
    LooperSettings& Settings() noexcept
    {
        return mSettings;
    }
    LayerMix& Mix(int slot) noexcept
    {
        return mMix[static_cast<std::size_t>(slot)];
    }

    const LayerMix& Mix(int slot) const noexcept
    {
        return mMix[static_cast<std::size_t>(slot)];
    }
    /// A consistent view of what the last block left, or false when the engine was publishing.
    bool ReadSnapshot(LooperSnapshot& out) const;
    /// The notices raised since the last call.
    [[nodiscard]] std::uint32_t TakeNotices() noexcept
    {
        return mNotices.exchange(0, std::memory_order_acquire);
    }
    /// The recording layer's waveform so far: wet (or DI) peaks per bucket of `LiveBucketSamples`.
    [[nodiscard]] float LivePeak(bool di, int bucket) const noexcept
    {
        return (di ? mLivePeaksDi : mLivePeaksWet)[static_cast<std::size_t>(bucket)].load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::int64_t LiveBucketSamples() const noexcept
    {
        return mLiveBucketSamples.load(std::memory_order_relaxed);
    }
    /// Samples recorded into the layer being recorded, or 0.
    [[nodiscard]] std::int64_t LiveRecordedSamples() const noexcept
    {
        return mLiveRecorded.load(std::memory_order_relaxed);
    }
    /// Blocks the engine has processed. A command enqueued when this read b has been applied once it
    /// reads b + 2: the block running at the time may have drained the queue just before it.
    [[nodiscard]] std::uint64_t BlocksProcessed() const noexcept
    {
        return mBlocksProcessed.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::int64_t PublishedPlayhead() const noexcept
    {
        return mPublishedPlayhead.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::int64_t CountInSamplesLeft() const noexcept
    {
        return mPublishedCountIn.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::int64_t ArmWaitingSamples() const noexcept
    {
        return mPublishedArmWait.load(std::memory_order_relaxed);
    }

    /// Samples from the moment an armed take started listening to where the take begins (its
    /// pre-roll taken off). Set when the take starts; riff capture snaps its trim to the bar with it.
    [[nodiscard]] std::int64_t ArmLeadSamples() const noexcept
    {
        return mPublishedArmLead.load(std::memory_order_relaxed);
    }

    // ── Audio thread ────────────────────────────────────────────────────────────────
    // "Audio thread" means whichever thread runs Process(). A caller that keeps Process() from
    // running — riff capture holds the DSP lock its engine is processed under — may use these too.

    /// A press from MIDI or automation, between blocks: applies at the next block's first sample.
    void Press(LooperAction action);
    /// A command applied now rather than queued, for a caller that excludes Process() as above.
    void Apply(const LooperCommand& command);
    /// The DI tap: after the input fold and channel select, before calibration. `right` is null for
    /// a one-channel DI. `calibrationGain` is the input calibration in force, stored per layer.
    void NoteInput(const float* left, const float* right, int numSamples, float calibrationGain) noexcept;
    /// Records `outL/outR` (the chains' output, or silence) into the layer being recorded, then adds
    /// the audible layers' playback to them. `muted` (the tuner's muted mode) punches out a take and
    /// keeps the loop running silently. Returns whether any playback was added.
    bool Process(float* outL, float* outR, int numSamples, bool muted) noexcept;
    /// Whether the chains must keep running through silence: armed, counting in, recording, or
    /// writing a tail. The mixer's sleep gate asks before it decides.
    [[nodiscard]] bool WantsChainsAwake() const noexcept;
    /// The sample in the block just processed where the metronome should restart its bar (a count-in
    /// started, or a Bars loop wrapped), or -1. Cleared by the call.
    int TakeMetronomeReset() noexcept
    {
        const int offset = mMetronomeReset;
        mMetronomeReset = -1;
        return offset;
    }
    [[nodiscard]] LooperState State() const noexcept
    {
        return mState;
    }

  private:
    struct Slot
    {
        SlotStatus status = SlotStatus::Free;
        const LayerAudio* audio = nullptr;
        std::int64_t head = 0;
        std::int64_t capacity = 0;
        std::uint32_t serial = 0;
        std::int64_t spanStart = 0;
        std::int64_t spanLength = 0;
        std::int64_t ringOut = 0;
        int latency = 0;
        float calibrationGain = 1.0f;
        int diChannels = 1;
        bool firstLayer = false;
        bool final = false;
        bool freeWhenSilent = false;
        int audibleFrom = 0; ///< First sample of this block it may be heard on
        float gainL = 0.0f;
        float gainR = 0.0f;
    };

    enum class WriterPhase : std::uint8_t
    {
        Off,
        Armed,     ///< Writing a ring of loop length, waiting for the trigger (overdub only)
        Recording, ///< Writing DI and wet
        Tail,      ///< DI closed; wet finishing the span, then ringing out
    };

    struct Writer
    {
        WriterPhase phase = WriterPhase::Off;
        int slot = -1;
        std::int64_t recAbs = 0;    ///< DI position of the next sample (unwrapped)
        std::int64_t spanStartAbs = 0;
        std::int64_t spanEndAbs = 0; ///< INT64_MAX while recording
        std::int64_t armStartAbs = 0;
        bool fadeIn = false;
        bool butted = false;     ///< Closed into the next layer: no ring-out
        bool ringOutAdds = false; ///< First layer: the tail is added onto its own start
        bool ringOutStopping = false;
        std::int64_t ringOutCap = 0;
        std::int64_t ringOutFade = 0; ///< Samples of the closing fade written
        std::int64_t quietRun = 0;
        std::int64_t diSeamLeft = 0; ///< First layer: DI added onto its start after the close
    };

    enum class EntryKind : std::uint8_t
    {
        Recorded,
        Removed
    };

    struct Entry
    {
        EntryKind kind = EntryKind::Recorded;
        int slot = -1;
    };

    struct PublishedSlot
    {
        std::atomic<SlotStatus> status{SlotStatus::Free};
        std::atomic<const LayerAudio*> audio{nullptr};
        std::atomic<std::int64_t> head{0};
        std::atomic<std::uint32_t> serial{0};
        std::atomic<std::int64_t> spanStart{0};
        std::atomic<std::int64_t> spanLength{0};
        std::atomic<std::int64_t> ringOut{0};
        std::atomic<int> latency{0};
        std::atomic<float> calibrationGain{1.0f};
        std::atomic<int> diChannels{1};
        std::atomic<bool> firstLayer{false};
        std::atomic<bool> final{false};
    };

    static constexpr int kMaxWriters = 4;
    static constexpr int kMaxEntries = 2 * kMaxLayers;

    /// No loop playing, nothing armed, counting in or being written.
    [[nodiscard]] bool IsIdle() const noexcept;
    void ApplyCommand(const LooperCommand& command);
    void ApplyAction(LooperAction action, int at);
    void StepSample(int i, float wetL, float wetR, bool muted);
    void WriteSample(Writer& w, float diL, float diR, float wetL, float wetR);
    void WriteArmedRing(float diL, float diR, float wetL, float wetR);
    bool CheckArmTrigger(float peak);
    void ResetArmDetector();

    void StartFirstLayer(int at);
    void StartFirstWriter(int at, std::int64_t recAbs);
    void TriggerFirstLayer(int at);
    void CloseFirstLayer(int at, LooperState next);
    void RequestOverdub(int at);
    bool OpenOverdub(int at, std::int64_t recordPos, bool fadeIn, bool armed);
    void TriggerArmedOverdub(int at);
    void CloseOverdub(int at, bool butted);
    void DiscardRecording();
    void CancelFirstLayer();
    void StartPlaying(int at, std::int64_t playhead);
    void BeginStop();
    void BeginClear();
    void FinishClear();
    void Undo();
    void Redo();
    void RemoveLayer(int slot);
    void AfterVisibilityChange();
    void ClearRedo();
    void HideSlot(int slot);
    void ShowSlot(int slot);
    void ReleaseSlot(int slot, bool immediately);
    void ReleaseUnreferenced();
    [[nodiscard]] bool IsReferenced(int slot) const;
    [[nodiscard]] int FindFreeSlot() const;
    [[nodiscard]] int VisibleCount() const;
    Writer* RecordingWriter();
    Writer* NewWriter();
    void KillWritersFor(int slot);
    void SnapGains(int slot);
    void NoteLivePeak(bool di, std::int64_t position, float peak) noexcept;
    void ResetLive(std::int64_t layerSamples) noexcept;

    bool MixPlayback(float* outL, float* outR, int numSamples);
    void UpdateSlotTargets(float* targetsL, float* targetsR) const;
    [[nodiscard]] std::int64_t Index(const Slot& slot, std::int64_t abs) const noexcept;
    [[nodiscard]] float PunchRamp(std::int64_t rel) const noexcept;
    void Publish();
    void Raise(std::uint32_t notice) noexcept
    {
        mNotices.fetch_or(notice, std::memory_order_release);
        mDirty = true;
    }

    // Settings and shared state.
    LooperSettings mSettings;
    std::array<LayerMix, kMaxLayers> mMix{};
    const TransportClock* mClock = nullptr;
    util::SpscRingBuffer<LooperCommand> mCommands{64};

    // Sizes, set by Prepare().
    double mSampleRate = 48000.0;
    int mMaxBlock = 0;
    std::int64_t mRampSamples = 480;
    std::int64_t mPunchSamples = 240;
    std::int64_t mPreRollSamples = 480;
    std::int64_t mRingOutSamples = 240000;
    std::int64_t mQuietSamples = 960;
    std::int64_t mArmFloorSamples = 9600;
    std::int64_t mMinLayerSamples = 2400;
    std::vector<float> mDiL, mDiR;

    // Audio thread.
    int mDiChannels = 1;
    float mCalibrationGain = 1.0f;
    bool mDiNoted = false;
    LooperState mState = LooperState::Empty;
    const LayerAudio* mCapture = nullptr;
    const LayerAudio* mPendingCapture = nullptr;
    const LayerAudio* mSpare = nullptr;
    std::array<Slot, kMaxLayers> mSlots{};
    std::array<Writer, kMaxWriters> mWriters{};
    std::uint32_t mNextSerial = 1;
    std::array<Entry, kMaxEntries> mHistory{};
    std::array<Entry, kMaxEntries> mRedo{};
    int mHistoryCount = 0;
    int mRedoCount = 0;

    std::int64_t mLength = 0;     ///< The loop's length, once the first layer closes
    std::int64_t mPlayhead = 0;   ///< Loop position of the next sample played
    bool mPlaying = false;
    bool mStopping = false;
    bool mClearing = false;
    bool mBarsLoop = false;
    std::int64_t mTargetLength = 0;  ///< A Bars first layer's length, known when it starts
    std::int64_t mClosePlayhead = 0; ///< Where the playhead is when the first layer closes
    bool mFirstBars = false;
    std::int64_t mPendingStart = -1; ///< Samples until the first layer starts recording
    std::int64_t mPendingArm = -1;   ///< Samples until a counted-in armed take starts listening
    std::int64_t mCountInLeft = 0;
    int mAlign = 0;               ///< D, for the overdub being recorded or awaited
    bool mDeferredOverdub = false; ///< Opens at the next record wrap
    bool mWrapPending = false;     ///< The loop wrapped after the last sample
    bool mAnyRecordingWet = false; ///< This sample, a recording layer is taking the wet sound

    // The capture buffer as the first layer's pre-roll ring while Armed.
    bool mRingArmed = false;
    std::int64_t mRingW = 0;
    std::int64_t mRingWritten = 0;
    int mRingLatency = 0;

    // Arm detector.
    std::int64_t mArmFloorLeft = 0;
    float mArmFloor = 0.0f;
    float mArmThreshold = 0.0f;
    std::int64_t mArmWait = 0;

    // This block's playback window.
    int mPlayFrom = -1;
    std::int64_t mPlayheadAtPlayFrom = 0;
    int mMetronomeReset = -1;

    // Published.
    bool mDirty = true;
    std::atomic<std::uint64_t> mSequence{0};
    std::atomic<LooperState> mPublishedState{LooperState::Empty};
    std::atomic<std::int64_t> mPublishedLength{0};
    std::atomic<std::int64_t> mPublishedPlayhead{0};
    std::atomic<bool> mPublishedPlaying{false};
    std::atomic<bool> mPublishedBars{false};
    std::atomic<bool> mPublishedCanUndo{false};
    std::atomic<bool> mPublishedCanRedo{false};
    std::atomic<int> mPublishedVisible{0};
    std::atomic<const LayerAudio*> mPublishedCapture{nullptr};
    std::atomic<const LayerAudio*> mPublishedSpare{nullptr};
    std::array<PublishedSlot, kMaxLayers> mPublishedSlots{};
    std::atomic<std::int64_t> mPublishedCountIn{0};
    std::atomic<std::int64_t> mPublishedArmWait{0};
    std::atomic<std::int64_t> mPublishedArmLead{0};
    std::atomic<std::uint32_t> mNotices{0};
    std::atomic<std::uint64_t> mBlocksProcessed{0};
    std::array<std::atomic<float>, kPeakBuckets> mLivePeaksWet{};
    std::array<std::atomic<float>, kPeakBuckets> mLivePeaksDi{};
    std::atomic<std::int64_t> mLiveBucketSamples{1};
    std::atomic<std::int64_t> mLiveRecorded{0};
};
} // namespace guitarfx::looper
