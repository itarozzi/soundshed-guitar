// LooperService: re-tone — a layer's DI rendered offline through another preset (or the current
// rig), swapped in with a crossfade while the loop plays, and the recorded tone kept to go back to;
// and a riff take brought in as a layer, rendered through the current rig. See LooperRender.h.
//
// The graphs are built on the message thread (ProjectHooks::buildRender) and rendered on the
// looper's worker, one layer after another; the live audio never pays for it. A layer keeps playing
// its old wet sound until the new one is ready.

#include "controller/LooperService.h"

#include "controller/LooperRender.h"
#include "controller/LooperWorker.h"

#include <algorithm>

namespace guitarfx
{
using looper::LooperCommand;
using looper::SlotInfo;
using looper::SlotStatus;

namespace
{
/// The layer's DI across the whole loop, silent outside what was played.
looper_render::Channels LoopDi(const SlotInfo& slot, std::int64_t length)
{
    const looper::LayerAudio& audio = *slot.audio;
    const int channels = std::clamp(slot.diChannels, 1, audio.di[1] ? 2 : 1);
    const std::int64_t span = slot.firstLayer ? length : slot.spanLength;
    looper_render::Channels di(static_cast<std::size_t>(channels),
                               std::vector<float>(static_cast<std::size_t>(length)));

    for (std::int64_t k = 0; k < span; ++k)
    {
        const std::int64_t pos = (slot.spanStart + k) % length;
        const auto index = static_cast<std::size_t>((slot.head + pos) % audio.capacity);

        for (int c = 0; c < channels; ++c)
        {
            di[static_cast<std::size_t>(c)][static_cast<std::size_t>(pos)] = audio.di[c][index];
        }
    }

    return di;
}

std::shared_ptr<LooperService::Buffer> MakeBuffer(const looper_render::Channels& di, looper_render::Channels wet)
{
    const auto length = static_cast<std::int64_t>(wet[0].size());
    auto buffer = std::make_shared<LooperService::Buffer>(length, di.size() > 1 ? 2 : 1, true);
    buffer->di0 = di[0];

    if (!buffer->di1.empty())
    {
        buffer->di1 = di[1];
    }

    buffer->wet0 = std::move(wet[0]);
    buffer->wet1 = std::move(wet[1]);
    // The vectors' storage moved: point at where it is now.
    buffer->audio.di[0] = buffer->di0.data();
    buffer->audio.di[1] = buffer->di1.empty() ? nullptr : buffer->di1.data();
    buffer->audio.wet[0] = buffer->wet0.data();
    buffer->audio.wet[1] = buffer->wet1.data();
    return buffer;
}
} // namespace

void LooperService::HandleRetone(const nlohmann::json& payload)
{
    if (!Active() || !mWorker || !mHaveSnapshot || mSnapshot.length <= 0)
    {
        return;
    }

    // Which layers: an array of ids, or "all".
    std::vector<int> slots;
    const auto ids = payload.find("layerIds");
    const bool all = ids != payload.end() && ids->is_string() && ids->get<std::string>() == "all";

    for (int s = 0; s < looper::kMaxLayers; ++s)
    {
        const SlotInfo& slot = mSnapshot.slots[static_cast<std::size_t>(s)];

        if (slot.status != SlotStatus::Visible || !slot.final || !slot.audio)
        {
            continue;
        }

        bool wanted = all;

        if (!all && ids != payload.end() && ids->is_array())
        {
            wanted = std::any_of(ids->begin(), ids->end(), [&](const nlohmann::json& id) {
                return id.is_string() && id.get<std::string>() == LayerId(slot.serial);
            });
        }

        if (wanted)
        {
            slots.push_back(s);
        }
    }

    // Back to the tone it was recorded with.
    if (payload.value("revert", false))
    {
        for (const int s : slots)
        {
            const SlotInfo& slot = mSnapshot.slots[static_cast<std::size_t>(s)];
            auto meta = mLayers.find(slot.serial);

            if (meta != mLayers.end() && meta->second.recorded)
            {
                Swap(s, slot.serial, meta->second.recorded, true, meta->second.recordedRingOut);
                meta->second.tone = meta->second.recordedTone;
                meta->second.reamped = false;
                meta->second.recorded.reset();
                meta->second.sessionWritten = false;
            }
        }

        mLastLayers.clear();
        return;
    }

    std::string error;
    std::unique_ptr<looper_render::Graphs> graphs =
        mHooks.buildRender ? mHooks.buildRender(payload.value("presetId", std::string{}),
                                                payload.value("sceneId", std::string{}), error)
                           : nullptr;

    if (!graphs)
    {
        mPendingNotice = "retoneFailed";
        mNoticeDetail = error;
        mLastState.clear();
        return;
    }

    struct Item
    {
        SlotInfo slot;
        std::shared_ptr<Buffer> buffer;
        std::shared_ptr<std::atomic<float>> progress;
    };

    std::vector<Item> items;

    for (const int s : slots)
    {
        const SlotInfo& slot = mSnapshot.slots[static_cast<std::size_t>(s)];
        auto meta = mLayers.find(slot.serial);
        auto buffer = FindBuffer(slot.audio);

        if (meta == mLayers.end() || !buffer || meta->second.retoneProgress)
        {
            continue;
        }

        meta->second.retoneProgress = std::make_shared<std::atomic<float>>(0.0f);
        items.push_back({slot, std::move(buffer), meta->second.retoneProgress});
    }

    const Tone tone{graphs->presetId, graphs->presetName, graphs->sceneId};
    const std::int64_t length = mSnapshot.length;
    LooperWorker* worker = mWorker.get();
    auto shared = std::shared_ptr<looper_render::Graphs>(std::move(graphs));
    mLastLayers.clear();

    // One layer at a time; each lands as soon as it is rendered.
    for (const Item& item : items)
    {
        mWorker->Post([this, worker, shared, item, tone, length] {
            const looper_render::Channels di = LoopDi(item.slot, length);
            looper_render::Channels wet =
                looper_render::RenderLoop(*shared, di, item.slot.calibrationGain, item.progress.get());
            auto buffer = wet.empty() ? nullptr : MakeBuffer(di, std::move(wet));

            worker->Deliver([this, item, tone, length, buffer] {
                auto meta = mLayers.find(item.slot.serial);

                if (meta == mLayers.end())
                {
                    return;
                }

                meta->second.retoneProgress.reset();
                mLastLayers.clear();
                const auto slot = std::find_if(mSnapshot.slots.begin(), mSnapshot.slots.end(),
                                               [&](const SlotInfo& s) { return s.serial == item.slot.serial; });

                if (!buffer || slot == mSnapshot.slots.end() || slot->status == SlotStatus::Free)
                {
                    return;
                }

                // The live recording is kept, once, so the recorded tone can come back.
                if (!meta->second.reamped)
                {
                    meta->second.recorded = FindBuffer(slot->audio);
                    meta->second.recordedRingOut = slot->ringOut;
                    meta->second.recordedTone = meta->second.tone;
                }

                buffer->appliedAfter = mEngine.BlocksProcessed() + 2;
                mBuffers.push_back(buffer);
                const auto index = static_cast<int>(slot - mSnapshot.slots.begin());
                const std::int64_t span = slot->firstLayer ? length : slot->spanLength;
                // A rendered layer sounds across the whole loop: its tails wrap.
                Swap(index, slot->serial, buffer, true, length - span);
                meta->second.tone = tone;
                meta->second.reamped = true;
                meta->second.toneChanged = false;
                meta->second.sessionWritten = false;
            });
        });
    }
}

void LooperService::HandleAddRiff(const std::string& takeId)
{
    if (!Active() || !mWorker || takeId.empty() || !mHooks.loadRiffTake)
    {
        return;
    }

    std::string error;
    std::unique_ptr<looper_render::Graphs> graphs = mHooks.buildRender ? mHooks.buildRender({}, {}, error) : nullptr;

    if (!graphs)
    {
        mPendingNotice = "retoneFailed";
        mNoticeDetail = error;
        mLastState.clear();
        return;
    }

    const double rate = mSampleRate.load(std::memory_order_relaxed);
    const std::int64_t loopLength = mHaveSnapshot ? mSnapshot.length : 0;
    const float calibration = mHooks.calibrationGain ? mHooks.calibrationGain() : 1.0f;
    const Tone tone{graphs->presetId, graphs->presetName, graphs->sceneId};
    auto shared = std::shared_ptr<looper_render::Graphs>(std::move(graphs));
    auto load = mHooks.loadRiffTake;
    LooperWorker* worker = mWorker.get();

    mWorker->Post([this, worker, shared, load, takeId, rate, loopLength, calibration, tone] {
        looper_render::Channels audio;
        std::string title;

        if (!load(takeId, rate, audio, title) || audio.empty() || audio[0].empty())
        {
            worker->Deliver([this] {
                mPendingNotice = "riffFailed";
                mLastState.clear();
            });
            return;
        }

        // Fitted to the loop: padded with silence, or cut at its end. With no loop, it is the loop.
        const std::int64_t length = loopLength > 0 ? loopLength : static_cast<std::int64_t>(audio[0].size());
        looper_render::Channels di(2, std::vector<float>(static_cast<std::size_t>(length)));
        const auto copy =
            static_cast<std::size_t>(std::min<std::int64_t>(length, static_cast<std::int64_t>(audio[0].size())));

        for (std::size_t c = 0; c < 2; ++c)
        {
            const auto& source = audio[std::min(c, audio.size() - 1)];
            std::copy_n(source.begin(), copy, di[c].begin());
        }

        auto buffer = MakeBuffer(di, looper_render::RenderLoop(*shared, di, calibration));
        auto image = std::make_shared<looper::LoopImage>();
        image->length = length;
        image->layerCount = 1;
        looper::LoopLayerImage& layer = image->layers[0];
        layer.audio = &buffer->audio;
        layer.spanStart = 0;
        layer.spanLength = length;
        layer.diChannels = 2;
        layer.calibrationGain = calibration;
        layer.firstLayer = loopLength <= 0;

        worker->Deliver([this, buffer, image, title, tone, takeId] {
            const std::uint64_t applied = mEngine.BlocksProcessed() + 2;
            buffer->appliedAfter = applied;
            mBuffers.push_back(buffer);
            // Held until the engine has surely read it.
            mHeldImages.emplace_back(std::make_unique<looper::LoopImage>(*image), applied);
            LooperCommand command;
            command.type = LooperCommand::Type::AddLayer;
            command.loop = mHeldImages.back().first.get();
            mEngine.Enqueue(command);
            mPendingImageApplied = applied;
            LayerMeta meta;
            meta.title = title.empty() ? "Riff" : title;
            meta.tone = tone;
            meta.reamped = true;
            meta.riffId = takeId;
            meta.copiedOut = true;
            mPendingMetas.emplace_back(&buffer->audio, std::move(meta));
            mLastLayers.clear();
        });
    });
}
} // namespace guitarfx
