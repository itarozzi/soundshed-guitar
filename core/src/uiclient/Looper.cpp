/**
 * Looper.cpp - The words and the waveform mix for the looper strip.
 */

#include "uiclient/Looper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace guitarfx::uiclient
{
namespace
{
std::string Clock(double seconds)
{
    const double safe = std::max(0.0, seconds);
    const int minutes = static_cast<int>(safe / 60.0);
    char text[24];
    std::snprintf(text, sizeof(text), "%d:%04.1f", minutes, safe - minutes * 60.0);
    return text;
}
} // namespace

std::string LooperMainButtonLabel(const std::string& state)
{
    if (state == "countIn" || state == "armed")
        return "Rec now";
    if (state == "recording" || state == "stopped" || state == "overdubbing")
        return "Play";
    if (state == "playing")
        return "Dub";
    if (state == "overdubArmed")
        return "Dub now";
    return "Rec";
}

std::string LooperStatusLabel(const std::string& state)
{
    static const std::map<std::string, std::string> kLabels{
        {"empty", "Ready"},     {"countIn", "Count-in"},   {"armed", "Play to start"},       {"recording", "Recording"},
        {"playing", "Playing"}, {"overdubArmed", "Play to overdub"}, {"overdubbing", "Overdubbing"}, {"stopped", "Stopped"}};
    const auto it = kLabels.find(state);
    return it != kLabels.end() ? it->second : std::string("Ready");
}

std::string LooperTimeText(const LooperState& looper, double positionSec)
{
    // The first take has no loop to sit in yet: how long it has run is all there is to show.
    if (looper.lengthSec <= 0.0)
        return looper.state == "recording" ? Clock(looper.recordedSec) : Clock(0.0);
    return Clock(positionSec) + " / " + Clock(looper.lengthSec);
}

std::string LooperBarText(const LooperState& looper)
{
    const std::string layers = std::to_string(looper.layerCount) + (looper.layerCount == 1 ? " layer" : " layers");
    if (looper.bars > 0 && looper.barIndex > 0)
        return "bar " + std::to_string(looper.barIndex) + " of " + std::to_string(looper.bars) + " \xC2\xB7 " + layers;
    return layers;
}

std::string LooperNoticeText(const std::string& notice)
{
    static const std::map<std::string, std::string> kNotices{
        {"disabled", "The looper is off. Turn it on in Settings, Features."},
        {"noCapture", "The looper is still getting ready. Try again in a moment."},
        {"tooLong", "That many bars is longer than the longest loop. Choose fewer bars, or a longer loop in the looper's options."},
        {"noSpare", "The next layer was not ready in time. Press again."},
        {"noLayerFree", "A loop holds 16 layers at most. Remove or undo one first."},
        {"noTempo", "Set a tempo in the metronome first."},
        {"tooShort", "Too short to loop."},
        {"memory", "The looper is out of memory. Remove or undo some layers, or raise its memory limit."},
        {"unknownCommand", "The looper did not understand that press."},
        {"nothingToSave", "There is no loop to save yet."},
        {"saveFailed", "The loop could not be saved."},
        {"loadFailed", "That loop could not be opened."},
        {"exported", "Layer saved as a WAV file."},
        {"exportFailed", "The layer could not be saved as a WAV file."},
        {"retoneFailed", "That preset cannot be rendered offline."},
        {"riffFailed", "That riff could not be read."}};
    const auto it = kNotices.find(notice);
    return it != kNotices.end() ? it->second : std::string{};
}

bool LooperTaking(const std::string& state)
{
    return state == "countIn" || state == "armed" || state == "recording" || state == "overdubArmed" || state == "overdubbing";
}

std::vector<float> LooperMixPeaks(const LooperState& looper)
{
    constexpr std::size_t kBuckets = 256;
    std::vector<float> sum(kBuckets, 0.0f);
    const bool anySolo = std::any_of(looper.layers.begin(), looper.layers.end(), [](const LooperLayer& l) { return l.solo; });
    for (const auto& layer : looper.layers)
    {
        if (layer.recording || layer.muted || (anySolo && !layer.solo))
            continue;
        for (std::size_t b = 0; b < kBuckets; ++b)
        {
            const float left = b < layer.peaksL.size() ? layer.peaksL[b] : 0.0f;
            const float right = b < layer.peaksR.size() ? layer.peaksR[b] : 0.0f;
            sum[b] += std::max(left, right) * static_cast<float>(layer.level);
        }
    }
    // Scaled to the loudest moment, so a stack of layers does not fill the strip solid.
    const float loudest = std::max(1.0f, *std::max_element(sum.begin(), sum.end()));
    for (auto& v : sum)
        v /= loudest;
    return sum;
}

double LooperPositionNow(const LooperState& looper, double nowSeconds)
{
    if (!looper.playing || looper.lengthSec <= 0.0)
        return looper.positionSec;
    const double moved = looper.positionSec + std::max(0.0, nowSeconds - looper.positionAt);
    return std::fmod(moved, looper.lengthSec);
}

float LooperLayerHue(const LooperLayer& layer)
{
    return static_cast<float>((layer.order * 67) % 360);
}
} // namespace guitarfx::uiclient
