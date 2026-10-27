// MultiRig.cpp - Multi-Rig (the web UI's signalPath/mixer.ts and multiPresetMixer.ts): with two or
// more presets in the mix, a tab per preset over the chain (the editor shows the one in focus;
// M and S mark a muted or soloed one; x takes it out of the mix) and a Mix tab whose page is
// the inline mixer: a strip per preset (Mix, Pan, Mute, Solo) and the master (the Multi-Rig's
// own Out level, Save and Delete). Saved Multi-Rigs are listed on the Presets page.

#include "ui/Shell.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/ParamFormat.h"

#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
namespace
{
constexpr double kMixGainMinDb = -24.0; // multiPresetMixerSupport.ts
constexpr double kMixGainMaxDb = 12.0;

std::string Percent(double value)
{
    return std::to_string(static_cast<int>(std::lround(value * 100.0))) + "%";
}

std::string Pan(double value)
{
    if (std::abs(value) < 0.01)
        return "C";
    return (value < 0.0 ? "L" : "R") + std::to_string(static_cast<int>(std::lround(std::abs(value) * 100.0)));
}

std::string GainDb(double db)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%+.1f dB", db);
    return text;
}
} // namespace

bool Shell::MultiRigShown() const
{
    return mState.mixer.activePresetIds.size() > 1;
}

std::string Shell::MixerSignature() const
{
    if (!MultiRigShown())
        return {};
    std::string s = std::string(mMixTab ? "mix|" : "rig|") + mState.activePresetId;
    for (const auto& id : mState.mixer.activePresetIds)
    {
        const auto it = mState.mixer.slots.find(id);
        s += "|" + id + ":" + (it != mState.mixer.slots.end() ? it->second.name + (it->second.mute ? "M" : "") + (it->second.solo ? "S" : "") : "");
    }
    if (const auto* loaded = guitarfx::uiclient::LoadedComposite(mState.mixer))
        s += "|=" + loaded->id + loaded->name;
    return s;
}

Ptr Shell::BuildMixerTabs()
{
    if (!MultiRigShown())
    {
        mMixTab = false;
        return nullptr;
    }
    auto tray = std::make_shared<el::htile_composite>();
    tray->push_back(HGap(2));
    for (const auto& id : mState.mixer.activePresetIds)
    {
        const auto it = mState.mixer.slots.find(id);
        std::string label = it != mState.mixer.slots.end() && !it->second.name.empty() ? it->second.name : id;
        if (it != mState.mixer.slots.end())
        {
            if (it->second.mute)
                label += "  M";
            if (it->second.solo)
                label += "  S";
        }
        auto tab = std::make_shared<SceneChip>(mTheme, label, !mMixTab && id == mState.activePresetId);
        tab->onClick = [this, id] {
            mMixTab = false;
            if (id != mState.activePresetId)
                mSession.Commands().FocusMixerPreset(id);
            mChainSignature.clear();
            mSceneSignature.clear();
            Mark(kScenes | kChain | kPage);
        };
        tab->onRemove = [this, id] { mSession.Commands().RemoveActivePreset(id); };
        tray->push_back(tab);
        tray->push_back(HGap(4));
    }
    auto mix = std::make_shared<SceneChip>(mTheme, "Mix", mMixTab);
    mix->onClick = [this] {
        mMixTab = true;
        mChainSignature.clear();
        mSceneSignature.clear();
        Mark(kScenes | kChain);
    };
    tray->push_back(mix);
    tray->push_back(HGap(2));
    return Row({HGap(26), Rounded(mTheme.Pick(el::rgba(24, 24, 30, 184), mTheme.Text().opacity(0.06f)), 11.0f, PadXY(0, 2, tray)), Stretch()});
}

