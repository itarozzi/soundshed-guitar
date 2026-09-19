// PresetVoicePool and PresetInstance: the swap rules on their own, without a mixer or audio.
//
// The mixer-level behaviour (what a swap sounds like, the audio thread racing a walk) is
// covered by GaplessSwitchingTests, PresetRetirementTests and MixerInstanceLockTests. These
// pin the lifetime rules those depend on: which instances are live, what a commit does to
// each, and how the ramps move.

#include "dsp/PresetVoicePool.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

using namespace guitarfx;

namespace
{
int gFailures = 0;

void Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << "\n";
        ++gFailures;
    }
}

bool Near(float a, float b)
{
    return std::abs(a - b) < 1.0e-4f;
}

std::unique_ptr<PresetInstance> MakeInstance(const std::string& id, bool canRingOut = false)
{
    auto inst = std::make_unique<PresetInstance>();
    inst->cfg.id = id;
    inst->cfg.name = id;
    inst->canRingOut = canRingOut;
    inst->ResizeBuffers(64);
    return inst;
}

constexpr int kFade = PresetVoicePool::kFadeSamples;

void TestFadeRamps()
{
    PresetInstance inst;
    inst.BeginFadeIn(kFade);

    float start = 0.0f, end = 0.0f;
    inst.GetFadeGains(kFade / 4, start, end);
    Check(Near(start, 0.0f) && Near(end, 0.25f), "a fade-in ramps up from silence");

    inst.AdvanceRamps(kFade / 2, 100);
    Check(Near(inst.CurrentFadeGain(), 0.5f), "half a fade-in is half gain");

    // Superseded mid-fade-in: the fade-out starts from where it is, not from unity.
    inst.BeginFadeOut(kFade);
    Check(inst.phase == InstancePhase::FadingOut && Near(inst.CurrentFadeGain(), 0.5f),
          "a fade-out resumes from the fade-in's gain");

    inst.AdvanceRamps(kFade, 100);
    Check(inst.IsFinishedFadingOut(), "a fade-out runs out to a finished instance");

    PresetInstance settling;
    settling.BeginFadeIn(kFade);
    settling.AdvanceRamps(kFade, 100);
    Check(settling.phase == InstancePhase::Active && settling.fadeTotalSamples == 0, "a finished fade-in is Active");
}

void TestTailHoldsThenReleases()
{
    PresetInstance inst;
    inst.BeginTail(kFade, 2048);
    Check(inst.phase == InstancePhase::Tailing && inst.tailInput && Near(inst.CurrentFadeGain(), 1.0f),
          "a tail holds the gain it began at");
    Check(inst.IsRetiring() && !inst.IsFinishedFadingOut(), "a tail is retiring but never finished");

    inst.AdvanceRamps(2047, 100);
    Check(inst.phase == InstancePhase::Tailing, "a tail holds for its whole budget");

    inst.AdvanceRamps(1, 100);
    Check(inst.phase == InstancePhase::FadingOut && inst.fadeTotalSamples == 100, "a spent tail starts its release");
}

void TestSlots()
{
    DspReaper reaper;
    reaper.Start(); // as MultiPresetMixer::Prepare() does; reserves the realtime queues
    PresetVoicePool pool(reaper);
    pool.Install(MakeInstance("a"));
    pool.Install(MakeInstance("b"));

    Check(pool.Find("a") != nullptr && pool.LiveCount() == 2, "installed slots are live");
    Check(!pool.Rename("a", "b", "clash"), "a rename onto a live id is refused");
    Check(pool.Rename("a", "c", "renamed") && pool.Find("c") && pool.Find("c")->cfg.name == "renamed",
          "a rename re-keys the slot");
    Check(!pool.Rename("", "d", "x"), "an empty id is refused");

    pool.Remove("b");
    Check(pool.Find("b") == nullptr && pool.LiveCount() == 1 && pool.RetiringCount() == 0,
          "a removed slot goes straight to the reaper");
    reaper.Stop();
}

void TestSwap()
{
    DspReaper reaper;
    reaper.Start(); // as MultiPresetMixer::Prepare() does; reserves the realtime queues
    PresetVoicePool pool(reaper);
    pool.Prepare(48000.0);

    Check(!pool.CommitSwap(), "a swap with nothing staged changes nothing");

    pool.Install(MakeInstance("a"));
    pool.Stage(MakeInstance("b"));
    Check(pool.CommitSwap(), "a staged instance commits");
    Check(pool.Find("a") == nullptr && pool.Find("b") != nullptr, "the outgoing slot is hidden from lookups");
    Check(pool.LiveCount() == 1 && pool.RetiringCount() == 1, "the outgoing slot is retiring, not gone");
    Check(pool.Find("b")->phase == InstancePhase::FadingIn, "the incoming slot fades in");

    pool.AdvanceRamps(kFade);
    pool.CollectFinishedFadeOuts();
    Check(pool.Instances().size() == 1 && pool.RetiringCount() == 0, "a finished fade-out is collected");
    reaper.Stop();
}

