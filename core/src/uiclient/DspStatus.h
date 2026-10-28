#pragma once

/**
 * DspStatus - the tiles of a DSP status readout (the web UI's dspStatusReadout.ts, outputStatus.ts
 * and the effect panel's readout in signalPath/telemetry.ts), for the native UIs.
 *
 * Two readouts share it: an effect's and the app output's. Each shows the figures the user chose,
 * an app setting per readout (ui.dspStatus.items, ui.outputStatus.items), so the choice is the
 * same in every UI and instance. Levels are averaged (an exponential average fed at the telemetry
 * rate) and redrawn at most every half second so they can be read; loudness needs a measurement of
 * its own, which the owner starts while a loudness tile is shown (Analysis::WatchLoudness).
 */

#include "uiclient/ClientState.h"

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
enum class DspStatusItem
{
    Peak,
    Rms,
    Headroom,
    Momentary,
    ShortTerm,
    Integrated,
    Processing,
    Latency,
    DspLoad,
    DspPeak
};

struct DspStatusItemInfo
{
    DspStatusItem id;
    const char* key;   // as the setting stores it ("peak", "shortTerm", ...)
    const char* label; // "Peak avg"
    const char* title; // the tooltip
    const char* group; // "Levels", "Loudness" or "DSP"
};

/// Every figure, in the menu's order.
[[nodiscard]] const std::vector<DspStatusItemInfo>& DspStatusItems();
[[nodiscard]] const DspStatusItemInfo& DspStatusInfo(DspStatusItem item);

struct DspStatusReadout
{
    const char* settingKey;
    std::vector<DspStatusItem> available;        // what it can show, in menu order
    std::vector<DspStatusItem> defaults;         // what it shows until the user chooses
    std::map<DspStatusItem, std::string> titles; // tooltips that differ from the general ones
};

/// An effect's readout: its own levels, loudness, time and latency.
[[nodiscard]] const DspStatusReadout& EffectDspStatus();
/// The output meter's readout: the app's output, and the whole chain's load and latency.
[[nodiscard]] const DspStatusReadout& OutputDspStatus();

/// What a readout shows: the user's choice, kept to what it can show, else its defaults.
[[nodiscard]] std::vector<DspStatusItem> ShownDspStatusItems(const ClientState& state, const DspStatusReadout& readout);
/// The setting's value for a choice, to store with UiCommands::SetSetting(readout.settingKey, ...).
[[nodiscard]] nlohmann::json DspStatusSettingValue(const std::vector<DspStatusItem>& items);
/// Whether a loudness figure is among `items`, which is what the loudness measurement waits on.
[[nodiscard]] bool ShowsLoudness(const std::vector<DspStatusItem>& items);
[[nodiscard]] std::string DspStatusTitle(const DspStatusReadout& readout, DspStatusItem item);

/// The figures of one readout, as they stand: fed every telemetry frame, formatted for the tiles.
class DspStatusMeter
{
  public:
    /// The smoothing factor of the averaged figures; lower is slower.
    static constexpr double kSmoothing = 0.01;
    /// How often averaged figures are worth redrawing.
    static constexpr double kRenderIntervalSeconds = 0.5;
    /// How far back the output's load peak looks.
    static constexpr double kPeakWindowSeconds = 10.0;

    /// For an effect, `nodeKey` is its feed key ("<presetId>::<nodeId>"); empty for the output.
    explicit DspStatusMeter(std::string nodeKey = {}) : mNodeKey(std::move(nodeKey))
    {
    }

    /// Folds in the state's latest telemetry. Call once per telemetry frame.
    void Update(const ClientState& state, double nowSeconds);
    /// Whether the figures are due a redraw; marks them drawn when they are.
    bool TakeRenderTurn(double nowSeconds);
    /// A figure as its tile shows it ("-12.3 dBFS", "\xE2\x80\x94" for none).
    [[nodiscard]] std::string Value(DspStatusItem item) const;
    /// The integrated tile's tooltip, which says how long it has measured.
    [[nodiscard]] std::string IntegratedTitle() const;
    void Reset();

  private:
    std::optional<double> Average(const char* name, std::optional<double> value);

    std::string mNodeKey;
    std::map<std::string, double> mAverages;
    std::map<DspStatusItem, std::string> mValues;
    std::deque<std::pair<double, double>> mPeaks; // (time, percent)
    std::optional<double> mIntegratedSeconds;
    double mLastRender = -1.0e9;
    unsigned mLastFrame = 0;
};

[[nodiscard]] std::string FormatDspStatusDb(std::optional<double> value);
[[nodiscard]] std::string FormatDspStatusTime(std::optional<double> timeUs, std::optional<double> totalUs);
[[nodiscard]] std::string FormatDspStatusLatency(std::optional<double> samples, double sampleRate, bool showZero);
} // namespace guitarfx::uiclient
