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

#include "controller/RiffTake.h"
#include "dsp/looper/LooperEngine.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace guitarfx
{
class IPluginHost;
class LooperWorker;
namespace looper_render
{
struct Graphs;
}
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

    /// What projects need from the controller: where the riff library is, its looper-projects
    /// index in the document store, and a way to hand a layer's DI to riff capture as a take.
    struct ProjectHooks
    {
        std::function<std::filesystem::path()> libraryPath;
        std::function<nlohmann::json()> loadIndex;
        std::function<bool(const nlohmann::json&)> saveIndex;
        std::function<void(std::shared_ptr<RiffTake>)> offerRiffTake;
        /// Re-tone's graphs: the preset and scene (the active ones when empty) with the global chains.
        /// Message thread. Null with the reason in `error` when it cannot be rendered offline.
        std::function<std::unique_ptr<looper_render::Graphs>(const std::string& presetId,
                                                             const std::string& sceneId, std::string& error)>
            buildRender;
        /// A riff take's audio at `sampleRate`, two channels, and its riff's title. Worker thread.
        std::function<bool(const std::string& takeId, double sampleRate, std::vector<std::vector<float>>& audio,
                           std::string& title)>
            loadRiffTake;
        /// The input calibration in force, for a riff added as a layer (riffs are stored raw).
        std::function<float()> calibrationGain;
    };

    void SetProjectHooks(ProjectHooks hooks);
    /// Finishes the job in hand and stops the worker. Called before the controller goes.
    void Shutdown();

    void RegisterMessageHandlers(MessageHandlerRegistry& registry);

    /// Reads looper.* from the app settings, writing back what it normalised, and applies it.
    void ApplySettingsFromAppSettings();
    /// With the device's rate and block size. Buffers for a new rate are made on the next idle.
    void Prepare(double sampleRate, int maxBlockSize);
    /// Buffers, the engine's latency and alignment, the metronome link, and the UI feeds.
    void OnIdle();

    /// Audio thread, after the mixer and before the metronome renders: how the click follows the
    /// loop this block (LoopClickSync), and the click muted through a take when the settings say so.
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

    // Projects (LooperServiceProjects.cpp).
    /// saveLooperProject: {projectId?, title?}. Without an id, the loaded project, else a new one.
    void HandleSaveProject(const nlohmann::json& payload);
    void HandleLoadProject(const std::string& projectId);
    void HandleDeleteProject(const std::string& projectId);
    void HandleRenameProject(const std::string& projectId, const std::string& title);
    void HandleRecoverSession();
    void HandleDismissRecovery();
    /// exportLooperLayer: {layerId, as: "riff" | "wetWav"}.
    void HandleExportLayer(const nlohmann::json& payload);
    void SendProjectList();

    // Re-tone (LooperServiceRetone.cpp).
    /// retoneLooperLayer: {layerIds: [...] | "all", presetId?, sceneId?, revert?}.
    void HandleRetone(const nlohmann::json& payload);
    /// addRiffAsLooperLayer: {takeId}. A new loop of the take's length when there is none.
    void HandleAddRiff(const std::string& takeId);
    /// True while the session writer, a save or a load is still working (tests wait on it).
    [[nodiscard]] bool ProjectWorkPending() const;

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
        std::string key;            ///< Its name on disk, kept across save and load
        bool sessionWritten = false; ///< Its files are in the session folder
        bool copiedOut = false;      ///< A first layer moved out of the capture buffer
        bool reamped = false;        ///< Its wet sound was rendered, not played live
        std::string riffId;          ///< The riff take it came from, if one
        /// The live recording a re-tone replaced, kept so the recorded tone can come back.
        std::shared_ptr<Buffer> recorded;
        std::int64_t recordedRingOut = 0;
        Tone recordedTone;
        /// A re-tone in progress (0..1), shared with the worker; null when none is.
        std::shared_ptr<std::atomic<float>> retoneProgress;
    };

    void ProjectsOnIdle(const looper::LooperSnapshot& snapshot);
    void PrepareSession();
    [[nodiscard]] std::filesystem::path LoopsRoot() const;
    [[nodiscard]] std::shared_ptr<Buffer> FindBuffer(const looper::LayerAudio* audio) const;
    [[nodiscard]] nlohmann::json ProjectJson(const std::string& id, const std::string& title,
                                             bool onlyWritten = true) const;
    [[nodiscard]] std::vector<std::string> SavedLayerKeys() const;
    [[nodiscard]] std::string ProjectKey() const;
    void LoadFrom(const std::filesystem::path& folder, const std::string& projectId, bool recovering);
    void UpdateIndex(const nlohmann::json& entry, const std::string& removeId);
    void Swap(int slot, std::uint32_t serial, const std::shared_ptr<Buffer>& buffer, bool crossfade,
              std::int64_t ringOut = -1);

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

    // Projects.
    ProjectHooks mHooks;
    std::unique_ptr<LooperWorker> mWorker;
    bool mSessionPrepared = false;
    bool mRecoverable = false;
    std::string mProjectId;
    std::string mProjectTitle;
    std::string mSavedKey;   ///< ProjectKey() when last saved or loaded
    std::string mSessionKey; ///< ProjectKey() when the session's project.json was last written
    bool mMarkSavedOnMap = false;
    std::unique_ptr<looper::LoopImage> mPendingImage;
    std::uint64_t mPendingImageApplied = 0;
    /// Layers sent to the engine (a load, a riff added) whose names, tones and keys go on once it has
    /// them, found by their audio.
    std::vector<std::pair<const looper::LayerAudio*, LayerMeta>> mPendingMetas;
    std::string mNoticeDetail;
    /// Images a command points at (a riff added), kept until the engine has surely applied it.
    std::vector<std::pair<std::unique_ptr<looper::LoopImage>, std::uint64_t>> mHeldImages;

    std::string mLastState;
    std::string mLastLayers;
    std::int64_t mLastPositionSent = -1;
    int mIdleTicks = 0;
};
} // namespace guitarfx