Ptr Shell::BuildInlineMixer()
{
    mMixerKnobs.clear();
    auto& commands = mSession.Commands();

    const auto knob = [this](const char* name, double position, std::string text, std::function<void(double)> onChange,
                             std::function<std::string(double)> format, std::string id, char kind) {
        auto k = std::make_shared<ParamKnob>(mTheme, name);
        k->Update(position, std::move(text));
        k->DefaultPosition(kind == 'p' ? 0.5 : kind == 'g' ? (0.0 - kMixGainMinDb) / (kMixGainMaxDb - kMixGainMinDb) : 1.0);
        k->onChange = [onChange, format, raw = k.get()](double p) {
            raw->SetValueText(format(p));
            onChange(p);
        };
        k->onReset = [onChange, kind, raw = k.get(), format] {
            const double p = kind == 'p' ? 0.5 : kind == 'g' ? (0.0 - kMixGainMinDb) / (kMixGainMaxDb - kMixGainMinDb) : 1.0;
            raw->Update(p, format(p));
            onChange(p);
        };
        mMixerKnobs.push_back({std::move(id), kind, k, {}});
        return k;
    };

    std::vector<Ptr> strips;
    for (const auto& id : mState.mixer.activePresetIds)
    {
        const auto it = mState.mixer.slots.find(id);
        const auto slot = it != mState.mixer.slots.end() ? it->second : guitarfx::uiclient::MixerSlot{};
        auto name = std::make_shared<Text>(mTheme, slot.name.empty() ? id : slot.name, 11.5f, Weight::SemiBold);
        name->Colour(mTheme.Ink()).Alignment(Text::Align::Centre);
        auto mixKnob = knob("Mix", slot.mix, Percent(slot.mix), [&commands, id](double p) { commands.SetPresetMix(id, p); }, Percent, id, 'm');
        auto panKnob = knob("Pan", (slot.pan + 1.0) / 2.0, Pan(slot.pan), [&commands, id](double p) { commands.SetPresetPan(id, p * 2.0 - 1.0); },
                            [](double p) { return Pan(p * 2.0 - 1.0); }, id, 'p');
        auto mute = std::make_shared<ParamToggle>(mTheme, "Mute");
        mute->Set(slot.mute);
        mute->onClick = [this, id] {
            const auto s = mState.mixer.slots.find(id);
            mSession.Commands().SetPresetMute(id, s == mState.mixer.slots.end() || !s->second.mute);
        };
        auto solo = std::make_shared<ParamToggle>(mTheme, "Solo");
        solo->Set(slot.solo);
        solo->onClick = [this, id] {
            const auto s = mState.mixer.slots.find(id);
            mSession.Commands().SetPresetSolo(id, s == mState.mixer.slots.end() || !s->second.solo);
        };
        mMixerKnobs.push_back({id, 'u', {}, mute});
        mMixerKnobs.push_back({id, 's', {}, solo});
        auto body = Col({FixedHeight(20, name), VGap(4), FixedHeight(92, Row({Stretch(), mixKnob, panKnob, Stretch()})), VGap(6),
                         FixedHeight(63, Row({Stretch(), mute, solo, Stretch()}))});
        strips.push_back(FixedWidth(140, Over(PadXY(4, 8, body), std::make_shared<InsetBox>(mTheme))));
        strips.push_back(HGap(10));
    }

    // The master: the Multi-Rig's own level, and saving it as a Multi-Rig preset (or updating
    // the one it is, while its presets are the same).
    const double gain = mState.mixer.mixGainDb;
    const auto toGain = [](double p) { return kMixGainMinDb + p * (kMixGainMaxDb - kMixGainMinDb); };
    auto out = knob("Out", (gain - kMixGainMinDb) / (kMixGainMaxDb - kMixGainMinDb), GainDb(gain),
                    [&commands, toGain](double p) { commands.SetMixGain(std::round(toGain(p) * 10.0) / 10.0); },
                    [toGain](double p) { return GainDb(std::round(toGain(p) * 10.0) / 10.0); }, {}, 'g');
    const auto* loaded = guitarfx::uiclient::LoadedComposite(mState.mixer);
    auto save = std::make_shared<IconButton>(mTheme, mIcons, "ui-save", IconButton::Look::Boxed);
    save->Size(30, 30).IconSize(14).Ink(loaded != nullptr ? mTheme.Accent() : mTheme.TextSecondary());
    save->onClick = [this] { SaveMultiRig(); };
    auto remove = std::make_shared<IconButton>(mTheme, mIcons, "ui-trash", IconButton::Look::Boxed);
    remove->Size(30, 30).IconSize(14).Disabled(loaded == nullptr);
    if (loaded != nullptr)
        remove->onClick = [this, id = loaded->id, name = loaded->name] {
            OpenConfirm("Delete Multi-Rig", "Delete the Multi-Rig " + name + "?", "Delete",
                        [this, id] { mSession.Commands().RemoveCompositePreset(id); });
        };
    auto masterName = std::make_shared<Text>(mTheme, loaded != nullptr ? loaded->name : std::string("Master"), 11.5f, Weight::SemiBold);
    masterName->Colour(mTheme.Ink()).Alignment(Text::Align::Centre);
    auto master = Col({FixedHeight(20, masterName), VGap(4), FixedHeight(92, Row({Stretch(), out, Stretch()})), VGap(6),
                       FixedHeight(36, Row({Stretch(), save, HGap(8), remove, Stretch()})), std::make_shared<el::element>()});
    strips.push_back(FixedWidth(120, Over(PadXY(4, 8, master), std::make_shared<InsetBox>(mTheme))));

    auto row = std::make_shared<el::htile_composite>();
    for (auto& s : strips)
        row->push_back(s);
    return Col({VGap(16), FixedHeight(210, Row({HGap(24), HScroll(row), Stretch()})), std::make_shared<el::element>()});
}

