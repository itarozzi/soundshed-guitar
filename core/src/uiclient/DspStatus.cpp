/**
 * DspStatus.cpp - the DSP status readouts' figures, choice and formats (see DspStatus.h).
 */

#include "uiclient/DspStatus.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace guitarfx::uiclient
{
namespace
{
const std::string kNone = "\xE2\x80\x94";

std::string Fixed(double value, int digits, const char* unit)
{
    char text[48];
    std::snprintf(text, sizeof(text), "%.*f%s", digits, value, unit);
    return text;
}

template <typename Map>
std::optional<double> Find(const Map& map, const std::string& key)
{
    const auto it = map.find(key);
    return it != map.end() && std::isfinite(it->second) ? std::optional<double>(it->second) : std::nullopt;
}
} // namespace

const std::vector<DspStatusItemInfo>& DspStatusItems()
{
    static const std::vector<DspStatusItemInfo> items = {
        {DspStatusItem::Peak, "peak", "Peak avg", "Peak level, averaged", "Levels"},
        {DspStatusItem::Rms, "rms", "RMS avg", "RMS level, averaged", "Levels"},
        {DspStatusItem::Headroom, "headroom", "Headroom avg", "Room left below 0 dBFS, averaged", "Levels"},
        {DspStatusItem::Momentary, "momentary", "Momentary", "Momentary loudness: the last 400 ms (BS.1770)", "Loudness"},
        {DspStatusItem::ShortTerm, "shortTerm", "Short-term", "Short-term loudness: the last 3 seconds (BS.1770)", "Loudness"},
        {DspStatusItem::Integrated, "integrated", "Integrated", "Integrated loudness since this readout opened", "Loudness"},
        {DspStatusItem::Processing, "processing", "Processing avg",
         "Processing time per block, averaged, and its share of all processing", "DSP"},
        {DspStatusItem::Latency, "latency", "Latency", "Delay this adds to the signal", "DSP"},
        {DspStatusItem::DspLoad, "dspLoad", "DSP load avg", "Processing time as a share of each block's real time, averaged", "DSP"},
        {DspStatusItem::DspPeak, "dspPeak", "Load peak", "The heaviest single block in the last 10 seconds, as a share of its real time",
         "DSP"},
    };
    return items;
}

const DspStatusItemInfo& DspStatusInfo(DspStatusItem item)
{
    const auto& items = DspStatusItems();
    return *std::find_if(items.begin(), items.end(), [item](const DspStatusItemInfo& info) { return info.id == item; });
}

const DspStatusReadout& EffectDspStatus()
{
    using I = DspStatusItem;
    static const DspStatusReadout readout{"ui.dspStatus.items",
                                          {I::Peak, I::Rms, I::Headroom, I::Momentary, I::ShortTerm, I::Integrated, I::Processing, I::Latency},
                                          {I::Peak, I::Rms, I::Processing, I::Latency},
                                          {}};
    return readout;
}

const DspStatusReadout& OutputDspStatus()
{
    using I = DspStatusItem;
    static const DspStatusReadout readout{
        "ui.outputStatus.items",
        {I::Peak, I::Rms, I::Headroom, I::Momentary, I::ShortTerm, I::Integrated, I::DspLoad, I::DspPeak, I::Processing, I::Latency},
        {I::DspLoad, I::DspPeak, I::Latency, I::Peak, I::Rms},
        {{I::Peak, "Output peak level after the master volume, averaged"},
         {I::Rms, "Output RMS level after the master volume, averaged"},
         {I::Headroom, "Output room left below 0 dBFS, averaged"},
         {I::Momentary, "Momentary loudness of the output: the last 400 ms (BS.1770)"},
         {I::ShortTerm, "Short-term loudness of the output: the last 3 seconds (BS.1770)"},
         {I::Integrated, "Integrated loudness of the output since this readout opened"},
         {I::Processing, "Processing time per block for the whole chain, averaged"},
         {I::Latency, "Delay the whole chain adds to the signal"}}};
    return readout;
}

std::vector<DspStatusItem> ShownDspStatusItems(const ClientState& state, const DspStatusReadout& readout)
{
    const auto stored = state.appSettings.find(readout.settingKey);
    if (stored == state.appSettings.end() || !stored->is_array())
        return readout.defaults;
    std::vector<DspStatusItem> shown;
    for (const auto& value : *stored)
    {
        if (!value.is_string())
            continue;
        for (const auto& info : DspStatusItems())
            if (value.get<std::string>() == info.key &&
                std::find(readout.available.begin(), readout.available.end(), info.id) != readout.available.end() &&
                std::find(shown.begin(), shown.end(), info.id) == shown.end())
                shown.push_back(info.id);
    }
    return shown;
}

nlohmann::json DspStatusSettingValue(const std::vector<DspStatusItem>& items)
{
    nlohmann::json value = nlohmann::json::array();
    for (const auto item : items)
        value.push_back(DspStatusInfo(item).key);
    return value;
}

bool ShowsLoudness(const std::vector<DspStatusItem>& items)
{
    return std::any_of(items.begin(), items.end(), [](DspStatusItem item) {
        return item == DspStatusItem::Momentary || item == DspStatusItem::ShortTerm || item == DspStatusItem::Integrated;
    });
}

std::string DspStatusTitle(const DspStatusReadout& readout, DspStatusItem item)
{
    const auto it = readout.titles.find(item);
    return it != readout.titles.end() ? it->second : DspStatusInfo(item).title;
}

std::string FormatDspStatusDb(std::optional<double> value)
{
    return value ? Fixed(*value, 1, " dBFS") : kNone;
}

std::string FormatDspStatusTime(std::optional<double> timeUs, std::optional<double> totalUs)
{
    if (!timeUs)
        return kNone;
    std::string text = Fixed(*timeUs, 1, " \xCE\xBCs");
    if (totalUs && *totalUs > 0.0)
        text += " (" + Fixed(*timeUs / *totalUs * 100.0, 1, "%") + ")";
    return text;
}

std::string FormatDspStatusLatency(std::optional<double> samples, double sampleRate, bool showZero)
{
    // A node that adds none reads as a dash; a whole chain's 0 is a figure worth seeing.
    if (!samples || *samples < 0.0 || (*samples == 0.0 && !showZero))
        return kNone;
    std::string text = std::to_string(std::lround(*samples)) + " smp";
    if (sampleRate > 0.0)
        text += " (" + Fixed(*samples / sampleRate * 1000.0, 2, " ms") + ")";
    return text;
}

std::optional<double> DspStatusMeter::Average(const char* name, std::optional<double> value)
{
    if (!value)
        return std::nullopt;
    const auto it = mAverages.find(name);
    const double average = it == mAverages.end() ? *value : it->second + kSmoothing * (*value - it->second);
    mAverages[name] = average;
    return average;
}

void DspStatusMeter::Update(const ClientState& state, double nowSeconds)
{
    using I = DspStatusItem;
    const auto& t = state.telemetry;
    const bool output = mNodeKey.empty();
    const LevelReading* level = output ? &t.output : nullptr;
    if (!output)
        if (const auto it = t.nodes.find(mNodeKey); it != t.nodes.end())
            level = &it->second;

    // Levels: averaged once per frame, so a frame applied twice does not count twice.
    if (t.frameCounter != mLastFrame)
    {
        mLastFrame = t.frameCounter;
        const auto peak = level != nullptr ? std::optional<double>(level->peakDb) : std::nullopt;
        const auto rms = level != nullptr ? std::optional<double>(level->rmsDb) : std::nullopt;
        mValues[I::Peak] = FormatDspStatusDb(Average("peak", peak));
        mValues[I::Rms] = FormatDspStatusDb(Average("rms", rms));
        const auto headroom = Average("headroom", peak ? std::optional<double>(std::max(0.0, -*peak)) : std::nullopt);
        mValues[I::Headroom] = headroom ? Fixed(*headroom, 1, " dB") : kNone;
    }

    const auto& loudness = output ? state.analysis.outputLoudness : [&]() -> const LoudnessReading& {
        static const LoudnessReading none;
        const auto it = state.analysis.loudness.find(mNodeKey);
        return it != state.analysis.loudness.end() ? it->second : none;
    }();
    const auto lufs = [](const std::optional<double>& value) { return value ? Fixed(*value, 1, " LUFS") : kNone; };
    mValues[I::Momentary] = lufs(loudness.momentaryLufs);
    mValues[I::ShortTerm] = lufs(loudness.shortTermLufs);
    mValues[I::Integrated] = lufs(loudness.integratedLufs);
    mIntegratedSeconds = loudness.integratedLufs ? std::optional<double>(loudness.integratedSeconds) : std::nullopt;

    const auto total = t.totalProcessingUs > 0.0 ? std::optional<double>(t.totalProcessingUs) : std::nullopt;
    if (output)
    {
        mValues[I::Processing] = FormatDspStatusTime(Average("processing", total), std::nullopt);
        mValues[I::Latency] = FormatDspStatusLatency(static_cast<double>(t.totalLatencySamples), t.sampleRate, true);
        const auto load = Average("dspLoad", t.dspLoadPercent);
        mValues[I::DspLoad] = load ? Fixed(*load, 1, "%") : kNone;
        if (t.peakDspLoadPercent)
            mPeaks.emplace_back(nowSeconds, *t.peakDspLoadPercent);
        while (!mPeaks.empty() && nowSeconds - mPeaks.front().first > kPeakWindowSeconds)
            mPeaks.pop_front();
        std::optional<double> peak;
        for (const auto& [at, percent] : mPeaks)
            peak = peak ? std::max(*peak, percent) : percent;
        mValues[I::DspPeak] = peak ? Fixed(*peak, 1, "%") : kNone;
    }
    else
    {
        mValues[I::Processing] = FormatDspStatusTime(Average("processing", Find(t.nodeProcessingUs, mNodeKey)), total);
        mValues[I::Latency] = FormatDspStatusLatency(Find(t.nodeLatencySamples, mNodeKey), t.sampleRate, false);
    }
}

bool DspStatusMeter::TakeRenderTurn(double nowSeconds)
{
    if (nowSeconds - mLastRender < kRenderIntervalSeconds)
        return false;
    mLastRender = nowSeconds;
    return true;
}

std::string DspStatusMeter::Value(DspStatusItem item) const
{
    const auto it = mValues.find(item);
    return it != mValues.end() ? it->second : kNone;
}

std::string DspStatusMeter::IntegratedTitle() const
{
    if (!mIntegratedSeconds)
        return "Integrated loudness since this readout opened";
    const long whole = std::max(0L, static_cast<long>(*mIntegratedSeconds));
    char duration[24];
    std::snprintf(duration, sizeof(duration), "%ld:%02ld", whole / 60, whole % 60);
    return std::string("Integrated loudness over the last ") + duration +
           ", since this readout opened. Close and reopen it to start again.";
}

void DspStatusMeter::Reset()
{
    mAverages.clear();
    mValues.clear();
    mPeaks.clear();
    mIntegratedSeconds.reset();
    mLastRender = -1.0e9;
}
} // namespace guitarfx::uiclient
