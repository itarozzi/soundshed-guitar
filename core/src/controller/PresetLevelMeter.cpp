/**
 * PresetLevelMeter.cpp — measurePresetLevel's queue, worker and render. See PresetLevelMeter.h
 * for why the render runs on a graph of its own and what it leaves alone; the request is turned
 * into a job by the controller (controller/PluginControllerPresetLevel.cpp).
 */

#include "controller/PresetLevelMeter.h"

#include "IPluginHost.h"
#include "MessageHandlerRegistry.h"
#include "controller/internal/OfflineRenderSupport.h"
#include "dsp/FiniteCheck.h"
#include "dsp/RealtimeParallel.h"
#include "dsp/SignalGraphExecutor.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace guitarfx
{
namespace
{
/// The render runs on past the input until this long a stretch stays under kTailSilencePeak,
/// or kMaxTailSeconds have gone, as OfflineRenderSupport does.
constexpr double kTailSilenceSeconds = 0.25;
constexpr double kMaxTailSeconds = 8.0;
constexpr float kTailSilencePeak = 7.5e-4f;

nlohmann::json LevelOrNull(const std::optional<double>& value)
{
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}
} // namespace

PresetLevelJob::PresetLevelJob() = default;
PresetLevelJob::~PresetLevelJob() = default;
PresetLevelJob::PresetLevelJob(PresetLevelJob&&) noexcept = default;
PresetLevelJob& PresetLevelJob::operator=(PresetLevelJob&&) noexcept = default;

std::optional<ExecutorLevels> RenderExecutorLevels(SignalGraphExecutor& executor, const std::vector<float>& input,
                                                   double sampleRate, int blockSize, float gainLeft, float gainRight,
                                                   const std::atomic<bool>* cancel, std::string& error)
{
    if (input.empty() || sampleRate <= 0.0 || blockSize <= 0)
    {
        error = "Nothing to render";
        return std::nullopt;
    }

    const auto block = static_cast<std::size_t>(blockSize);
    const int latency = std::max(0, executor.GetTotalLatencySamples());
    // Output frame `latency` is the answer to input frame 0, so that is where the window opens.
    const std::size_t windowStart = static_cast<std::size_t>(latency);
    const std::size_t windowEnd = windowStart + input.size();
    const auto tailSilenceFrames = static_cast<std::size_t>(std::ceil(sampleRate * kTailSilenceSeconds));
    const auto maxTailFrames = static_cast<std::size_t>(std::ceil(sampleRate * kMaxTailSeconds));

    loudness::IntegratedLoudnessMeter meter(sampleRate);
    std::vector<float> inL(block), inR(block), outL(block), outR(block);
    float peak = 0.0f;
    std::size_t rendered = 0;
    std::size_t tailFrames = 0;
    std::size_t silentFrames = 0;

    while (true)
    {
        if (cancel && cancel->load(std::memory_order_relaxed))
        {
            error = "Measurement cancelled";
            return std::nullopt;
        }

        // Input runs out partway through a block; the rest of it, and every block after, is silence.
        const std::size_t available = rendered < input.size() ? std::min(block, input.size() - rendered) : 0;
        std::copy_n(input.data() + (available > 0 ? rendered : 0), available, inL.data());
        std::fill(inL.begin() + static_cast<std::ptrdiff_t>(available), inL.end(), 0.0f);
        std::copy(inL.begin(), inL.end(), inR.begin());
        std::fill(outL.begin(), outL.end(), 0.0f);
        std::fill(outR.begin(), outR.end(), 0.0f);

        float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        executor.Process(inputs, outputs, blockSize);

        float blockPeak = 0.0f;

        for (std::size_t i = 0; i < block; ++i)
        {
            if (!IsFinite(outL[i]) || !IsFinite(outR[i]))
            {
                error = "The preset produced non-finite output";
                return std::nullopt;
            }

            outL[i] *= gainLeft;
            outR[i] *= gainRight;
            blockPeak = std::max({blockPeak, std::abs(outL[i]), std::abs(outR[i])});
        }

        peak = std::max(peak, blockPeak);

        // The part of this block inside the measured window.
        const std::size_t from = std::max(rendered, windowStart);
        const std::size_t to = std::min(rendered + block, windowEnd);

        if (from < to)
        {
            meter.Push(outL.data() + (from - rendered), outR.data() + (from - rendered), to - from);
        }

        rendered += block;

        if (rendered < windowEnd)
        {
            continue;
        }

        tailFrames += block;
        silentFrames = blockPeak <= kTailSilencePeak ? silentFrames + block : 0;

        if (silentFrames >= tailSilenceFrames || tailFrames >= maxTailFrames)
        {
            break;
        }
    }

    ExecutorLevels result;
    result.levels = meter.Result();
    result.levels.peakDbfs =
        peak > 0.0f ? std::optional<double>(20.0 * std::log10(static_cast<double>(peak))) : std::nullopt;
    result.latencySamples = latency;
    result.renderedFrames = rendered;
    return result;
}

PresetLevelMeter::PresetLevelMeter(IPluginHost& host, SendMessageFn sendMessage, BuildJobFn buildJob)
    : mHost(host), mBuildJob(std::move(buildJob)), mOutbox(std::make_shared<Outbox>())
{
    mOutbox->send = std::move(sendMessage);
}

PresetLevelMeter::~PresetLevelMeter()
{
    Shutdown();
}

void PresetLevelMeter::RegisterMessageHandlers(MessageHandlerRegistry& registry)
{
    registry.Register("measurePresetLevel",
                      [this](const nlohmann::json& payload) { HandleMeasurePresetLevelRequest(payload); });
}

void PresetLevelMeter::Shutdown()
{
    std::deque<PresetLevelJob> dropped;
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mStopping = true;
        dropped.swap(mQueue);
    }

    mCancel.store(true, std::memory_order_relaxed);
    mQueueCv.notify_all();

    if (mWorker.joinable())
    {
        mWorker.join();
    }

    {
        std::lock_guard<std::mutex> lock(mOutbox->mutex);
        mOutbox->open = false;
    }

    // `dropped` goes here, on the caller's thread, with the graphs it holds.
}

