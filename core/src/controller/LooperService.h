#pragma once

// LooperService — the looper's message-thread half. docs/plans/looper.md is the design;
// dsp/looper/LooperEngine.h is the realtime half it drives, the mixer's own.
//
// It owns every buffer the engine records into or plays from: the capture buffer (the first layer
// and the Armed pre-roll ring) and the layers, handed to the engine as raw pointers. A buffer is let
// go once no consistent snapshot of the engine refers to it and every command that offered it has
// surely been applied (LooperEngine::BlocksProcessed). It always keeps one spare ready for the next
// overdub: capture-sized until the loop has a length, then exactly the loop's length.
//
// It applies the looper.* app settings to the engine, keeps the chain latency and the alignment D
// current, links the engine to the metronome (the count-in click, the tempo lock, re-phasing on a
// Bars loop's wrap), and sends the UI looperState, looperLayers and looperWaveforms. It answers its
// own messages (RegisterMessageHandlers).
//
// Standalone only: in a plugin it never enables, so the engine stays idle.
//
// Threads: everything here is the message thread, except AfterMixer(), which the audio callback
// calls between the mixer and the metronome.

#include "dsp/looper/LooperEngine.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace guitarfx
{
class IPluginHost;
class MessageHandlerRegistry;
class MetronomeService;
class MultiPresetMixer;

class LooperService
{
  public:
    using SendMessageFn = std::function<void(const std::string&)>;

    /// The preset a layer is played through, as the controller sees it.
    struct Tone
    {
        std::string presetId;
        std::string presetName;
        std::string sceneId;
    };

    struct Settings
    {
        bool enabled = false;
        int maxSeconds = 60; ///< 30, 60, 120 or 300
        int memoryBudgetMb = 512;
        double level = 1.0;
        bool startArmed = false;   ///< looper.start "armed"
        bool lengthBars = false;   ///< looper.length "bars"
        int bars = 4;
        int countInBars = 1;
        bool clickWhileRecording = true;
        bool afterRecordOverdub = false; ///< looper.afterRecord "overdub"
        double armThresholdDb = -50.0;
        double alignOffsetMs = 0.0;
    };

    /// One buffer the engine may point at. Shared, so a background writer (the session folder, an
    /// export) can keep one alive past the service's interest in it.
    struct Buffer
    {
        std::vector<float> di0, di1, wet0, wet1;
        looper::LayerAudio audio;
        std::uint64_t appliedAfter = 0; ///< BlocksProcessed() at which an offer has surely been applied

        Buffer(std::int64_t capacity, int diChannels, bool wet);
        [[nodiscard]] std::size_t Bytes() const noexcept;
    };

    LooperService(IPluginHost& host, MultiPresetMixer& mixer, MetronomeService& metronome, std::mutex& dspMutex,
                  nlohmann::json& appSettings, std::function<void()> saveSettings,
                  std::function<Tone()> currentTone, std::function<void()> metronomeStateChanged,
                  SendMessageFn sendMessage);
    ~LooperService();

    LooperService(const LooperService&) = delete;
    LooperService& operator=(const LooperService&) = delete;

    void RegisterMessageHandlers(MessageHandlerRegistry& registry);

    /// Reads looper.* from the app settings, writing back what it normalised, and applies it.
    void ApplySettingsFromAppSettings();
    /// With the device's rate and block size. Buffers for a new rate are made on the next idle.
    void Prepare(double sampleRate, int maxBlockSize);
    /// Buffers, the engine's latency and alignment, the metronome link, and the UI feeds.
    void OnIdle();

    /// Audio thread, after the mixer and before the metronome renders: a metronome restart the
    /// looper asked for this block, and the click muted through a take when the settings say so.
    void AfterMixer() noexcept;

    [[nodiscard]] const Settings& GetSettings() const noexcept
    {
        return mSettings;
    }

    /// Applies a setLooper payload: any of the settings' fields.
    void ApplySettingsRequest(const nlohmann::json& payload);
    /// A looperCommand: "recPlayDub", "record", "play", "overdub", "stop", "undo", "redo" or "clear".
    void HandleCommand(const std::string& command);
    /// setLooperLayer: {layerId, level?, balance?, muted?, solo?, title?}.
    void HandleSetLayer(const nlohmann::json& payload);
    void HandleRemoveLayer(const std::string& layerId);

    /// The state the UI is sent, as it stands (also what tests read).
    [[nodiscard]] nlohmann::json BuildState() const;
    [[nodiscard]] nlohmann::json BuildLayers() const;
    /// Bytes held in buffers.
    [[nodiscard]] std::size_t HeldBytes() const noexcept;

    static std::string LayerId(std::uint32_t serial);

  private:
    struct LayerMeta
    {
        std::string title;
        Tone tone;
        bool toneChanged = false;
        bool peaksSent = false;
    };

    void StoreSettings();
    void ApplySettingsToEngine();
    void ManageBuffers(const looper::LooperSnapshot& snapshot);
    std::shared_ptr<Buffer> Allocate(std::int64_t capacity, int diChannels);
    void Offer(looper::LooperCommand::Type type, const std::shared_ptr<Buffer>& buffer);
    void TrackLayers(const looper::LooperSnapshot& snapshot);
    void LinkMetronome(const looper::LooperSnapshot& snapshot);
    void SendFeeds(const looper::LooperSnapshot& snapshot);
    void SendWaveforms(const looper::LooperSnapshot& snapshot);
    [[nodiscard]] int FindSlot(const std::string& layerId) const;
    [[nodiscard]] bool Active() const noexcept;

    IPluginHost& mHost;
    MultiPresetMixer& mMixer;
    looper::LooperEngine& mEngine;
    MetronomeService& mMetronome;
    std::mutex& mDSPMutex;
    nlohmann::json& mAppSettings;
    std::function<void()> mSaveSettings;
    std::function<Tone()> mCurrentTone;
    std::function<void()> mMetronomeStateChanged;
    SendMessageFn mSendMessage;

    Settings mSettings;
    std::atomic<double> mSampleRate{0.0};
    std::atomic<bool> mClickWhileRecording{true}; // AfterMixer() reads it

    std::vector<std::shared_ptr<Buffer>> mBuffers;
    /// The capture buffer wanted in the engine (none while disabled), offered until it is adopted.
    std::shared_ptr<Buffer> mWantedCapture;
    std::uint64_t mCaptureOfferApplied = 0;
    bool mCaptureOffered = false;
    /// A spare offered and not yet seen in a snapshot.
    std::shared_ptr<Buffer> mOfferedSpare;
    bool mWasActive = false;
    bool mRedoDropped = false;
    int mLayerNumber = 0;

    looper::LooperSnapshot mSnapshot;
    bool mHaveSnapshot = false;
    std::map<std::uint32_t, LayerMeta> mLayers;
    bool mGuidanceForCountIn = false;
    bool mTempoLocked = false;
    std::string mPendingNotice;

    std::string mLastState;
    std::string mLastLayers;
    std::int64_t mLastPositionSent = -1;
    int mIdleTicks = 0;
};
} // namespace guitarfx
