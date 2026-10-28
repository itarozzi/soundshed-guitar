// Controls: the global chain around every preset, as the web UI's compact Controls strip drops it
// under the preset bar (ui-components/control-bar.html): the input meter and level, with which
// inputs feed it; the noise gate; a global transpose; the four band EQ; the doubler; the output
// level and meter, and mute. It stays open while it is adjusted, bound in place. The output
// limiter is in Settings. The full layout's control bar (WideFrame.cpp) lays out the same cells.

#include "ui/Shell.h"

#include "dsp/EffectGuids.h"

#include "uiclient/ParamFormat.h"

#include <cmath>
#include <cstdio>
#include <functional>

namespace nanoq::ui
{
using guitarfx::uiclient::EffectParamInfo;
using guitarfx::uiclient::Topic;

namespace
{
EffectParamInfo Param(const char* key, const char* name, const char* unit, double lo, double hi, double def, double step = 0.0)
{
    EffectParamInfo info;
    info.key = key;
    info.name = name;
    info.unit = unit;
    info.minValue = lo;
    info.maxValue = hi;
    info.defaultValue = def;
    info.step = step;
    return info;
}

double NodeParam(const guitarfx::SignalGraph& graph, const char* id, const char* key, double fallback)
{
    if (const auto* node = graph.FindNode(id))
        if (const auto it = node->params.find(key); it != node->params.end())
            return it->second;
    return fallback;
}

bool NodeEnabled(const guitarfx::SignalGraph& graph, const char* id)
{
    const auto* node = graph.FindNode(id);
    return node != nullptr && node->enabled;
}

std::string Decibels(double v)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%+.1f dB", v);
    return text;
}
} // namespace

