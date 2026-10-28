// LooperService: exporting a layer, its DI as a riff take to save in the riff library, or its wet
// sound as a WAV file. See LooperService.h.

#include "controller/LooperService.h"

#include "IPluginHost.h"
#include "controller/LooperProjectFiles.h"
#include "controller/MetronomeService.h"
#include "util/FileIO.h"
#include "util/Wav.h"

#include <algorithm>
#include <cmath>

namespace guitarfx
{
namespace lf = looper_files;
using looper::SlotInfo;
using looper::SlotStatus;

void LooperService::HandleExportLayer(const nlohmann::json& payload)
{
    const int s = FindSlot(payload.value("layerId", std::string{}));

    if (s < 0)
    {
        return;
    }

    const SlotInfo slot = mSnapshot.slots[static_cast<std::size_t>(s)];
    const std::int64_t length = mSnapshot.length;
    const auto buffer = FindBuffer(slot.audio);

    if (slot.status != SlotStatus::Visible || !slot.final || !buffer || length <= 0)
    {
        return;
    }

    const double rate = std::max(1.0, mSampleRate.load(std::memory_order_relaxed));
    const LayerMeta meta = mLayers.count(slot.serial) ? mLayers.at(slot.serial) : LayerMeta{};
    lf::LayerCopy copy = lf::ExtractLayer(slot, length);

    if (payload.value("as", std::string{}) == "riff")
    {
        // Riffs are DI, replayed through the rig: the DI over the layer's span, a mono DI on both sides.
        auto take = std::make_shared<RiffTake>();
        take->complete = true;
        take->takeId = "take-" + lf::NewKey();
        take->left = copy.di[0];
        take->right = copy.di.size() > 1 ? copy.di[1] : copy.di[0];
        take->sampleRate = rate;
        take->config.tempoBpm = mMetronome.EffectiveTempoBpm();
        take->config.timeSigNum = mMetronome.TimeSigNum();
        take->config.timeSigDen = mMetronome.TimeSigDen();
        const double samplesPerBar = mMetronome.Clock().samplesPerBar.load(std::memory_order_relaxed);
        take->config.bars =
            samplesPerBar > 1.0
                ? std::max(1, static_cast<int>(std::lround(static_cast<double>(take->left.size()) / samplesPerBar)))
                : 1;
        take->config.countInBars = 0;
        take->config.presetId = meta.tone.presetId;
        take->config.presetName = meta.tone.presetName;
        take->startedAt = take->endedAt = std::chrono::steady_clock::now();

        if (mHooks.offerRiffTake)
        {
            mHooks.offerRiffTake(std::move(take));
        }

        return;
    }

    // The wet sound across the whole loop, aligned to it, to a file the user picks.
    lf::Channels wet(2, std::vector<float>(static_cast<std::size_t>(length)));

    for (std::size_t k = 0; k < copy.wet[0].size(); ++k)
    {
        const auto pos = static_cast<std::size_t>((slot.spanStart + static_cast<std::int64_t>(k)) % length);
        wet[0][pos] = copy.wet[0][k];
        wet[1][pos] = copy.wet[1][k];
    }

    const std::string name = (meta.title.empty() ? std::string("Layer") : meta.title) + ".wav";
    mHost.SaveFileAsync(BrowseFileType::AudioFile, "Export layer", name,
                        [this, wet = std::move(wet), rate](const BrowseFileResult& result) {
                            if (!result.success || result.path.empty())
                            {
                                return;
                            }

                            const std::vector<const std::vector<float>*> channels{&wet[0], &wet[1]};
                            const auto bytes = util::Encode32BitFloatWav(channels, static_cast<int>(std::lround(rate)));
                            mPendingNotice = !bytes.empty() && util::WriteFileAtomic(result.path, bytes)
                                                 ? "exported"
                                                 : "exportFailed";
                            mLastState.clear();
                        });
}
} // namespace guitarfx