void PresetLevelMeter::Post(const nlohmann::json& message)
{
    // Strings in a request (a preset id, a clip id) came from the UI and should be UTF-8, but one
    // that is not must not throw on a worker thread.
    std::string text = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    mHost.RunOnMainThread([outbox = mOutbox, text = std::move(text)]() {
        std::lock_guard<std::mutex> lock(outbox->mutex);

        if (outbox->open && outbox->send)
        {
            outbox->send(text);
        }
    });
}

void PresetLevelMeter::HandleMeasurePresetLevelRequest(const nlohmann::json& payload)
{
    const auto requestIt = payload.find("requestId");
    const std::string requestId =
        requestIt != payload.end() && requestIt->is_string() ? requestIt->get<std::string>() : std::string{};

    const auto fail = [&](nlohmann::json failure) {
        failure["type"] = "presetLevelMeasureFailed";
        failure["requestId"] = requestId;
        Post(failure);
    };

    nlohmann::json failure = nlohmann::json::object();
    std::optional<PresetLevelJob> job;

    try
    {
        job = mBuildJob ? mBuildJob(payload, failure) : std::nullopt;
    }
    catch (const std::exception& e)
    {
        failure = {{"message", std::string("Could not build the preset: ") + e.what()}};
        job.reset();
    }

    if (!job)
    {
        if (!failure.contains("message"))
        {
            failure["message"] = "Could not build the preset";
        }

        fail(std::move(failure));
        return;
    }

    job->requestId = requestId;
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);

        if (mStopping)
        {
            return;
        }

        if (mQueue.size() >= kMaxQueued)
        {
            // Let `job` go after the lock; its graph is freed here, on the message thread.
            fail({{"message", "Too many level measurements are waiting; try again when they finish"}});
            return;
        }

        mQueue.push_back(std::move(*job));

        if (!mWorker.joinable())
        {
            mWorker = std::thread([this]() { WorkerLoop(); });
        }
    }

    mQueueCv.notify_one();
}