void TestTailSpill()
{
    DspReaper reaper;
    reaper.Start(); // as MultiPresetMixer::Prepare() does; reserves the realtime queues
    PresetVoicePool pool(reaper);
    pool.Prepare(48000.0);
    pool.SetTailSeconds(1.0);

    pool.Install(MakeInstance("dry", false));
    pool.Install(MakeInstance("wet", true));
    pool.Stage(MakeInstance("next"));
    pool.CommitSwap();

    const auto& instances = pool.Instances();
    Check(instances[0]->phase == InstancePhase::FadingOut, "a graph that cannot ring is faded out");
    Check(instances[1]->phase == InstancePhase::Tailing && instances[1]->tailSamplesRemaining == 48000,
          "a graph that can ring out holds for the tail length");

    pool.SetTailSeconds(1000.0);
    Check(pool.GetTailSeconds() == PresetVoicePool::kMaxTailSeconds, "the tail length is capped");
    pool.SetTailSeconds(std::numeric_limits<double>::quiet_NaN());
    Check(pool.GetTailSeconds() == 0.0, "a non-finite tail length turns the spill off");
    reaper.Stop();
}

void TestRetireeCaps()
{
    DspReaper reaper;
    reaper.Start(); // as MultiPresetMixer::Prepare() does; reserves the realtime queues
    PresetVoicePool pool(reaper);
    pool.Prepare(48000.0);
    pool.SetTailSeconds(5.0);

    pool.Install(MakeInstance("p0", true));

    for (int i = 1; i <= 6; ++i)
    {
        pool.Stage(MakeInstance("p" + std::to_string(i), true));
        pool.CommitSwap();
    }

    std::size_t tailing = 0;

    for (const auto& inst : pool.Instances())
    {
        tailing += inst->phase == InstancePhase::Tailing ? 1 : 0;
    }

    Check(pool.RetiringCount() <= 3, "fast switching never keeps more than three retirees");
    Check(tailing <= 1, "only one tail rings at a time");
    Check(pool.LiveCount() == 1 && pool.Find("p6") != nullptr, "the last preset is the live one");
    reaper.Stop();
}

void TestReplacementAndAddition()
{
    DspReaper reaper;
    reaper.Start(); // as MultiPresetMixer::Prepare() does; reserves the realtime queues
    PresetVoicePool pool(reaper);
    pool.Prepare(48000.0);

    auto a = MakeInstance("a");
    a->cfg.mix = 0.3;
    a->cfg.pan = -0.5;
    pool.Install(std::move(a));

    auto edited = MakeInstance("a");
    edited->cfg.name = "edited";
    pool.Stage(std::move(edited));
    Check(!pool.CommitReplacement("other"), "a replacement for the wrong id is refused");
    Check(pool.CommitReplacement("a"), "a replacement commits");

    const auto* live = pool.Find("a");
    Check(live && live->cfg.mix == 0.3 && live->cfg.pan == -0.5 && live->cfg.name == "edited",
          "a replacement keeps the slot's mix settings and takes the new name");
    Check(pool.RetiringCount() == 1, "the replaced chain fades out alongside");

    pool.Stage(MakeInstance("a"));
    Check(!pool.CommitAddition("a"), "an addition onto a live id is refused");
    pool.Stage(MakeInstance("b"));
    Check(pool.CommitAddition("b") && pool.Find("b")->phase == InstancePhase::Active, "an addition joins at full gain");
    reaper.Stop();
}

void TestMutedSlotIsNotFaded()
{
    DspReaper reaper;
    reaper.Start(); // as MultiPresetMixer::Prepare() does; reserves the realtime queues
    PresetVoicePool pool(reaper);

    auto muted = MakeInstance("muted");
    muted->cfg.mute = true;
    pool.Install(std::move(muted));
    pool.Stage(MakeInstance("next"));
    pool.CommitSwap();

    Check(pool.Instances().size() == 1 && pool.RetiringCount() == 0, "a muted slot is retired outright on a swap");
    reaper.Stop();
}
} // namespace

int main()
{
    TestFadeRamps();
    TestTailHoldsThenReleases();
    TestSlots();
    TestSwap();
    TestTailSpill();
    TestRetireeCaps();
    TestReplacementAndAddition();
    TestMutedSlotIsNotFaded();

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }

    std::cout << "PresetVoicePoolTests passed\n";
    return 0;
}
