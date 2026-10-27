// GateLearn.cpp - the noise gate's Learn (the web UI's gateLearn.ts, gateLearnPanel.ts and
// gateSettings.ts): a meter of the noise it heard against Threshold and its hysteresis, the
// button that listens for two seconds with the strings muted, and what it found. A finished
// listen sets Threshold the way the knob would; the engine only measures and suggests.
//
// Shown in a Noise Gate node's editor and in the global gate's settings, which also carry the
// gate's other parameters.

#include "ui/Drawn.h"
#include "ui/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/Analysis.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>
#include <cmath>

namespace nanoq::ui
{
namespace guids = guitarfx::EffectGuids;
using guitarfx::uiclient::GateLearnStatus;
using guitarfx::uiclient::NodeAddress;
using guitarfx::uiclient::Topic;

namespace
{
using cycfi::artist::canvas;

/// The meter: the threshold's range across, the noise floor heard filled from the left, the
/// band between Threshold and where the gate closes again, Threshold as a line, and while
/// listening the live level and the listen's progress.
class GateLearnMeter final : public el::element, public Drawn
{
public:
    explicit GateLearnMeter(const Theme& theme) : mTheme(theme) {}

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, 14.0f}, {el::full_extent, 14.0f}};
    }

    void Set(double minDb, double maxDb, double threshold, double hysteresis, const GateLearnStatus* status)
    {
        mMin = minDb;
        mMax = std::max(minDb + 1.0, maxDb);
        mThreshold = threshold;
        mClose = threshold - std::max(0.0, hysteresis);
        mNoise = status != nullptr ? status->noiseFloorDb : std::nullopt;
        const bool listening = status != nullptr && status->state == "listening";
        mLevel = listening ? status->levelDb : std::nullopt;
        mProgress = listening ? status->progress : 0.0;
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto at = [&](double db) { return b.left + static_cast<float>(std::clamp((db - mMin) / (mMax - mMin), 0.0, 1.0)) * b.width(); };

        cnv.fill_style(mTheme.Pick(el::rgba(0, 0, 0, 90), el::rgba(0, 0, 0, 28)));
        cnv.add_round_rect(b, 3.0f);
        cnv.fill();

        auto state = cnv.new_state();
        cnv.add_round_rect(b, 3.0f);
        cnv.clip();
        if (mNoise)
        {
            cnv.fill_style(mTheme.TextMuted().opacity(0.35f));
            cnv.fill_rect(el::rect{b.left, b.top, at(*mNoise), b.bottom});
        }
        cnv.fill_style(mTheme.Accent().opacity(0.22f));
        cnv.fill_rect(el::rect{at(mClose), b.top, at(mThreshold), b.bottom});
        if (mLevel)
        {
            cnv.fill_style(mTheme.Success().opacity(0.7f));
            cnv.fill_rect(el::rect{b.left, b.top + 4.0f, at(*mLevel), b.bottom - 4.0f});
        }
        if (mProgress > 0.0)
        {
            cnv.fill_style(mTheme.Accent());
            cnv.fill_rect(el::rect{b.left, b.bottom - 2.0f, b.left + static_cast<float>(std::clamp(mProgress, 0.0, 1.0)) * b.width(), b.bottom});
        }
        cnv.fill_style(mTheme.Accent());
        const float x = at(mThreshold);
        cnv.fill_rect(el::rect{x - 1.0f, b.top, x + 1.0f, b.bottom});
    }

private:
    const Theme& mTheme;
    double mMin = -80.0, mMax = 0.0, mThreshold = -60.0, mClose = -64.0;
    std::optional<double> mNoise, mLevel;
    double mProgress = 0.0;
};

std::string WholeDb(double value)
{
    const long rounded = std::lround(value);
    return (rounded < 0 ? std::string("\xE2\x88\x92") + std::to_string(-rounded) : std::to_string(rounded)) + " dB";
}
} // namespace