void PresetLevelMeter::WorkerLoop()
{
    rtparallel::FlushDenormalsOnThisThread();
    rtparallel::LowerPriorityOfThisThread();
    const rtparallel::ScopedSharedHelpersRefused offTheSharedHelpers;

    while (true)
    {
        PresetLevelJob job;
        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            mQueueCv.wait(lock, [this]() { return mStopping || !mQueue.empty(); });

            if (mStopping)
            {
                return;
            }

            job = std::move(mQueue.front());
            mQueue.pop_front();
        }

        try
        {
            Run(job);
        }
        catch (const std::exception& e)
        {
            Post({{"type", "presetLevelMeasureFailed"},
                  {"requestId", job.requestId},
                  {"message", std::string("Measurement failed: ") + e.what()}});
        }

        // The graph goes here, on this thread: the controller built it with nothing that must be
        // destroyed on the message thread.
    }
}

const std::vector<float>* PresetLevelMeter::LoadClip(const PresetLevelJob& job, std::string& error)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(job.clipFile, ec);
    const auto time = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(job.clipFile, ec);

    if (ec)
    {
        error = "The demo clip could not be read";
        return nullptr;
    }

    if (!mClip.empty() && mClipFile == job.clipFile && mClipSampleRate == job.sampleRate && mClipFileSize == size &&
        mClipFileTime == time)
    {
        return &mClip;
    }

    mClip.clear();
    std::ifstream stream(job.clipFile, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    if (bytes.empty())
    {
        error = "The demo clip could not be read";
        return nullptr;
    }

    auto prepared = controller_detail::PrepareOfflineRenderBuffer(bytes, job.sampleRate, job.clipId, job.clipId, error);

    if (!prepared || prepared->channelSamples.empty() || prepared->channelSamples.front().empty())
    {
        if (error.empty())
        {
            error = "The demo clip could not be decoded";
        }

        return nullptr;
    }

    mClip = std::move(prepared->channelSamples.front());
    mClipFile = job.clipFile;
    mClipSampleRate = job.sampleRate;
    mClipFileSize = size;
    mClipFileTime = time;
    return &mClip;
}

void PresetLevelMeter::Run(PresetLevelJob& job)
{
    const auto fail = [&](const std::string& message) {
        Post({{"type", "presetLevelMeasureFailed"},
              {"requestId", job.requestId},
              {"presetId", job.presetId},
              {"message", message}});
    };

    std::string error;
    const std::vector<float>* clip = LoadClip(job, error);

    if (!clip || !job.executor)
    {
        fail(error.empty() ? "Nothing to measure" : error);
        return;
    }

    std::vector<float> limited;
    const std::vector<float>* input = clip;

    if (job.maxSeconds > 0.0)
    {
        const auto frames = static_cast<std::size_t>(std::llround(job.maxSeconds * job.sampleRate));

        if (frames > 0 && frames < clip->size())
        {
            limited.assign(clip->begin(), clip->begin() + static_cast<std::ptrdiff_t>(frames));
            input = &limited;
        }
    }

    const auto measured = RenderExecutorLevels(*job.executor, *input, job.sampleRate, job.blockSize, job.slotGainLeft,
                                               job.slotGainRight, &mCancel, error);

    if (!measured)
    {
        if (!mCancel.load(std::memory_order_relaxed))
        {
            fail(error);
        }

        return;
    }

    const auto& levels = measured->levels;
    const std::size_t windowEnd = levels.frames + static_cast<std::size_t>(measured->latencySamples);
    const std::size_t tailFrames = measured->renderedFrames - std::min(measured->renderedFrames, windowEnd);
    Post({{"type", "presetLevelMeasured"},
          {"requestId", job.requestId},
          {"presetId", job.presetId},
          {"sceneId", job.sceneId},
          {"clipId", job.clipId},
          {"integratedLufs", LevelOrNull(levels.integratedLufs)},
          {"peakDbfs", LevelOrNull(levels.peakDbfs)},
          {"rmsDbfs", LevelOrNull(levels.rmsDbfs)},
          {"durationSeconds", levels.DurationSeconds()},
          {"sampleRate", job.sampleRate},
          {"latencySamples", measured->latencySamples},
          {"slotGainDb", 20.0 * std::log10(static_cast<double>(std::max(job.slotGainLeft, 1.0e-6f)))},
          {"tailSeconds", static_cast<double>(tailFrames) / job.sampleRate}});
}
} // namespace guitarfx