Shell::ControlCells Shell::BuildControlCells()
{
    auto& commands = mSession.Commands();
    const auto chain = [this]() -> const guitarfx::GlobalSignalChainConfig& { return mState.globalChain; };
    using Read = std::function<double()>;
    using Write = std::function<void(double)>;
    using Format = std::function<std::string(double)>;

    // A cell of the strip: its label (with a switch beside it for a stage that turns off) over a
    // knob and its value, and, for the input, its mode under that.
    auto knobCell = [&](const char* label, const EffectParamInfo& info, Read read, Write write, Format format,
                        std::function<bool()> readOn = {}, std::function<void(bool)> writeOn = {}, Ptr below = nullptr) -> Ptr {
        auto knob = std::make_shared<ParamKnob>(mTheme, "");
        knob->DefaultPosition(guitarfx::uiclient::ParamValueToPosition(info, info.defaultValue));
        knob->onChange = [info, write, format, k = knob.get()](double position) {
            const double v = guitarfx::uiclient::SnapParamValue(info, guitarfx::uiclient::ParamPositionToValue(info, position));
            k->SetValueText(format(v));
            write(v);
        };
        knob->onReset = [info, write] { write(info.defaultValue); };
        knob->onEdit = [this, label, info, read, write] {
            OpenPrompt(label, guitarfx::uiclient::EditableParamText(info, read()), "Set", [info, write](const std::string& entered) {
                if (const auto typed = guitarfx::uiclient::ParseTypedNumber(entered))
                    write(guitarfx::uiclient::SnapParamValue(info, std::clamp(*typed, info.minValue, info.maxValue)));
            });
        };
        // The input and output levels take a MIDI mapping, as the web UI's control-bar knobs do.
        if (std::string_view(label) == "IN" || std::string_view(label) == "OUT")
            knob->onMenu = [this, which = std::string(std::string_view(label) == "IN" ? "input" : "output")](el::point at) {
                OpenGlobalKnobMidiMenu(which, at);
            };
        mLive.Bind(knob, read, [info, format](ParamKnob& k, double v) { k.Update(guitarfx::uiclient::ParamValueToPosition(info, v), format(v)); });

        auto title = std::make_shared<Text>(mTheme, label, 9.0f, Weight::SemiBold);
        title->Colour(mTheme.Pick(el::rgba(201, 209, 225, 200), mTheme.TextSecondary())).Fixed(true);
        std::vector<Ptr> head{Stretch(), title};
        if (readOn)
        {
            auto toggle = std::make_shared<MiniSwitch>(mTheme, readOn());
            toggle->onClick = [readOn, writeOn] { writeOn(!readOn()); };
            mLive.Bind(toggle, readOn, &MiniSwitch::Set);
            head.push_back(HGap(5));
            head.push_back(toggle);
        }
        head.push_back(Stretch());

        // The knob draws no name of its own: the label row sits over the space it leaves.
        auto top = Over(Col({FixedHeight(12, Row(head)), std::make_shared<el::element>()}), Row({Stretch(), knob, Stretch()}));
        std::vector<Ptr> cell{FixedHeight(67, top)};
        if (below)
        {
            cell.push_back(VGap(1));
            cell.push_back(FixedHeight(16, Row({Stretch(), below, Stretch()})));
        }
        cell.push_back(std::make_shared<el::element>());
        return FixedWidth(84, Col(cell));
    };

    // ── IN: the level, and which inputs feed it ──
    auto inputModeLabel = [chain, this] {
        const auto& c = chain();
        if (c.dualMono)
            return std::string("Dual mono");
        if (!c.monoMode)
            return std::string(mState.environment.standalone ? "Stereo" : "Track");
        return std::string(c.inputChannel == 1 ? "Mono In 2" : c.inputChannel == 2 ? "Mono In 1+2" : "Mono In 1");
    };
    auto mode = std::make_shared<IconButton>(mTheme, mIcons, "");
    mode->Size(70, 16).Label(inputModeLabel(), 10.0f).Caret(true).Ink(mTheme.TextSecondary());
    mode->onClick = [this, chain, m = mode.get()] {
        auto set = [this](bool mono, int channel, bool dual) {
            return [this, mono, channel, dual] { mSession.Commands().SetInputMode(mono, channel, dual); };
        };
        const auto& c = chain();
        std::vector<MenuItem> items;
        // In the standalone app the player picks mono (input 1, input 2 or both summed), stereo or
        // dual mono; in a DAW the track decides mono or stereo, and dual mono is the one choice left.
        if (mState.environment.standalone)
        {
            items.push_back({"Mono In 1", c.monoMode && !c.dualMono && c.inputChannel == 0, false, set(true, 0, false)});
            items.push_back({"Mono In 2", c.monoMode && !c.dualMono && c.inputChannel == 1, false, set(true, 1, false)});
            items.push_back({"Mono In 1+2", c.monoMode && !c.dualMono && c.inputChannel == 2, false, set(true, 2, false)});
            items.push_back({"Stereo", !c.monoMode && !c.dualMono, false, set(false, c.inputChannel, false)});
        }
        else
        {
            items.push_back({"As the track", !c.dualMono, false, set(c.monoMode, c.inputChannel, false)});
        }
        // Dual mono needs both inputs kept apart, so it turns mono off.
        items.push_back({"Dual mono", c.dualMono, false, set(false, c.inputChannel, true)});
        OpenDropdown(m->Bounds(), std::move(items), 150.0f);
    };
    mLive.Bind(mode, inputModeLabel, [](IconButton& m, const std::string& label) { m.Label(label, 10.0f); });

    auto in = knobCell("IN", Param("inputGain", "In", "dB", -12.0, 12.0, 0.0), [chain] { return chain().inputGain; },
                       [&commands](double v) { commands.SetGlobalChainParam("input.gain", v); }, Decibels, {}, {}, mode);

    // ── GATE ──
    // ── GATE: Threshold and on/off here; everything else, and Learn, behind Settings ──
    auto gateSettings = std::make_shared<IconButton>(mTheme, mIcons, "");
    gateSettings->Size(70, 16).Label("Settings", 10.0f).Caret(true).Ink(mTheme.TextSecondary());
    gateSettings->onClick = [this, g = gateSettings.get()] { OpenGlobalGate(g->Bounds()); };
    auto gate = knobCell("GATE", Param("threshold", "Gate", "dB", -90.0, 0.0, -60.0, 1.0),
                         [chain] { return NodeParam(chain().preChainGraph, "global_gate", "threshold", -60.0); },
                         [&commands](double v) { commands.SetGlobalChainParam("gate.threshold", v); },
                         [](double v) { return std::to_string(static_cast<int>(std::lround(v))) + " dB"; },
                         [chain] { return NodeEnabled(chain().preChainGraph, "global_gate"); },
                         [&commands](bool on) { commands.SetGlobalChainToggle("gate.enabled", on); }, gateSettings);

    // ── TRANSPOSE: as the web UI's knob, the transpose runs only while it transposes ──
    auto transpose = knobCell(
        "TRANSPOSE", Param("semitones", "Transpose", " st", -12.0, 12.0, 0.0, 1.0),
        [chain] {
            return NodeEnabled(chain().preChainGraph, "global_transpose") ? NodeParam(chain().preChainGraph, "global_transpose", "semitones", 0.0)
                                                                          : 0.0;
        },
        [&commands](double v) {
            const auto semitones = std::round(v);
            commands.SetGlobalChainParam("transpose.semitones", semitones);
            commands.SetGlobalChainToggle("transpose.enabled", semitones != 0.0);
        },
        [](double v) {
            const int semitones = static_cast<int>(std::lround(v));
            return std::string(semitones > 0 ? "+" : "") + std::to_string(semitones) + " st";
        });

    // ── EQ: its switch, and a button that opens its four bands ──
    auto eqButton = std::make_shared<IconButton>(mTheme, mIcons, "sliders", IconButton::Look::Boxed);
    eqButton->Size(46, 46).IconSize(18).Ink(mTheme.TextSecondary());
    eqButton->onClick = [this, b = eqButton.get()] { OpenGlobalEq(b->Bounds()); };
    auto eqOn = std::make_shared<MiniSwitch>(mTheme, NodeEnabled(chain().postChainGraph, "global_eq"));
    eqOn->onClick = [this, chain] {
        mSession.Commands().SetGlobalChainToggle("eq.enabled", !NodeEnabled(chain().postChainGraph, "global_eq"));
    };
    mLive.Bind(eqOn, [chain] { return NodeEnabled(chain().postChainGraph, "global_eq"); }, &MiniSwitch::Set);
    auto eqTitle = std::make_shared<Text>(mTheme, "EQ", 9.0f, Weight::SemiBold);
    eqTitle->Colour(mTheme.Pick(el::rgba(201, 209, 225, 200), mTheme.TextSecondary())).Fixed(true);
    auto eq = FixedWidth(72, Col({FixedHeight(12, Row({Stretch(), eqTitle, HGap(5), eqOn, Stretch()})), VGap(8),
                                  FixedHeight(46, Row({Stretch(), eqButton, Stretch()})), std::make_shared<el::element>()}));

    // ── DOUBLER ──
    auto doubler = knobCell("DOUBLER", Param("time", "Doubler", "ms", 0.5, 50.0, 6.0),
                            [chain] { return NodeParam(chain().postChainGraph, "global_doubler", "time", 6.0); },
                            [&commands](double v) { commands.SetGlobalChainParam("doubler.delay", v); },
                            [](double v) {
                                char text[24];
                                std::snprintf(text, sizeof(text), "%.2f ms", v);
                                return std::string(text);
                            },
                            [chain] { return NodeEnabled(chain().postChainGraph, "global_doubler"); },
                            [&commands](bool on) { commands.SetGlobalChainToggle("doubler.enabled", on); });

    // ── OUT, with mute under its meter, and the output's status readout behind Status ──
    auto outStatus = std::make_shared<IconButton>(mTheme, mIcons, "");
    outStatus->Size(70, 16).Label("Status", 10.0f).Caret(true).Ink(mTheme.TextSecondary());
    outStatus->onClick = [this, o = outStatus.get()] { OpenOutputStatus(o->Bounds()); };
    auto out = knobCell("OUT", Param("outputGain", "Out", "dB", -12.0, 12.0, 0.0), [chain] { return chain().outputGain; },
                        [&commands](double v) { commands.SetGlobalChainParam("output.gain", v); }, Decibels, {}, {}, outStatus);

    // The meters, from the telemetry feed (20 Hz): only a bar that moved is redrawn.
    auto inMeter = std::make_shared<MeterBar>(mTheme);
    inMeter->Vertical(true);
    mLive.Feed(inMeter, [this](MeterBar& m) { return m.Set(mState.telemetry.input.peakDb, mState.telemetry.input.clipped); });
    auto outMeter = std::make_shared<MeterBar>(mTheme);
    outMeter->Vertical(true);
    mLive.Feed(outMeter, [this](MeterBar& m) { return m.Set(mState.telemetry.output.peakDb, mState.telemetry.output.clipped); });
    auto mute = std::make_shared<IconButton>(mTheme, mIcons, mState.outputMuted ? "ui-mute" : "ui-speaker");
    mute->Size(20, 18).IconSize(12);
    mute->onClick = [this] { mSession.Commands().SetOutputMuted(!mState.outputMuted); };
    mLive.Bind(mute, [this] { return mState.outputMuted; }, &IconButton::Lit);

    ControlCells cells;
    cells.in = in;
    cells.gate = gate;
    cells.transpose = transpose;
    cells.eq = eq;
    cells.doubler = doubler;
    cells.out = out;
    cells.inMeter = inMeter;
    cells.outMeter = outMeter;
    cells.mute = mute;
    cells.levelling = BuildInputLevellingButton();
    return cells;
}

