// LooperService: looper projects in the riff library — the session folder written as layers finish,
// save, load, recovery after a crash, and the looper-projects index. See LooperService.h and
// LooperProjectFiles.h; exporting a layer is in LooperServiceExport.cpp.
//
// Slow work (encoding, file I/O, decoding and resampling) runs on the LooperWorker, reading layer
// buffers it holds a shared_ptr to; a finished layer is never written again, so no lock is needed.
// What a job finds comes back to the message thread through Deliver().

#include "controller/LooperService.h"

#include "IPluginHost.h"
#include "controller/LooperProjectFiles.h"
#include "controller/LooperWorker.h"
#include "controller/MetronomeService.h"

#include <algorithm>
#include <cmath>

namespace guitarfx
{
namespace fs = std::filesystem;
namespace lf = looper_files;
using looper::LooperCommand;
using looper::LooperSnapshot;
using looper::LooperState;
using looper::SlotInfo;
using looper::SlotStatus;

namespace
{
constexpr const char* kIndexProjects = "projects";

nlohmann::json ToneJson(const LooperService::Tone& tone)
{
    return {{"presetId", tone.presetId}, {"presetName", tone.presetName}, {"sceneId", tone.sceneId}};
}
} // namespace

void LooperService::SetProjectHooks(ProjectHooks hooks)
{
    mHooks = std::move(hooks);

    if (!mWorker)
    {
        mWorker = std::make_unique<LooperWorker>();
    }
}

void LooperService::Shutdown()
{
    if (mWorker)
    {
        mWorker->Stop();
    }
}

bool LooperService::ProjectWorkPending() const
{
    return mWorker && mWorker->Busy();
}

fs::path LooperService::LoopsRoot() const
{
    if (!mHooks.libraryPath)
    {
        return {};
    }

    const fs::path library = mHooks.libraryPath();
    return library.empty() ? fs::path{} : library / lf::kLoopsFolder;
}

std::shared_ptr<LooperService::Buffer> LooperService::FindBuffer(const looper::LayerAudio* audio) const
{
    for (const auto& buffer : mBuffers)
    {
        if (&buffer->audio == audio)
        {
            return buffer;
        }
    }

    return nullptr;
}

void LooperService::Swap(int slot, std::uint32_t serial, const std::shared_ptr<Buffer>& buffer, bool crossfade,
                         std::int64_t ringOut)
{
    buffer->appliedAfter = mEngine.BlocksProcessed() + 2;
    LooperCommand command;
    command.type = LooperCommand::Type::ReplaceAudio;
    command.slot = slot;
    command.serial = serial;
    command.audio = &buffer->audio;
    command.crossfade = crossfade;
    command.ringOut = ringOut;
    mEngine.Enqueue(command);
}

void LooperService::PrepareSession()
{
    const fs::path root = LoopsRoot();

    if (mSessionPrepared || root.empty())
    {
        return;
    }

    // What the last run left in the session folder is offered back; this run starts a fresh one.
    std::error_code ec;
    fs::create_directories(root, ec);

    if (fs::exists(root / lf::kSessionFolder / lf::kProjectFile, ec))
    {
        fs::remove_all(root / lf::kRecoverFolder, ec);
        fs::rename(root / lf::kSessionFolder, root / lf::kRecoverFolder, ec);
    }

    fs::remove_all(root / lf::kSessionFolder, ec);
    mRecoverable = fs::exists(root / lf::kRecoverFolder / lf::kProjectFile, ec);
    mSessionPrepared = true;
}

std::string LooperService::ProjectKey() const
{
    nlohmann::json key = ProjectJson("", "", false);
    key.erase("id");
    key.erase("title");
    return key.dump();
}

std::vector<std::string> LooperService::SavedLayerKeys() const
{
    std::vector<std::string> keys;

    for (const auto& slot : mSnapshot.slots)
    {
        const auto meta = mLayers.find(slot.serial);

        if (slot.status == SlotStatus::Visible && meta != mLayers.end() && meta->second.sessionWritten)
        {
            keys.push_back(meta->second.key);
        }
    }

    return keys;
}

nlohmann::json LooperService::ProjectJson(const std::string& id, const std::string& title, bool onlyWritten) const
{
    const double rate = mSampleRate.load(std::memory_order_relaxed);
    const auto& clock = mMetronome.Clock();
    const double samplesPerBar = clock.samplesPerBar.load(std::memory_order_relaxed);
    nlohmann::json project = {{"version", lf::kProjectVersion},
                              {"id", id},
                              {"title", title},
                              {"sampleRate", rate},
                              {"lengthSamples", mSnapshot.length}};

    if (mSnapshot.barsLoop && samplesPerBar > 1.0)
    {
        project["tempoBpm"] = mMetronome.EffectiveTempoBpm();
        project["timeSigNum"] = mMetronome.TimeSigNum();
        project["timeSigDen"] = mMetronome.TimeSigDen();
        project["bars"] = static_cast<int>(std::lround(static_cast<double>(mSnapshot.length) / samplesPerBar));
    }

    std::vector<int> order;

    for (int s = 0; s < looper::kMaxLayers; ++s)
    {
        const auto& slot = mSnapshot.slots[static_cast<std::size_t>(s)];
        const auto meta = mLayers.find(slot.serial);

        // On disk, only what has been written: a layer still being written is added once it is.
        if (slot.status == SlotStatus::Visible && slot.final && meta != mLayers.end() &&
            (meta->second.sessionWritten || !onlyWritten))
        {
            order.push_back(s);
        }
    }

    std::sort(order.begin(), order.end(), [this](int a, int b) {
        return mSnapshot.slots[static_cast<std::size_t>(a)].serial < mSnapshot.slots[static_cast<std::size_t>(b)].serial;
    });

    nlohmann::json layers = nlohmann::json::array();

    for (const int s : order)
    {
        const auto& slot = mSnapshot.slots[static_cast<std::size_t>(s)];
        const LayerMeta& meta = mLayers.at(slot.serial);
        const auto& mix = mEngine.Mix(s);
        layers.push_back(
            {{"id", meta.key},
             {"title", meta.title},
             {"order", layers.size()},
             {"firstLayer", slot.firstLayer},
             {"startSample", slot.spanStart},
             {"lengthSamples", slot.firstLayer ? mSnapshot.length : slot.spanLength},
             {"ringOutSamples", slot.firstLayer ? 0 : slot.ringOut},
             {"diChannels", slot.diChannels},
             {"calibrationGainDb", 20.0 * std::log10(std::max(1.0e-6f, slot.calibrationGain))},
             {"latencySamples", slot.latencySamples},
             {"level", mix.level.load()},
             {"balance", mix.balance.load()},
             {"muted", mix.muted.load()},
             {"solo", mix.solo.load()},
             {"wetSource", meta.reamped ? "reamp" : "live"},
             {"riffId", meta.riffId},
             {"tone", ToneJson(meta.tone)},
             {"toneChanged", meta.toneChanged}});
    }

    project["layers"] = std::move(layers);
    return project;
}

void LooperService::ProjectsOnIdle(const LooperSnapshot& snapshot)
{
    if (!mWorker)
    {
        return;
    }

    mWorker->RunDelivered();
    PrepareSession();
    const std::uint64_t blocks = mEngine.BlocksProcessed();

    // Layers sent to the engine (a load, a riff added), once it has them: found by their audio,
    // their names, tones and keys go on. Ones it refused are given up on.
    if (!mPendingMetas.empty())
    {
        for (auto it = mPendingMetas.begin(); it != mPendingMetas.end();)
        {
            const auto slot = std::find_if(snapshot.slots.begin(), snapshot.slots.end(),
                                           [&](const SlotInfo& s) { return s.audio == it->first && s.status != SlotStatus::Free; });

            if (slot != snapshot.slots.end())
            {
                mLayers[slot->serial] = it->second;
                it = mPendingMetas.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (mPendingMetas.empty() && mMarkSavedOnMap)
        {
            mSavedKey = ProjectKey();
            mMarkSavedOnMap = false;
        }

        if (blocks >= mPendingImageApplied + 8)
        {
            mPendingMetas.clear();
            mMarkSavedOnMap = false;
        }
    }

    if (mPendingImage && mPendingMetas.empty() && blocks >= mPendingImageApplied)
    {
        mPendingImage.reset();
    }

    const fs::path root = LoopsRoot();

    if (!Active() || root.empty() || mPendingImage)
    {
        return;
    }

    const fs::path session = root / lf::kSessionFolder;
    const auto rate = static_cast<int>(std::lround(mSampleRate.load(std::memory_order_relaxed)));

    for (int s = 0; s < looper::kMaxLayers; ++s)
    {
        const SlotInfo& slot = snapshot.slots[static_cast<std::size_t>(s)];
        auto meta = mLayers.find(slot.serial);

        if (slot.status != SlotStatus::Visible || !slot.final || !slot.audio || meta == mLayers.end())
        {
            continue;
        }

        if (meta->second.key.empty())
        {
            meta->second.key = lf::NewKey();
        }

        // A first layer leaves the capture buffer for one of its own length, so the next loop can
        // record there and a background writer never reads a buffer that is being reused.
        if (slot.audio == snapshot.capture)
        {
            if (!meta->second.copiedOut)
            {
                auto copy = Allocate(snapshot.length, slot.diChannels > 1 ? 2 : 1);

                if (copy)
                {
                    const lf::LayerCopy whole = lf::ExtractLayer(slot, snapshot.length);
                    copy->di0 = whole.di[0];

                    if (!copy->di1.empty())
                    {
                        copy->di1 = whole.di.size() > 1 ? whole.di[1] : whole.di[0];
                    }

                    copy->wet0 = whole.wet[0];
                    copy->wet1 = whole.wet[1];
                    Swap(s, slot.serial, copy, false);
                    meta->second.copiedOut = true;
                }
            }

            continue;
        }

        if (!meta->second.sessionWritten)
        {
            const auto buffer = FindBuffer(slot.audio);

            if (!buffer)
            {
                continue;
            }

            const std::string key = meta->second.key;
            const std::int64_t length = snapshot.length;
            mWorker->Post([buffer, slot, length, rate, session, key] {
                const lf::LayerCopy copy = lf::ExtractLayer(slot, length);
                (void)lf::WriteLayer(session, key, copy.di, copy.wet, rate);
            });
            meta->second.sessionWritten = true;
        }
    }

    // The session's project.json follows every change, and every layer written, so a crash loses
    // only the take in hand.
    const std::string key = ProjectKey() + "#" + std::to_string(SavedLayerKeys().size());

    if (key != mSessionKey)
    {
        mSessionKey = key;
        const bool empty = SavedLayerKeys().empty();
        nlohmann::json project = ProjectJson(mProjectId, mProjectTitle);
        mWorker->Post([session, empty, project = std::move(project)] {
            std::error_code ec;

            if (empty)
            {
                fs::remove_all(session, ec);
            }
            else
            {
                (void)lf::WriteJson(session / lf::kProjectFile, project);
            }
        });
    }

    // A cleared loop is no longer the project it was loaded from.
    if (snapshot.length == 0 && snapshot.state == LooperState::Empty && snapshot.visibleLayers == 0)
    {
        mProjectId.clear();
        mProjectTitle.clear();
        mSavedKey.clear();
    }
}

void LooperService::UpdateIndex(const nlohmann::json& entry, const std::string& removeId)
{
    if (!mHooks.loadIndex || !mHooks.saveIndex)
    {
        return;
    }

    nlohmann::json index = mHooks.loadIndex();

    if (!index.is_object())
    {
        index = nlohmann::json::object();
    }

    nlohmann::json projects = nlohmann::json::array();
    const std::string id = entry.is_object() ? entry.value("id", std::string{}) : std::string{};

    for (const auto& project : index.value(kIndexProjects, nlohmann::json::array()))
    {
        const std::string existing = project.is_object() ? project.value("id", std::string{}) : std::string{};

        if (!existing.empty() && existing != id && existing != removeId)
        {
            projects.push_back(project);
        }
    }

    if (!id.empty())
    {
        projects.push_back(entry);
    }

    std::sort(projects.begin(), projects.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("modified", std::string{}) > b.value("modified", std::string{});
    });

    index[kIndexProjects] = std::move(projects);
    (void)mHooks.saveIndex(index);
}

void LooperService::SendProjectList()
{
    nlohmann::json index = mHooks.loadIndex ? mHooks.loadIndex() : nlohmann::json::object();
    nlohmann::json msg;
    msg["type"] = "looperProjects";
    msg["projects"] = index.is_object() ? index.value(kIndexProjects, nlohmann::json::array()) : nlohmann::json::array();
    msg["recoverable"] = mRecoverable;
    msg["currentId"] = mProjectId;

    if (mSendMessage)
    {
        mSendMessage(msg.dump());
    }
}

void LooperService::HandleSaveProject(const nlohmann::json& payload)
{
    const std::vector<std::string> keys = SavedLayerKeys();
    const fs::path root = LoopsRoot();

    if (!mWorker || keys.empty() || root.empty())
    {
        mPendingNotice = "nothingToSave";
        mLastState.clear();
        return;
    }

    std::string id = payload.value("projectId", std::string{});
    std::string title = payload.value("title", std::string{});
    id = !id.empty() ? id : (!mProjectId.empty() ? mProjectId : lf::NewKey());
    title = !title.empty() ? title.substr(0, 120) : (!mProjectTitle.empty() ? mProjectTitle : "Loop " + lf::NowIso().substr(0, 10));

    nlohmann::json project = ProjectJson(id, title);
    project["modified"] = lf::NowIso();
    const double rate = std::max(1.0, mSampleRate.load(std::memory_order_relaxed));
    nlohmann::json entry = {{"id", id},
                            {"title", title},
                            {"modified", project["modified"]},
                            {"lengthSec", static_cast<double>(mSnapshot.length) / rate},
                            {"layerCount", keys.size()}};

    if (project.contains("tempoBpm"))
    {
        entry["tempoBpm"] = project["tempoBpm"];
        entry["bars"] = project["bars"];
    }

    const std::string key = ProjectKey();
    const fs::path session = root / lf::kSessionFolder;
    const fs::path target = root / id;
    LooperWorker* worker = mWorker.get();
    mWorker->Post([this, worker, session, target, keys, project, entry, id, title, key] {
        const bool saved = lf::PublishProject(session, target, keys, project);
        worker->Deliver([this, saved, entry, id, title, key] {
            if (!saved)
            {
                mPendingNotice = "saveFailed";
                mLastState.clear();
                return;
            }

            mProjectId = id;
            mProjectTitle = title;
            mSavedKey = key;
            UpdateIndex(entry, "");
            mLastState.clear();

            if (mSendMessage)
            {
                mSendMessage(nlohmann::json{{"type", "looperProjectSaved"}, {"projectId", id}, {"title", title}}.dump());
            }

            SendProjectList();
        });
    });
}

void LooperService::LoadFrom(const fs::path& folder, const std::string& projectId, bool recovering)
{
    if (!mWorker || !Active())
    {
        mPendingNotice = "disabled";
        mLastState.clear();
        return;
    }

    const double rate = mSampleRate.load(std::memory_order_relaxed);
    LooperWorker* worker = mWorker.get();
    mWorker->Post([this, worker, folder, projectId, recovering, rate] {
        auto project = lf::ReadJson(folder / lf::kProjectFile);
        auto image = std::make_unique<looper::LoopImage>();
        std::vector<LayerMeta> metas;
        std::vector<std::shared_ptr<Buffer>> buffers;
        const double saved = project ? project->value("sampleRate", rate) : rate;
        const double ratio = saved > 0.0 ? rate / saved : 1.0;
        const auto scale = [ratio](std::int64_t samples) {
            return static_cast<std::int64_t>(std::llround(static_cast<double>(samples) * ratio));
        };
        const std::int64_t length = project ? scale(project->value("lengthSamples", std::int64_t{0})) : 0;

        for (const auto& layer : project && length > 0 ? project->value("layers", nlohmann::json::array())
                                                       : nlohmann::json::array())
        {
            const std::string key = layer.value("id", std::string{});
            const auto di = lf::ReadWav(lf::DiPath(folder, key), rate);
            const auto wet = lf::ReadWav(lf::WetPath(folder, key), rate);

            if (key.empty() || !di || !wet || di->empty() || wet->size() < 2 || image->layerCount >= looper::kMaxLayers)
            {
                continue;
            }

            const int diChannels = di->size() > 1 ? 2 : 1;
            auto buffer = std::make_shared<Buffer>(length, diChannels, true);
            const bool first = layer.value("firstLayer", false);
            const std::int64_t start = scale(layer.value("startSample", std::int64_t{0})) % length;
            const std::int64_t span = first ? length : std::clamp<std::int64_t>(scale(layer.value("lengthSamples", std::int64_t{0})), 1, length);
            const auto wetLength = static_cast<std::int64_t>(std::min<std::size_t>((*wet)[0].size(), static_cast<std::size_t>(length)));

            for (std::int64_t k = 0; k < wetLength; ++k)
            {
                const auto pos = static_cast<std::size_t>((start + k) % length);
                const auto in = static_cast<std::size_t>(k);
                buffer->wet0[pos] = (*wet)[0][in];
                buffer->wet1[pos] = (*wet)[1][in];

                if (in < (*di)[0].size())
                {
                    buffer->di0[pos] = (*di)[0][in];

                    if (diChannels > 1)
                    {
                        buffer->di1[pos] = (*di)[1][in];
                    }
                }
            }

            looper::LoopLayerImage& slot = image->layers[static_cast<std::size_t>(image->layerCount++)];
            slot.audio = &buffer->audio;
            slot.spanStart = start;
            slot.spanLength = span;
            slot.ringOut = std::max<std::int64_t>(0, wetLength - span);
            slot.latencySamples = layer.value("latencySamples", 0);
            slot.calibrationGain = static_cast<float>(std::pow(10.0, layer.value("calibrationGainDb", 0.0) / 20.0));
            slot.diChannels = diChannels;
            slot.firstLayer = first;
            slot.level = layer.value("level", 1.0f);
            slot.balance = layer.value("balance", 0.0f);
            slot.muted = layer.value("muted", false);
            slot.solo = layer.value("solo", false);

            LayerMeta meta;
            meta.key = key;
            meta.title = layer.value("title", std::string{});
            const auto tone = layer.value("tone", nlohmann::json::object());
            meta.tone = {tone.value("presetId", std::string{}), tone.value("presetName", std::string{}),
                         tone.value("sceneId", std::string{})};
            meta.toneChanged = layer.value("toneChanged", false);
            meta.reamped = layer.value("wetSource", std::string{}) == "reamp";
            meta.riffId = layer.value("riffId", std::string{});
            meta.copiedOut = true;
            metas.push_back(std::move(meta));
            buffers.push_back(std::move(buffer));
        }

        image->length = length;
        image->barsLoop = project && project->contains("bars");
        nlohmann::json details = project ? *project : nlohmann::json::object();
        auto shared = std::make_shared<std::unique_ptr<looper::LoopImage>>(std::move(image));

        worker->Deliver([this, shared, metas, buffers, details, projectId, recovering] {
            auto& loaded = *shared;

            if (!loaded || loaded->layerCount == 0)
            {
                mPendingNotice = "loadFailed";
                mLastState.clear();
                return;
            }

            const std::uint64_t applied = mEngine.BlocksProcessed() + 2;

            for (const auto& buffer : buffers)
            {
                buffer->appliedAfter = applied;
                mBuffers.push_back(buffer);
            }

            // A Bars project brings its tempo and meter, and holds them while it is loaded.
            if (details.contains("tempoBpm"))
            {
                mMetronome.SetTempoLocked(false);
                mTempoLocked = false;
                (void)mMetronome.ApplyRequest({{"bpm", details.value("tempoBpm", 120.0)},
                                               {"timeSigNum", details.value("timeSigNum", 4)},
                                               {"timeSigDen", details.value("timeSigDen", 4)}});

                if (mSaveSettings)
                {
                    mSaveSettings();
                }
            }

            LooperCommand command;
            command.type = LooperCommand::Type::LoadLoop;
            command.loop = loaded.get();
            mEngine.Enqueue(command);
            mPendingImage = std::move(loaded);
            mPendingImageApplied = applied;
            mPendingMetas.clear();
            mLayers.clear();

            for (std::size_t i = 0; i < metas.size() && i < buffers.size(); ++i)
            {
                mPendingMetas.emplace_back(&buffers[i]->audio, metas[i]);
            }

            mLayerNumber = static_cast<int>(metas.size());
            mProjectId = recovering ? std::string{} : projectId;
            mProjectTitle = recovering ? std::string{} : details.value("title", std::string{});
            mMarkSavedOnMap = !recovering;
            mSavedKey.clear();
            mLastState.clear();
            mLastLayers.clear();
        });
    });
}

void LooperService::HandleLoadProject(const std::string& projectId)
{
    const fs::path root = LoopsRoot();

    if (projectId.empty() || root.empty() || projectId.find_first_of("/\\.") != std::string::npos)
    {
        return;
    }

    LoadFrom(root / projectId, projectId, false);
}

void LooperService::HandleRecoverSession()
{
    const fs::path root = LoopsRoot();

    if (!mRecoverable || root.empty())
    {
        return;
    }

    LoadFrom(root / lf::kRecoverFolder, {}, true);
    HandleDismissRecovery();
}

void LooperService::HandleDismissRecovery()
{
    const fs::path root = LoopsRoot();
    mRecoverable = false;

    if (mWorker && !root.empty())
    {
        // After the load that may be reading it, since jobs run in order.
        mWorker->Post([folder = root / lf::kRecoverFolder] {
            std::error_code ec;
            fs::remove_all(folder, ec);
        });
    }

    mLastState.clear();
    SendProjectList();
}

void LooperService::HandleDeleteProject(const std::string& projectId)
{
    const fs::path root = LoopsRoot();

    if (!mWorker || projectId.empty() || root.empty() || projectId.find_first_of("/\\.") != std::string::npos)
    {
        return;
    }

    mWorker->Post([folder = root / projectId] {
        std::error_code ec;
        fs::remove_all(folder, ec);
    });

    if (mProjectId == projectId)
    {
        mProjectId.clear();
        mSavedKey.clear();
        mLastState.clear();
    }

    UpdateIndex(nlohmann::json{}, projectId);
    SendProjectList();
}

void LooperService::HandleRenameProject(const std::string& projectId, const std::string& title)
{
    const fs::path root = LoopsRoot();

    if (!mWorker || projectId.empty() || title.empty() || root.empty() ||
        projectId.find_first_of("/\\.") != std::string::npos)
    {
        return;
    }

    const std::string name = title.substr(0, 120);
    nlohmann::json index = mHooks.loadIndex ? mHooks.loadIndex() : nlohmann::json::object();

    for (const auto& project : index.value(kIndexProjects, nlohmann::json::array()))
    {
        if (project.value("id", std::string{}) == projectId)
        {
            nlohmann::json entry = project;
            entry["title"] = name;
            UpdateIndex(entry, "");
        }
    }

    mWorker->Post([file = root / projectId / lf::kProjectFile, name] {
        if (auto project = lf::ReadJson(file))
        {
            (*project)["title"] = name;
            (void)lf::WriteJson(file, *project);
        }
    });

    if (mProjectId == projectId)
    {
        mProjectTitle = name;
        mLastState.clear();
    }

    SendProjectList();
}

} // namespace guitarfx