void Shell::SyncMixerKnobs()
{
    for (const auto& entry : mMixerKnobs)
    {
        // Mute and Solo are patched in place: the chain region does not rebuild under the pointer.
        if (const auto t = entry.toggle.lock())
        {
            if (const auto it = mState.mixer.slots.find(entry.presetId); it != mState.mixer.slots.end())
                t->Set(entry.kind == 'u' ? it->second.mute : it->second.solo);
            mSession.Repaint(*t);
            continue;
        }
        const auto k = entry.knob.lock();
        if (!k || k->Dragging())
            continue;
        if (entry.kind == 'g')
        {
            const double db = mState.mixer.mixGainDb;
            k->Update((db - kMixGainMinDb) / (kMixGainMaxDb - kMixGainMinDb), GainDb(db));
        }
        else if (const auto it = mState.mixer.slots.find(entry.presetId); it != mState.mixer.slots.end())
        {
            if (entry.kind == 'm')
                k->Update(it->second.mix, Percent(it->second.mix));
            else
                k->Update((it->second.pan + 1.0) / 2.0, Pan(it->second.pan));
        }
        mSession.Repaint(*k);
    }
}

void Shell::SaveMultiRig()
{
    // The web UI asks for a name (its form also takes a description and tags); one already loaded
    // is updated in place under the name given.
    const auto* loaded = guitarfx::uiclient::LoadedComposite(mState.mixer);
    const std::string id = loaded != nullptr ? loaded->id : std::string();
    const std::string description = loaded != nullptr ? loaded->description : std::string();
    const auto tags = loaded != nullptr ? loaded->tags : std::vector<std::string>{};
    OpenPrompt(loaded != nullptr ? "Update Multi-Rig" : "Save Multi-Rig", loaded != nullptr ? loaded->name : std::string(), "Save",
               [this, id, description, tags](const std::string& entered) {
                   if (!entered.empty())
                       mSession.Commands().SaveCompositePreset(entered, description, tags, id);
               });
}

Ptr Shell::BuildMultiRigList()
{
    std::vector<Ptr> rows;
    if (!mState.mixer.compositePresetsKnown)
        mSession.Commands().GetCompositePresetList();
    const auto* loaded = guitarfx::uiclient::LoadedComposite(mState.mixer);
    for (const auto& rig : mState.mixer.compositePresets)
    {
        std::string detail = std::to_string(rig.presetIds.size()) + " presets";
        for (const auto& tag : rig.tags)
            detail += "  #" + tag;
        const std::string id = rig.id, name = rig.name;
        auto row = std::make_shared<ListRow>(mTheme, rig.name, detail, loaded != nullptr && loaded->id == id, false, [this, id] {
            // Loading replaces every preset in the mix, so unsaved edits to the one shown would go.
            if (mState.activePresetDirty)
                OpenConfirm("Load Multi-Rig", "Discard the unsaved changes to the preset shown?", "Load",
                            [this, id] { mSession.Commands().LoadCompositePreset(id); });
            else
                mSession.Commands().LoadCompositePreset(id);
        });
        row->onMenu = [this, id, name] {
            PostMenu(name, {{"Load", false, false, [this, id] { mSession.Commands().LoadCompositePreset(id); }},
                            {"Delete...", false, false, [this, id, name] {
                                 mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), id, name] {
                                     if (alive.lock())
                                         OpenConfirm("Delete Multi-Rig", "Delete the Multi-Rig " + name + "?", "Delete",
                                                     [this, id] { mSession.Commands().RemoveCompositePreset(id); });
                                 });
                             }}});
        };
        rows.push_back(FixedHeight(40, row));
    }
    if (rows.empty())
    {
        auto none = std::make_shared<Text>(mTheme, "No Multi-Rigs yet. Add two or more presets to the mix (a preset's menu), then save it from the Mix tab.",
                                           13.0f);
        none->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(32, PadXY(12, 0, none)));
    }
    if (mPresetCount)
        mPresetCount->Set(std::to_string(mState.mixer.compositePresets.size()) + " Multi-Rigs");
    return VScroll(Col(rows));
}
} // namespace nanoq::ui
