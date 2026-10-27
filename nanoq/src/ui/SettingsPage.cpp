// The settings page: the standalone app's audio and MIDI devices (DevicePage.cpp), the theme,
// the output, the amp model (NAM) quality settings, levels, the interface, DSP performance,
// Tone3000, software updates and the feature switches (as the web UI's Settings), and what this
// build is. The controls patch themselves from the engine's settings in place (SyncSettings), so
// dragging a slider is not interrupted by the setting it just changed coming back.

#include "ui/Shell.h"

#include "Paths.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/ParamFormat.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
namespace
{
constexpr const char* kOutputLimiterSetting = "audio.dsp.outputLimiterEnabled";
constexpr const char* kPresetSwitchTailSetting = "audio.presetSwitch.tailBars";
constexpr int kPresetSwitchTailDefault = 2;
constexpr const char* kNamSlimmableSizeSetting = "audio.nam.slimmableSize";
constexpr const char* kNamOversamplingSetting = "audio.nam.oversampling";
constexpr const char* kNamAntiAliasPhaseSetting = "audio.nam.antiAliasPhase";
constexpr const char* kWebsite = "https://soundshed.com";
constexpr const char* kGuides = "https://guitar.soundshed.com";

double NumberSetting(const nlohmann::json& settings, const char* key, double fallback)
{
    const auto it = settings.find(key);
    return it != settings.end() && it->is_number() ? it->get<double>() : fallback;
}

bool BoolSetting(const nlohmann::json& settings, const char* key, bool fallback)
{
    const auto it = settings.find(key);
    return it != settings.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
} // namespace

Ptr Shell::BuildSettingsPage()
{
    auto& commands = mSession.Commands();
    mSettingsSyncs.clear();

    std::vector<Ptr> page;

    auto heading = [this, &page](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 11.0f, Weight::SemiBold);
        t->Colour(mTheme.TextMuted());
        page.push_back(FixedHeight(32, PadXY(0, 6, t)));
    };