void Shell::OpenControls()
{
    auto c = BuildControlCells();
    auto strip = Row({Stretch(), Col({FixedHeight(84, c.inMeter), VGap(2), FixedHeight(18, Row({Stretch(), c.levelling, Stretch()})), std::make_shared<el::element>()}), HGap(14), c.in, c.gate, c.transpose,
                      Stretch(), c.eq, c.doubler, c.out, HGap(14),
                      Col({FixedHeight(84, c.outMeter), VGap(2), FixedHeight(18, Row({Stretch(), c.mute, Stretch()})),
                           std::make_shared<el::element>()}),
                      Stretch()});

    const auto view = ViewSize();
    ShowPopover(Pad(8, 10, 8, 6, strip), el::rect{metrics::kRailWidth + 6.0f, 0.0f, view.x - 6.0f, metrics::kTopBarHeight - 6.0f},
                view.x - metrics::kRailWidth - 12.0f, 124.0f);
}

void Shell::OpenGlobalEq(el::rect anchor)
{
    // The web UI's Global EQ dialog (eqPanel.ts): the curve over the output's spectrum, with a
    // handle per band, then each band's Gain, Freq and Q, and Shelf or Bell for the outer two.
    // Whatever overlay is up goes first, so its close cannot take this dialog's plot with it.
    CloseOverlay();
    auto& commands = mSession.Commands();
    auto syncs = std::make_shared<std::vector<std::function<void()>>>();
    const auto eqParams = [this]() -> std::map<std::string, double> {
        if (const auto* node = mState.globalChain.postChainGraph.FindNode("global_eq"))
            return node->params;
        return {};
    };
    const auto* type = mState.FindEffectType(guitarfx::EffectGuids::kEqParametric);

    EqPlotSource source;
    source.effectType = guitarfx::EffectGuids::kEqParametric;
    source.address = {"post", "global_eq", {}};
    source.feedKeys = {"post::global_eq"};
    source.read = eqParams;
    // The engine does not echo a global chain edit, so the knobs are moved from the mirrored state.
    source.write = [&commands, syncs](const std::string& key, double value, bool) {
        commands.SetGlobalChainParam("eq." + key, value);
        for (auto& fn : *syncs)
            fn();
    };
    auto plot = MakeEqPlot(std::move(source), true);

    std::vector<Ptr> bandRow{Stretch()};
    const std::pair<const char*, const char*> bands[] = {{"low", "Low"}, {"lowMid", "Low Mid"}, {"highMid", "High Mid"}, {"high", "High"}};
    for (const auto& [prefix, label] : bands)
    {
        std::vector<Ptr> knobs;
        for (const char* suffix : {"Gain", "Freq", "Q"})
        {
            const std::string key = std::string(prefix) + suffix;
            const auto it = type != nullptr ? std::find_if(type->parameters.begin(), type->parameters.end(),
                                                           [&key](const guitarfx::uiclient::EffectParamInfo& p) { return p.key == key; })
                                            : decltype(type->parameters.begin()){};
            if (type == nullptr || it == type->parameters.end())
                continue;
            const auto info = *it;
            auto knob = std::make_shared<ParamKnob>(mTheme, suffix);
            knob->DefaultPosition(guitarfx::uiclient::ParamValueToPosition(info, info.defaultValue));
            knob->onChange = [this, info, key, &commands, k = knob.get()](double position) {
                const double v = guitarfx::uiclient::SnapParamValue(info, guitarfx::uiclient::ParamPositionToValue(info, position));
                k->SetValueText(guitarfx::uiclient::FormatParamValue(info, v));
                commands.SetGlobalChainParam("eq." + key, v);
                if (mOverlayPlot)
                    SyncEqPlot(*mOverlayPlot);
            };
            knob->onReset = [this, info, key, &commands] {
                commands.SetGlobalChainParam("eq." + key, info.defaultValue);
                if (mOverlayPlot)
                    SyncEqPlot(*mOverlayPlot);
            };
            syncs->push_back([this, knob, info, key, eqParams] {
                const auto params = eqParams();
                const auto found = params.find(key);
                const double v = found != params.end() ? found->second : info.defaultValue;
                knob->Update(guitarfx::uiclient::ParamValueToPosition(info, v), guitarfx::uiclient::FormatParamValue(info, v));
                mSession.Repaint(*knob);
            });
            if (!knobs.empty())
                knobs.push_back(HGap(4));
            knobs.push_back(knob);
        }

        // The outer bands are shelves unless set to bells (lowShape/highShape: 0 shelf, 1 bell).
        Ptr heading;
        auto name = std::make_shared<Text>(mTheme, Upper(label), 9.0f, Weight::SemiBold);
        name->Colour(mTheme.Pick(el::rgba(220, 227, 243, 173), mTheme.TextSecondary()));
        const std::string band = prefix;
        if (band == "low" || band == "high")
        {
            const std::string shapeKey = band + "Shape";
            auto shape = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Chip);
            shape->Size(52, 18).Caret(true);
            shape->onClick = [this, shapeKey, eqParams, &commands, s = shape.get()] {
                const auto params = eqParams();
                const auto found = params.find(shapeKey);
                const bool bell = found != params.end() && found->second >= 0.5;
                OpenDropdown(s->Bounds(),
                             {{"Shelf", !bell, false, [this, shapeKey, &commands] {
                                   commands.SetGlobalChainParam("eq." + shapeKey, 0.0);
                                   if (mOverlayPlot)
                                       SyncEqPlot(*mOverlayPlot);
                               }},
                              {"Bell", bell, false, [this, shapeKey, &commands] {
                                   commands.SetGlobalChainParam("eq." + shapeKey, 1.0);
                                   if (mOverlayPlot)
                                       SyncEqPlot(*mOverlayPlot);
                               }}},
                             120.0f);
            };
            syncs->push_back([this, shape, shapeKey, eqParams] {
                const auto params = eqParams();
                const auto found = params.find(shapeKey);
                shape->Label(found != params.end() && found->second >= 0.5 ? "Bell" : "Shelf", 10.0f);
                mSession.Repaint(*shape);
            });
            heading = Row({name, HGap(6), shape, Stretch()});
        }
        else
        {
            heading = Row({name, Stretch()});
        }
        const float width = 3.0f * 60.0f + 8.0f;
        bandRow.push_back(FixedWidth(width, Col({FixedHeight(18, heading), VGap(4), FixedHeight(63, Row(knobs))})));
        bandRow.push_back(HGap(16));
    }
    bandRow.back() = Stretch();

    auto title = std::make_shared<Text>(mTheme, "GLOBAL EQ", 9.0f, Weight::SemiBold);
    title->Colour(mTheme.Pick(el::rgba(220, 227, 243, 173), mTheme.TextSecondary()));
    const float width = std::min(4.0f * (3.0f * 60.0f + 8.0f) + 3.0f * 16.0f + 24.0f, ViewSize().x - 24.0f);
    ShowPopover(Pad(12, 10, 12, 10, Col({FixedHeight(14, title), VGap(6), FixedHeight(160, plot), VGap(10), FixedHeight(85, Row(bandRow))})),
                anchor, width, 14.0f + 6.0f + 160.0f + 10.0f + 85.0f + 20.0f);
    // The dialog's plot goes with it, and the spectrum watch back to the effect editor's.
    mOnOverlayClosed = [this] { mOverlayPlot.reset(); };

    auto sync = [this, syncs] {
        for (auto& fn : *syncs)
            fn();
        if (mOverlayPlot)
            SyncEqPlot(*mOverlayPlot);
    };
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::GlobalChain, sync));
    sync();
}
} // namespace nanoq::ui