Ptr Shell::BuildGateLearn(GateLearnTarget target)
{
    auto meter = std::make_shared<GateLearnMeter>(mTheme);
    auto low = std::make_shared<Text>(mTheme, "", 9.0f);
    auto high = std::make_shared<Text>(mTheme, "", 9.0f);
    low->Colour(mTheme.TextMuted());
    high->Colour(mTheme.TextMuted()).Alignment(Text::Align::Right);
    auto status = std::make_shared<Text>(mTheme, "", 11.0f);
    status->Colour(mTheme.TextSecondary());
    auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    button->Size(72, 26).Label("Learn", 11.0f);

    const auto key = guitarfx::uiclient::NodeFeedKey(target.address.scope, target.address.presetId, target.address.nodeId);
    button->onClick = [this, key, address = target.address, ensureEnabled = target.ensureEnabled] {
        auto& analysis = mSession.Client().GetAnalysis();
        auto& learn = mSession.Client().MutableState().analysis.gateLearn;
        if (const auto it = learn.find(key); it != learn.end() && it->second.state == "listening")
        {
            analysis.CancelGateLearn(address);
            return;
        }
        if (ensureEnabled)
            ensureEnabled();
        // Listening at once, as the web UI shows it, until the engine's first status.
        learn[key] = GateLearnStatus{"listening"};
        mGateLearnSeen[key] = "listening";
        analysis.StartGateLearn(address);
        SyncGateLearn();
    };

    mGateLearnViews.push_back({std::move(target), meter, button, status, low, high});
    SyncGateLearn();
    return Col({FixedHeight(14, meter), VGap(2), FixedHeight(12, Row({low, Stretch(), high})), VGap(6),
                FixedHeight(26, Row({button, HGap(10), status, Stretch()}))});
}

void Shell::SyncGateLearn()
{
    // Views of a page or dialog that has gone are dropped.
    mGateLearnViews.erase(std::remove_if(mGateLearnViews.begin(), mGateLearnViews.end(),
                                         [](const GateLearnView& v) { return v.meter.expired(); }),
                          mGateLearnViews.end());

    for (auto& view : mGateLearnViews)
    {
        const auto& t = view.target;
        const auto key = guitarfx::uiclient::NodeFeedKey(t.address.scope, t.address.presetId, t.address.nodeId);
        const auto it = mState.analysis.gateLearn.find(key);
        const GateLearnStatus* status = it != mState.analysis.gateLearn.end() ? &it->second : nullptr;

        // A listen that just finished sets Threshold, once, where it measured.
        if (status != nullptr)
        {
            auto& seen = mGateLearnSeen[key];
            if (status->state == "done" && seen == "listening" && status->thresholdDb && t.apply)
                t.apply(*status->thresholdDb);
            seen = status->state;
        }

        const auto meter = std::static_pointer_cast<GateLearnMeter>(view.meter.lock());
        if (!meter)
            continue;
        meter->Set(t.minDb, t.maxDb, t.threshold ? t.threshold() : -60.0, t.hysteresis ? t.hysteresis() : 0.0, status);
        mSession.Repaint(*meter);
        if (const auto text = view.status.lock())
        {
            const auto line = guitarfx::uiclient::GateLearnStatusText(status, t.minDb);
            if (text->Value() != line)
            {
                text->Set(line);
                mSession.Repaint(*text);
            }
        }
        if (const auto button = view.button.lock())
        {
            button->Label(status != nullptr && status->state == "listening" ? "Cancel" : "Learn", 11.0f);
            mSession.Repaint(*button);
        }
        if (const auto low = view.low.lock())
            low->Set(WholeDb(t.minDb));
        if (const auto high = view.high.lock())
            high->Set(WholeDb(t.maxDb));
    }
}

Shell::GateLearnTarget Shell::NodeGateLearnTarget(const guitarfx::GraphNode& node)
{
    const std::string nodeId = node.id;
    GateLearnTarget target;
    target.address = NodeAddress{"preset", nodeId, mState.activePresetId};
    const auto* type = mState.FindEffectType(node.type);
    // The effect type is looked up on each call, not kept: the catalog is replaced whenever the
    // engine sends it again, and a pointer into the old one read freed memory on the next sync
    // (seen when a Bars loop started recording).
    const auto param = [this, nodeId, typeId = node.type](const char* key, double fallback) {
        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
        if (n != nullptr)
            if (const auto it = n->params.find(key); it != n->params.end())
                return it->second;
        if (const auto* type = mState.FindEffectType(typeId))
            for (const auto& p : type->parameters)
                if (p.key == key)
                    return p.defaultValue;
        return fallback;
    };
    target.threshold = [param] { return param("threshold", -60.0); };
    target.hysteresis = [param] { return param("hysteresis", 4.0); };
    if (type != nullptr)
        for (const auto& p : type->parameters)
            if (p.key == "threshold")
                target.minDb = p.minValue, target.maxDb = p.maxValue;
    const std::string presetId = mState.activePresetId;
    target.apply = [this, nodeId, presetId](double thresholdDb) {
        // Only into the preset it measured: one switched away from while listening is left alone.
        if (mState.activePresetId != presetId)
            return;
        mSession.Commands().SetNodeParam(nodeId, "threshold", thresholdDb);
        NoteChainEdit();
        Mark(kValues);
    };
    target.ensureEnabled = [this, nodeId] {
        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
        if (n != nullptr && !n->enabled)
            mSession.Commands().SetNodeBypassed(nodeId, false);
    };
    return target;
}