    auto note = [this, &page](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, metrics::kSmallText);
        t->Colour(mTheme.TextMuted());
        page.push_back(FixedHeight(24, t));
    };

    auto label = [this](const char* text) {
        auto l = std::make_shared<Text>(mTheme, text, metrics::kTextSize);
        l->Colour(mTheme.TextSecondary());
        return FixedWidth(190, l);
    };

    // A row of options, one lit: `get` says which, `set` is told which was tapped.
    auto choice = [&](const char* title, const std::vector<std::string>& options, std::function<int()> get, std::function<void(int)> set) {
        auto buttons = std::make_shared<std::vector<std::shared_ptr<Button>>>();
        std::vector<Ptr> row{label(title)};
        for (std::size_t i = 0; i < options.size(); ++i)
        {
            auto b = std::make_shared<Button>(mTheme, options[i], [set, i] { set(static_cast<int>(i)); }, Button::Style::Plain);
            b->MinWidth(76).TextSize(metrics::kSmallText);
            buttons->push_back(b);
            row.push_back(b);
            row.push_back(HGap(6));
        }
        row.push_back(Stretch());
        page.push_back(FixedHeight(44, Row(row)));
        mSettingsSyncs.push_back([this, buttons, get] {
            const int selected = get();
            for (std::size_t i = 0; i < buttons->size(); ++i)
            {
                (*buttons)[i]->Selected(static_cast<int>(i) == selected);
                mSession.View().refresh(*(*buttons)[i]);
            }
        });
    };

    auto toggle = [&](const char* title, const char* text, std::function<bool()> get, std::function<void(bool)> set) {
        auto b = std::make_shared<Button>(mTheme, text, [get, set] { set(!get()); }, Button::Style::Plain);
        b->Dot(true).MinWidth(220);
        page.push_back(FixedHeight(44, Row({label(title), b, Stretch()})));
        mSettingsSyncs.push_back([this, b, get] {
            b->On(get());
            mSession.View().refresh(*b);
        });
    };

    // ── Audio and MIDI devices: the standalone app only (DevicePage.cpp) ──
    page.push_back(BuildDeviceSection());

    // ── Appearance ──
    heading("APPEARANCE");
    choice("Theme", {"Dark", "Light", "Classic"},
           [this] { return mState.theme == "light" ? 1 : (mState.theme == "classic" ? 2 : 0); },
           [&commands](int i) { commands.SetTheme(i == 1 ? "light" : (i == 2 ? "classic" : "dark")); });

    // ── Output ──
    heading("OUTPUT");
    toggle("Limiter", "Soft limit the output", [this] { return BoolSetting(mState.appSettings, kOutputLimiterSetting, false); },
           [&commands](bool on) { commands.SetSetting(kOutputLimiterSetting, on); });
    choice("Preset switch tails", {"Off", "1 bar", "2 bars", "3 bars", "4 bars"},
           [this] { return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kPresetSwitchTailSetting, kPresetSwitchTailDefault)), 0, 4); },
           [&commands](int i) { commands.SetSetting(kPresetSwitchTailSetting, i); });
    note("How many bars of delay and reverb tail carry over when you change preset.");

    // ── Amp models ──
    heading("AMP MODELS (NAM)");
    {
        auto quality = std::make_shared<ParamSlider>(mTheme, "Quality");
        quality->onChange = [&commands](double v) { commands.SetSetting(kNamSlimmableSizeSetting, std::round(v * 100.0) / 100.0); };
        quality->onReset = [&commands] { commands.SetSetting(kNamSlimmableSizeSetting, 1.0); };
        page.push_back(FixedHeight(60, Row({FixedWidth(420, quality), Stretch()})));
        mSettingsSyncs.push_back([this, quality] {
            const double v = std::clamp(NumberSetting(mState.appSettings, kNamSlimmableSizeSetting, 1.0), 0.0, 1.0);
            quality->Update(v, std::to_string(static_cast<int>(std::lround(v * 100.0))) + "%");
            mSession.View().refresh(*quality);
        });
    }
    note("Lower quality reduces CPU use on models that support it. Watch the DSP load while you adjust it.");
    choice("Oversampling", {"Off", "2x", "4x", "8x", "16x", "32x"},
           [this] { return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kNamOversamplingSetting, 0)), 0, 5); },
           [&commands](int i) { commands.SetSetting(kNamOversamplingSetting, i); });
    choice("Anti-alias filter", {"Minimum phase", "Linear short", "Linear long"},
           [this] { return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kNamAntiAliasPhaseSetting, 0)), 0, 2); },
           [&commands](int i) { commands.SetSetting(kNamAntiAliasPhaseSetting, i); });

    // A slider over [lo, hi] for a number setting, with its unit and decimals; a double click on the
    // slider puts `def` back.
    auto slider = [&](const char* title, const char* key, double lo, double hi, double def, const char* unit, int decimals) {
        auto sl = std::make_shared<ParamSlider>(mTheme, title);
        sl->onChange = [&commands, key, lo, hi, decimals](double v) {
            const double scale = std::pow(10.0, decimals);
            commands.SetSetting(key, std::round((lo + v * (hi - lo)) * scale) / scale);
        };
        sl->onReset = [&commands, key, def] { commands.SetSetting(key, def); };
        page.push_back(FixedHeight(60, Row({FixedWidth(420, sl), Stretch()})));
        mSettingsSyncs.push_back([this, sl, key, lo, hi, def, unit, decimals] {
            const double v = std::clamp(NumberSetting(mState.appSettings, key, def), lo, hi);
            char text[48];
            std::snprintf(text, sizeof(text), "%.*f %s", decimals, v, unit);
            sl->Update((v - lo) / (hi - lo), text);
            mSession.View().refresh(*sl);
        });
    };
    const auto boolOr = [this](const char* key, bool def) { return BoolSetting(mState.appSettings, key, def); };

    // ── Levels (the web UI's General) ──
    heading("LEVELS");
    slider("Output ceiling", "audio.dsp.outputProtectionCeilingDbfs", -6.0, 0.0, -1.0, "dBFS", 1);
    slider("Nominal level", "audio.dsp.nominalOperatingLevelDbfs", -30.0, -6.0, -18.0, "dBFS", 1);
    note("The level the effects are voiced for, and the ceiling the limiter holds the output under.");
    toggle("Mono output", "Make up the centre pan's 3 dB", [boolOr] { return boolOr("audio.dsp.monoOutputUnity", false); },
           [&commands](bool on) { commands.SetSetting("audio.dsp.monoOutputUnity", on); });
    toggle("NAM calibration", "Match models to the interface level", [boolOr] { return boolOr("audio.nam.autoInputCalibration", true); },
           [&commands](bool on) { commands.SetSetting("audio.nam.autoInputCalibration", on); });
    slider("Interface level", "audio.nam.interfaceCalibrationLevelDbu", 0.0, 24.0, 12.0, "dBu", 1);
    note("Your interface's input level at 0 dBFS, from its manual; models captured at a known level are matched to it.");

    // ── Interface (the web UI's UI section) ──
    heading("INTERFACE");
    toggle("Mouse wheel", "Turns knobs and sliders", [boolOr] { return boolOr("ui.mouseWheelAdjustsControls", true); },
           [&commands](bool on) { commands.SetSetting("ui.mouseWheelAdjustsControls", on); });
    toggle("Signal flow", "Animate the cables while playing", [boolOr] { return boolOr("ui.signalFlowAnimation", false); },
           [&commands](bool on) { commands.SetSetting("ui.signalFlowAnimation", on); });
    toggle("Channels", "Show the channel choice on effects", [boolOr] { return boolOr("audio.signalPath.channelModeControl", false); },
           [&commands](bool on) { commands.SetSetting("audio.signalPath.channelModeControl", on); });

    // ── DSP performance ──
    heading("DSP PERFORMANCE");
    toggle("Fast NAM engine", "Fused pass where a model allows", [boolOr] { return boolOr("audio.nam.fusedEngine", true); },
           [&commands](bool on) { commands.SetSetting("audio.nam.fusedEngine", on); });
    {
        // The standalone app and the plugins keep their own choice, as in the web UI.
        const char* key = mState.environment.standalone ? "audio.processing.multiCore.standalone" : "audio.processing.multiCore.plugin";
        toggle("Multi-core", "Spread the chain over more cores", [boolOr, key] { return boolOr(key, true); },
               [&commands, key](bool on) { commands.SetSetting(key, on); });
    }

    // ── Tone3000 ──
    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::Tone3000))
    {
        heading("TONE3000");
        toggle("Search", "Through Soundshed's relay", [boolOr] { return boolOr("tone3000.useSoundshedToneSearchApi", true); },
               [&commands](bool on) { commands.SetSetting("tone3000.useSoundshedToneSearchApi", on); });
        note("Off, searches use your own Tone3000 API key, set in Soundshed Guitar's Settings.");
    }

    // ── Library: the web UI's Export Presets & Blends ──
    heading("LIBRARY");
    choice("Export resources", {"Used by presets", "Whole library"}, [this] { return mLibraryExportAll ? 1 : 0; },
           [this](int i) {
               mLibraryExportAll = i == 1;
               SyncSettings();
           });
    {
        auto exportButton = std::make_shared<Button>(mTheme, "Export Library...",
                                                     [this, &commands] { commands.ExportLibraryArchive(mLibraryExportAll); });
        exportButton->MinWidth(180);
        page.push_back(FixedHeight(44, Row({label(""), exportButton, Stretch()})));
    }
    note("Your presets and every blend, with their models and IRs or the whole library, as a .soundshed-library.zip.");

    // ── Software updates ──
    heading("SOFTWARE UPDATES");
    toggle("Updates", "Check for a new version at startup", [boolOr] { return boolOr("app.updateCheckEnabled", true); },
           [&commands](bool on) { commands.SetSetting("app.updateCheckEnabled", on); });
    if (mUpdate)
    {
        auto get = std::make_shared<Button>(mTheme, "Version " + mUpdate->version + " is available...", [this] { OpenUpdateDialog(); },
                                            Button::Style::Primary);
        get->MinWidth(260);
        page.push_back(FixedHeight(44, Row({label(""), get, Stretch()})));
    }

    // ── Features: every UI hides what is switched off here ──
    heading("FEATURES");
    for (const auto feature : guitarfx::uiclient::AllFeatures())
    {
        const char* key = guitarfx::uiclient::FeatureSettingKey(feature);
        toggle(guitarfx::uiclient::FeatureLabel(feature), "Enabled", [this, feature] { return guitarfx::uiclient::IsFeatureEnabled(mState, feature); },
               [&commands, key](bool on) { commands.SetSetting(key, on); });
    }
    note("Shared with Soundshed Guitar. Some features (the AI Tone Builder, Jam, the editors) are there only.");

    // ── Help: the web UI's Help tab, told for this app ──
    heading("HELP");
    auto topic = [this, &page](const char* title, const char* text) {
        auto t = std::make_shared<Text>(mTheme, title, metrics::kTextSize, Weight::SemiBold);
        page.push_back(FixedHeight(26, PadXY(0, 6, t)));
        page.push_back(Row({el::share(el::hold_any(std::make_shared<Paragraph>(mTheme, text, 520.0f))), Stretch()}));
        page.push_back(VGap(10));
    };
    topic("Signal chain",
          "Click a card to edit its effect, and drag it to move it, onto a cable or into a branch. The power button "
          "bypasses an effect: it stays in the preset, faded, and is saved with it; Delete takes it out. In the full "
          "layout, drag the bar under the chain up for icon tiles and down again for cards.");
    topic("Presets and scenes",
          "Scenes are variations stored inside one preset, so switching between them does not reload it. Multi-Rig "
          "runs two or more presets at once, as tabs over the chain with a Mix tab; saving the mix stores it as a "
          "Multi-Rig preset.");
    topic("Keys",
          "Ctrl+Z and Ctrl+Y undo and redo chain edits. Space bypasses the effect clicked last, or taps the tempo; "
          "Delete removes it; F2 renames the scene; Esc closes a menu or dialog.");
    topic("Library and sharing",
          "Tone3000 results come through the Soundshed API by default; your own Tone3000 key is set in Soundshed "
          "Guitar's Settings. Tones installed from tone sharing bring the models they need with them.");
    topic("Performance",
          "DSP load measures the audio thread against its block deadline, not the whole CPU, so it can sit near 100% "
          "while the machine reports a low overall load. In an effect's DSP readouts a blank figure means it did not "
          "run in the last block (bypassed, or fed no signal) rather than a measured zero.");
    {
        auto guides = std::make_shared<Button>(mTheme, "guitar.soundshed.com", [&commands] { commands.OpenUrl(kGuides); },
                                               Button::Style::Flat);
        guides->MinWidth(190);
        page.push_back(FixedHeight(44, Row({label("Guides and release notes"), guides, Stretch()})));
    }

    // ── About ──
    heading("ABOUT");
    auto line = [this, &page](const std::string& labelText, const std::string& value) {
        auto l = std::make_shared<Text>(mTheme, labelText, metrics::kTextSize);
        l->Colour(mTheme.TextSecondary());
        auto v = std::make_shared<Text>(mTheme, value, metrics::kTextSize, Weight::Medium);
        page.push_back(FixedHeight(26, Row({FixedWidth(190, l), v})));
    };
    line("Product", "Soundshed Nano Q");
    line("Engine", mState.environment.version.empty() ? "Soundshed engine" : mState.environment.version);
    line("Interface", "QPlug and Elements (no JUCE)");
    // As UTF-8, which the text expects: path::string() converts through the ANSI code page on
    // Windows, where a user name that page cannot spell throws, and opening Settings took the
    // app down with it.
    line("Profile", guitarfx::util::PathToUtf8(ProfileFolder()));
    {
        auto site = std::make_shared<Button>(mTheme, "soundshed.com", [&commands] { commands.OpenUrl(kWebsite); }, Button::Style::Flat);
        site->MinWidth(150);
        page.push_back(FixedHeight(44, Row({FixedWidth(190, std::make_shared<el::element>()), site, Stretch()})));
    }
    note("Riff capture, the JAM tools, plugin hosting and preset blending are in Soundshed Guitar,");
    note("which shares this app's presets, library and settings.");
    if (!mState.environment.audioDeviceSettings)
        note("The audio device is set by the host, or in the window menu (Alt+Space) of the standalone app.");

    SyncSettings();
    return Fill(mTheme.Background(), Pad(24, 8, 24, 12, VScroll(Col(page))));
}

void Shell::SyncSettings()
{
    for (auto& sync : mSettingsSyncs)
        sync();
}
} // namespace nanoq::ui
