#include "controller/GateLearnService.h"

#include "MessageHandlerRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/effects/NoiseGateEffect.h"

#include <cmath>
#include <utility>

namespace guitarfx
{
namespace
{
using Clock = std::chrono::steady_clock;

/// How long a listen may take before the gate is taken to be hearing nothing: the listen
/// itself, plus room for a rebuild or a slow first block.
constexpr auto kTimeout = std::chrono::milliseconds(6000);
/// Progress reports while listening, often enough for a smooth bar.
constexpr auto kProgressInterval = std::chrono::milliseconds(100);

double Rounded(double value)
{
    return std::round(value * 10.0) / 10.0;
}
} // namespace

GateLearnService::GateLearnService(SendMessageFn sendMessage, MultiPresetMixer& mixer, std::mutex& dspMutex,
                                   ActivePresetIdFn activePresetId)
    : mSendMessage(std::move(sendMessage)), mActivePresetId(std::move(activePresetId)), mMixer(mixer),
      mDSPMutex(dspMutex)
{
}

void GateLearnService::RegisterMessageHandlers(MessageHandlerRegistry& registry)
{
    registry.Register("gateLearn", [this](const nlohmann::json& payload) { HandleRequest(payload); });
}

void GateLearnService::HandleRequest(const nlohmann::json& payload)
{
    const std::string action = payload.value("action", "start");

    if (action == "cancel")
    {
        Cancel();
        return;
    }

    if (action != "start")
    {
        return;
    }

    Target target;
    target.scope = payload.value("scope", "global");
    target.presetId = payload.value("presetId", "");
    target.nodeId = payload.value("nodeId", target.scope == "global" ? "global_gate" : "");

    if (target.scope == "preset" && target.presetId.empty() && mActivePresetId)
    {
        target.presetId = mActivePresetId();
    }

    if ((target.scope != "global" && target.scope != "preset") || target.nodeId.empty() ||
        (target.scope == "preset" && target.presetId.empty()))
    {
        mTarget = std::move(target);
        Send("failed", {{"reason", "notFound"}});
        return;
    }

    Start(std::move(target));
}

void GateLearnService::Start(Target target)
{
    if (mActive)
    {
        // One listen at a time: a new one replaces the old, which says so.
        Cancel();
    }

    mTarget = std::move(target);
    ++mSession;

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        auto* gate = FindGate();

        if (!gate)
        {
            Send("failed", {{"reason", "notFound"}});
            return;
        }

        gate->Learner().Start(mSession);
        mMixer.SetHoldAwake(true);
    }

    mActive = true;
    mStarted = Clock::now();
    mLastProgressSent = mStarted;
    Send("listening", {{"progress", 0.0}});
}

void GateLearnService::Cancel()
{
    if (!mActive)
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (auto* gate = FindGate())
        {
            gate->Learner().Cancel();
        }
    }

    Finish();
    Send("cancelled");
}

void GateLearnService::Finish()
{
    mActive = false;
    mMixer.SetHoldAwake(false);
}

NoiseGateEffect* GateLearnService::FindGate() const
{
    EffectProcessor* processor = mTarget.scope == "global" ? mMixer.GetGlobalPreNodeProcessor(mTarget.nodeId)
                                                           : mMixer.GetNodeProcessor(mTarget.presetId, mTarget.nodeId);
    return dynamic_cast<NoiseGateEffect*>(processor);
}

void GateLearnService::OnIdle()
{
    if (!mActive)
    {
        return;
    }

    const auto now = Clock::now();
    const bool timedOut = now - mStarted > kTimeout;
    GateNoiseLearner::Snapshot snapshot;
    double thresholdDb = 0.0;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (auto* gate = FindGate())
        {
            found = true;
            snapshot = gate->Learner().Read();

            if (snapshot.session != mSession && !timedOut)
            {
                // Rebuilt since the listen began: this is a new gate, so ask it afresh.
                gate->Learner().Start(mSession);
            }
            else if (snapshot.state == GateNoiseLearner::State::Done)
            {
                thresholdDb = gate->LearnedThresholdDb(snapshot.noiseFloorDb);
            }
            else if (timedOut)
            {
                gate->Learner().Cancel();
            }
        }
    }

    const bool done = found && snapshot.session == mSession && snapshot.state == GateNoiseLearner::State::Done;

    if (done)
    {
        Finish();
        const nlohmann::json measured{{"noiseFloorDb", Rounded(snapshot.noiseFloorDb)},
                                      {"spreadDb", Rounded(snapshot.spreadDb)}};

        if (!GateNoiseLearner::HeardOnlyNoise(snapshot))
        {
            nlohmann::json failure = measured;
            failure["reason"] = "playing";
            Send("failed", failure);
            return;
        }

        nlohmann::json result = measured;
        result["thresholdDb"] = Rounded(thresholdDb);
        Send("done", result);
        return;
    }

    if (timedOut)
    {
        // Found but never finished means no audio reached it: a bypassed gate, or a stopped
        // audio device.
        Finish();
        Send("failed", {{"reason", found ? "noSignal" : "notFound"}});
        return;
    }

    if (found && snapshot.session == mSession && now - mLastProgressSent >= kProgressInterval)
    {
        mLastProgressSent = now;
        Send("listening",
             {{"progress", Rounded(snapshot.progress * 100.0) / 100.0}, {"levelDb", Rounded(snapshot.levelDb)}});
    }
}

void GateLearnService::Send(const std::string& state, const nlohmann::json& extra) const
{
    nlohmann::json message = extra;
    message["type"] = "gateLearnStatus";
    message["state"] = state;
    message["scope"] = mTarget.scope;
    message["nodeId"] = mTarget.nodeId;

    if (!mTarget.presetId.empty())
    {
        message["presetId"] = mTarget.presetId;
    }

    mSendMessage(message.dump());
}
} // namespace guitarfx