void Shell::OpenGlobalGate(el::rect anchor)
{
    // The web UI's gate settings flyout: Learn, then every parameter the gate has, from the
    // engine's own definitions, sent as gate.<key>.
    CloseOverlay();
    auto& commands = mSession.Commands();
    const auto* type = mState.FindEffectType(guids::kDynamicsGate);
    const auto gateParam = [this](const std::string& key, double fallback) {
        if (const auto* node = mState.globalChain.preChainGraph.FindNode("global_gate"))
            if (const auto it = node->params.find(key); it != node->params.end())
                return it->second;
        return fallback;
    };
    auto syncs = std::make_shared<std::vector<std::function<void()>>>();

    GateLearnTarget target;
    target.address = NodeAddress{"global", "global_gate", {}};
    target.threshold = [gateParam] { return gateParam("threshold", -60.0); };
    target.hysteresis = [gateParam] { return gateParam("hysteresis", 4.0); };
    target.apply = [this, &commands, syncs](double thresholdDb) {
        commands.SetGlobalChainParam("gate.threshold", thresholdDb);
        for (auto& fn : *syncs)
            fn();
    };
    target.ensureEnabled = [this, &commands] {
        if (const auto* node = mState.globalChain.preChainGraph.FindNode("global_gate"); node != nullptr && !node->enabled)
            commands.SetGlobalChainToggle("gate.enabled", true);
    };

    std::vector<Ptr> knobs{Stretch()};
    int count = 0;
    if (type != nullptr)
        for (const auto& info : type->parameters)
        {
            if (info.key == "threshold")
                target.minDb = info.minValue, target.maxDb = info.maxValue;
            const std::string key = info.key;
            Ptr cell;
            if (guitarfx::uiclient::IsToggleParam(info))
            {
                auto toggle = std::make_shared<ParamToggle>(mTheme, info.name);
                toggle->onClick = [this, key, info, gateParam, &commands, syncs] {
                    const bool on = gateParam(key, info.defaultValue) > 0.5 * (info.minValue + info.maxValue);
                    commands.SetGlobalChainParam("gate." + key, on ? info.minValue : info.maxValue);
                    for (auto& fn : *syncs)
                        fn();
                };
                syncs->push_back([this, toggle, key, info, gateParam] {
                    toggle->Set(gateParam(key, info.defaultValue) > 0.5 * (info.minValue + info.maxValue));
                    mSession.Repaint(*toggle);
                });
                cell = toggle;
            }
            else
            {
                auto knob = std::make_shared<ParamKnob>(mTheme, info.name);
                knob->DefaultPosition(guitarfx::uiclient::ParamValueToPosition(info, info.defaultValue));
                knob->onChange = [this, info, key, &commands, k = knob.get()](double position) {
                    const double v = guitarfx::uiclient::SnapParamValue(info, guitarfx::uiclient::ParamPositionToValue(info, position));
                    k->SetValueText(guitarfx::uiclient::FormatParamValue(info, v));
                    commands.SetGlobalChainParam("gate." + key, v);
                    SyncGateLearn();
                };
                knob->onReset = [this, info, key, &commands, syncs] {
                    commands.SetGlobalChainParam("gate." + key, info.defaultValue);
                    for (auto& fn : *syncs)
                        fn();
                };
                syncs->push_back([this, knob, key, info, gateParam] {
                    const double v = gateParam(key, info.defaultValue);
                    knob->Update(guitarfx::uiclient::ParamValueToPosition(info, v), guitarfx::uiclient::FormatParamValue(info, v));
                    mSession.Repaint(*knob);
                });
                cell = knob;
            }
            knobs.push_back(cell);
            knobs.push_back(HGap(6));
            ++count;
        }
    knobs.back() = Stretch();
    syncs->push_back([this] { SyncGateLearn(); });

    auto title = std::make_shared<Text>(mTheme, "NOISE GATE", 9.0f, Weight::SemiBold);
    title->Colour(mTheme.Pick(el::rgba(220, 227, 243, 173), mTheme.TextSecondary()));
    auto learn = BuildGateLearn(std::move(target));
    const float width = std::min(std::max(360.0f, count * 66.0f + 24.0f), ViewSize().x - 24.0f);
    ShowPopover(Pad(12, 10, 12, 10, Col({FixedHeight(14, title), VGap(8), FixedHeight(60, learn), VGap(10), FixedHeight(63, Row(knobs))})),
                anchor, width, 14.0f + 8.0f + 60.0f + 10.0f + 63.0f + 20.0f);

    auto sync = [syncs] {
        for (auto& fn : *syncs)
            fn();
    };
    mInstrumentSubs.push_back(mSession.Client().Subscribe(Topic::GlobalChain, sync));
    sync();
}
} // namespace nanoq::ui
