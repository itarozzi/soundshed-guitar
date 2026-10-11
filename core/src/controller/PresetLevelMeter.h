#pragma once

// PresetLevelMeter — measurePresetLevel: how loud a preset plays the demo DI, measured offline.
//
// The tone builder trims the presets it makes to -18 LUFS, so it needs to know how loud one
// is without anyone playing it. A request names a preset (sent whole, or by id) and optionally
// a scene and a demo clip; the answer is the BS.1770 integrated loudness, sample peak and RMS
// of that preset playing the clip (dsp/Loudness.h).
//
// It never borrows the live mixer. renderDemoAudio does (OfflineRenderSupport), which is why
// it silences what is playing for as long as it renders; this must not, since the tone builder
// measures while the player plays on. So each request gets a graph of its own, built from the
// preset exactly as a mixer slot's is (MultiPresetMixer::BuildDetachedExecutor) but never
// installed. The controller builds and prepares it on the message thread, without the DSP lock,
// as a preset load does, because that is where the resource library and the effect registry
// may be read. Rendering it then runs here, on one worker thread of our own:
//  - below normal priority, so the audio thread and the UI come first;
//  - refusing the DSP helpers the live chain's effects share (rtparallel::DualLaneExecutor),
//    so the audio thread never finds one busy with our work, and with the graph's own
//    level fan-out off;
//  - never taking the DSP lock, and never touching anything the audio thread reads.
// Requests queue, one render at a time; a queue already kMaxQueued deep refuses more.
//
// Answers go back through IPluginHost::RunOnMainThread, so the UI hears from the message thread
// as it does for everything else. Shutdown() (called from ~PluginController) cancels the render
// in progress, drops what is queued and joins the worker; an answer posted before then that the
// message thread has not run yet is dropped rather than sent through a controller being torn down.
//
// What a measurement plays and why:
//  - The clip's first channel, as the app's mono input takes it, at the host's sample rate (or
//    48 kHz without one), in 512-frame blocks, into a mono graph input.
//  - Loudness and RMS cover exactly the clip's length, moved on by the graph's reported latency.
//    The render runs on past the end until the tail has died away (or 8 s), as renderDemoAudio
//    does, and the peak covers that tail too, since a delay's repeat can be the loudest sample.
//  - The preset as a lone rig plays it: its output through the mix's equal-power pan law at
//    the default mix and pan, which puts a centred rig 3 dB down on each side (slotGainDb in
//    the answer). That is what a player hears and what the content pipeline's AudioSnapshot
//    render measures, so -18 LUFS means the same on both. The global chain, input calibration
//    gain, master gain and limiter are the player's, not the preset's, and are left out; at
//    their defaults they change nothing. NAM nodes get the reference interface calibration
//    (12 dBu) whatever this player set, so a preset measures the same for everyone, as that
//    render at default settings does.

#include "dsp/Loudness.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class IPluginHost;
class MessageHandlerRegistry;
class SignalGraphExecutor;

/// One measurement as the message thread hands it over: the graph built and prepared, and the
/// clip still a file to read.
struct PresetLevelJob
{
    std::string requestId;
    std::string presetId;
    std::string sceneId;
    std::string clipId;
    std::filesystem::path clipFile;
    double sampleRate = 48000.0;
    int blockSize = 512;
    /// Measure only this much of the clip; 0 for all of it.
    double maxSeconds = 0.0;
    /// What the mix does to a lone rig's output on each side (its pan law at the default mix and
    /// pan), so the level is the one heard.
    float slotGainLeft = 1.0f;
    float slotGainRight = 1.0f;
    /// Prepared at sampleRate and blockSize. Holds no node that needs the message thread.
    std::unique_ptr<SignalGraphExecutor> executor;

    PresetLevelJob();
    ~PresetLevelJob();
    PresetLevelJob(PresetLevelJob&&) noexcept;
    PresetLevelJob& operator=(PresetLevelJob&&) noexcept;
};

/// What one render measured.
struct ExecutorLevels
{
    /// Loudness and RMS over the input's length, latency compensated; the peak over the whole
    /// render, tail included.
    loudness::LevelMeasurement levels;
    int latencySamples = 0;
    /// Everything rendered: the input's length, the latency and the tail.
    std::size_t renderedFrames = 0;
};

/// Renders `input` (mono, at `sampleRate`) through `executor`, prepared at that rate for blocks
/// of at least `blockSize`, scales each side of its output by `gainLeft`/`gainRight`, and
/// measures the result (see the header comment). Nullopt with `error` set when `cancel` is
/// raised or the graph puts out anything non-finite.
[[nodiscard]] std::optional<ExecutorLevels> RenderExecutorLevels(SignalGraphExecutor& executor,
                                                                 const std::vector<float>& input, double sampleRate,
                                                                 int blockSize, float gainLeft, float gainRight,
                                                                 const std::atomic<bool>* cancel, std::string& error);

class PresetLevelMeter
{
  public:
    using SendMessageFn = std::function<void(const std::string&)>;
    /// Message thread: turns a request into a job, or fills `failure` ({message, ...}) and
    /// returns nullopt.
    using BuildJobFn =
        std::function<std::optional<PresetLevelJob>(const nlohmann::json& request, nlohmann::json& failure)>;

    /// How many requests may wait behind the one rendering.
    static constexpr std::size_t kMaxQueued = 8;

    PresetLevelMeter(IPluginHost& host, SendMessageFn sendMessage, BuildJobFn buildJob);
    ~PresetLevelMeter();

    PresetLevelMeter(const PresetLevelMeter&) = delete;
    PresetLevelMeter& operator=(const PresetLevelMeter&) = delete;

    /// Registers "measurePresetLevel" with the dispatcher.
    void RegisterMessageHandlers(MessageHandlerRegistry& registry);

    /// Cancels the render in progress, drops the queue and joins the worker. Idempotent.
    void Shutdown();

  private:
    /// Answers already posted to the message thread check this before sending, so one that
    /// runs after Shutdown() is dropped.
    struct Outbox
    {
        std::mutex mutex;
        bool open = true;
        SendMessageFn send;
    };

    void HandleMeasurePresetLevelRequest(const nlohmann::json& payload);
    void WorkerLoop();
    /// Worker thread: decodes the clip (reusing the last one), renders, and answers.
    void Run(PresetLevelJob& job);
    /// The clip's first channel at `sampleRate`, from the cache when it is the last one decoded.
    const std::vector<float>* LoadClip(const PresetLevelJob& job, std::string& error);
    /// Any thread: hands `message` to the message thread to send.
    void Post(const nlohmann::json& message);

    IPluginHost& mHost;
    BuildJobFn mBuildJob;
    std::shared_ptr<Outbox> mOutbox;

    std::mutex mQueueMutex;
    std::condition_variable mQueueCv;
    std::deque<PresetLevelJob> mQueue;
    bool mStopping = false;
    std::atomic<bool> mCancel{false};
    std::thread mWorker;

    // Worker thread only.
    std::filesystem::path mClipFile;
    double mClipSampleRate = 0.0;
    std::uintmax_t mClipFileSize = 0;
    std::filesystem::file_time_type mClipFileTime{};
    std::vector<float> mClip;
};
} // namespace guitarfx
