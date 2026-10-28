/**
 * Riffs.cpp - Reading the riff library, and the words for a riff and a capture.
 */

#include "uiclient/Riffs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace guitarfx::uiclient
{
namespace
{
std::string StringOf(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

double NumberOf(const nlohmann::json& object, const char* key, double fallback = 0.0)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_number() ? it->get<double>() : fallback;
}

std::vector<std::string> StringsOf(const nlohmann::json& object, const char* key)
{
    std::vector<std::string> out;

    if (const auto it = object.find(key); it != object.end() && it->is_array())
    {
        for (const auto& v : *it)
        {
            if (v.is_string() && !v.get<std::string>().empty())
            {
                out.push_back(v.get<std::string>());
            }
        }
    }

    return out;
}

/// Samples in a bar at the take's tempo and meter, or 0 when it has none.
double SamplesPerBar(const RiffCapture& capture, double sampleRate)
{
    if (capture.tempoBpm <= 0.0 || sampleRate <= 0.0 || capture.timeSigDen <= 0)
    {
        return 0.0;
    }

    return sampleRate * 60.0 / capture.tempoBpm * (4.0 / capture.timeSigDen) * capture.timeSigNum;
}

std::string Clock(double seconds)
{
    const int total = static_cast<int>(std::lround(std::max(0.0, seconds)));
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}

std::string Join(const std::vector<std::string>& items)
{
    std::string out;

    for (const auto& item : items)
    {
        out += (out.empty() ? "" : ", ") + item;
    }

    return out;
}
} // namespace

const RiffTake* Riff::PreferredTake() const
{
    for (const auto& take : takes)
    {
        if (take.id == preferredTakeId)
        {
            return &take;
        }
    }

    return takes.empty() ? nullptr : &takes.front();
}

const Riff* RiffState::Find(const std::string& riffId) const
{
    for (const auto& riff : riffs)
    {
        if (riff.id == riffId)
        {
            return &riff;
        }
    }

    return nullptr;
}

std::vector<Riff> ParseRiffs(const nlohmann::json& library)
{
    std::vector<Riff> riffs;
    const auto list = library.find("riffs");

    if (list == library.end() || !list->is_array())
    {
        return riffs;
    }

    for (const auto& item : *list)
    {
        if (!item.is_object())
        {
            continue;
        }

        Riff riff;
        riff.id = StringOf(item, "id");
        riff.title = StringOf(item, "title");
        riff.categories = StringsOf(item, "categories");
        riff.tags = StringsOf(item, "tags");
        riff.notes = StringOf(item, "notes");
        riff.favorite = item.value("favorite", false);
        riff.used = item.value("used", false) || !StringOf(item, "usedAt").empty();
        riff.usedSongTitle = StringOf(item, "usedSongTitle");
        riff.preferredTakeId = StringOf(item, "preferredTakeId");

        if (const auto takes = item.find("takes"); takes != item.end() && takes->is_array())
        {
            for (const auto& t : *takes)
            {
                if (!t.is_object())
                {
                    continue;
                }

                RiffTake take;
                take.id = StringOf(t, "id");
                take.filePath = StringOf(t, "filePath");
                take.durationSec = NumberOf(t, "durationSec");
                take.bars = static_cast<int>(NumberOf(t, "bars"));
                take.tempoBpm = NumberOf(t, "tempoBpm");
                take.timeSigNum = static_cast<int>(NumberOf(t, "timeSigNum", 4));
                take.timeSigDen = static_cast<int>(NumberOf(t, "timeSigDen", 4));
                take.presetName = StringOf(t, "presetName");
                take.createdAt = StringOf(t, "createdAt");

                if (!take.id.empty())
                {
                    riff.takes.push_back(std::move(take));
                }
            }
        }

        if (!riff.id.empty())
        {
            riffs.push_back(std::move(riff));
        }
    }

    return riffs;
}

double RiffCaptureFill(const RiffCapture& capture, double sampleRate)
{
    const double rate = capture.sampleRate > 0.0 ? capture.sampleRate : sampleRate;
    const double buffer = SamplesPerBar(capture, rate) * kRiffCaptureMaxBars;

    if (buffer <= 0.0)
    {
        return 0.0;
    }

    // The engine's buckets are ceil(buffer / 256) samples each.
    const double span = 256.0 * std::ceil(buffer / 256.0);
    return std::clamp(static_cast<double>(capture.capturedSamples) / span, 0.0, 1.0);
}

double RiffCaptureSeconds(const RiffCapture& capture, double sampleRate)
{
    const double rate = capture.sampleRate > 0.0 ? capture.sampleRate : sampleRate;
    return rate > 0.0 ? static_cast<double>(capture.capturedSamples) / rate : 0.0;
}

std::string RiffCaptureStatus(const RiffCapture& capture, double sampleRate)
{
    char text[160];

    switch (capture.phase)
    {
    case RiffCapture::Phase::Armed:
        std::snprintf(text, sizeof(text), "Armed \xC2\xB7 %.0f BPM \xC2\xB7 count-in, then waiting for you to play",
                      capture.tempoBpm);
        return text;
    case RiffCapture::Phase::Recording: {
        const double perBar = SamplesPerBar(capture, capture.sampleRate > 0.0 ? capture.sampleRate : sampleRate);
        const int bars = perBar > 0.0 ? std::max(1, static_cast<int>(std::ceil(capture.capturedSamples / perBar))) : 0;
        std::snprintf(text, sizeof(text), "Recording \xC2\xB7 ~%d bar%s @ %.0f BPM", bars, bars == 1 ? "" : "s",
                      capture.tempoBpm);
        return text;
    }
    case RiffCapture::Phase::Complete:

        if (!capture.hasAudio)
        {
            return "Nothing was recorded";
        }

        std::snprintf(text, sizeof(text), "Captured \xC2\xB7 %.1f s, %d bar%s", RiffCaptureSeconds(capture, sampleRate),
                      capture.bars, capture.bars == 1 ? "" : "s");
        return text;
    case RiffCapture::Phase::Idle:
        break;
    }

    return "Ready";
}

std::string RiffTakeSummary(const RiffTake& take)
{
    char text[96];
    std::snprintf(text, sizeof(text), "%d bar%s \xC2\xB7 %.0f BPM \xC2\xB7 %d/%d \xC2\xB7 ", take.bars,
                  take.bars == 1 ? "" : "s", take.tempoBpm, take.timeSigNum, take.timeSigDen);
    return text + Clock(take.durationSec);
}

std::string RiffMeta(const Riff& riff)
{
    std::string meta;

    if (!riff.categories.empty())
    {
        meta += Join(riff.categories) + " \xC2\xB7 ";
    }

    if (!riff.tags.empty())
    {
        meta += "Tags: " + Join(riff.tags) + " \xC2\xB7 ";
    }

    meta += riff.used ? (riff.usedSongTitle.empty() ? std::string("Used") : "Used (" + riff.usedSongTitle + ")")
                      : std::string("Unused");
    meta += " \xC2\xB7 Takes: " + std::to_string(riff.takes.size());
    return meta;
}

std::vector<std::string> SplitRiffList(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t start = 0;

    while (start <= text.size())
    {
        auto end = text.find(',', start);

        if (end == std::string::npos)
        {
            end = text.size();
        }

        auto item = text.substr(start, end - start);
        const auto first = item.find_first_not_of(" \t");
        const auto last = item.find_last_not_of(" \t");

        if (first != std::string::npos)
        {
            out.push_back(item.substr(first, last - first + 1));
        }

        start = end + 1;
    }

    return out;
}

std::string FormatRiffSeconds(double seconds)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%.2f s", std::max(0.0, seconds));
    return text;
}
} // namespace guitarfx::uiclient
