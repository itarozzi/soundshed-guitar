#include "controller/GateLearnService.h"

#include "MessageHandlerRegistry.h"
#include "dsp/MultiPresetMixer.h"
#include "dsp/EffectProcessor.h"
#include "dsp/effects/NoiseGateLearn.h"

#include <cmath>
#include <utility>
#include <vector>

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

        StartListening(*gate);
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
            for (int band = 0; band < gate->NoiseLearnBandCount(); ++band)
            {
                gate->NoiseLearner(band).Cancel();
            }
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

NoiseLearnHost* GateLearnService::FindGate() const
{
    EffectProcessor* processor = mTarget.scope == "global" ? mMixer.GetGlobalPreNodeProcessor(mTarget.nodeId)
                                                           : mMixer.GetNodeProcessor(mTarget.presetId, mTarget.nodeId);
    return dynamic_cast<NoiseLearnHost*>(processor);
}

void GateLearnService::StartListening(NoiseLearnHost& gate) const
{
    for (int band = 0; band < gate.NoiseLearnBandCount(); ++band)
    {
        gate.NoiseLearner(band).Start(mSession);
    }
}

void GateLearnService::OnIdle()
{
    if (!mActive)
    {
        return;
    }

    const auto now = Clock::now();
    const bool timedOut = now - mStarted > kTimeout;
    // One snapshot and suggested threshold per band: one for the noise gate, six for the
    // Multi-band Gate. Its learners are fed in lockstep, so they finish together.
    std::vector<GateNoiseLearner::Snapshot> snapshots;
    std::vector<double> thresholdsDb;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        if (auto* gate = FindGate())
        {
            found = true;
            const int bands = gate->NoiseLearnBandCount();
            bool stale = false;
            bool done = true;

            for (int band = 0; band < bands; ++band)
            {
                snapshots.push_back(gate->NoiseLearner(band).Read());
                stale = stale || snapshots.back().session != mSession;
                done = done && snapshots.back().state == GateNoiseLearner::State::Done;
            }

            if (stale && !timedOut)
            {
                // Rebuilt since the listen began: this is a new gate, so ask it afresh.
                StartListening(*gate);
            }
            else if (!stale && done)
            {
                for (int band = 0; band < bands; ++band)
                {
                    thresholdsDb.push_back(
                        gate->LearnedThresholdDb(band, snapshots[static_cast<std::size_t>(band)].noiseFloorDb));
                }
            }
            else if (timedOut)
            {
                for (int band = 0; band < bands; ++band)
                {
                    gate->NoiseLearner(band).Cancel();
                }
            }
        }
    }

    const bool done = found && !snapshots.empty() && thresholdsDb.size() == snapshots.size();
    const bool multiband = snapshots.size() > 1;

    if (done)
    {
        Finish();
        bool heardOnlyNoise = true;
        nlohmann::json measured = nlohmann::json::object();
        nlohmann::json bands = nlohmann::json::array();

        for (std::size_t band = 0; band < snapshots.size(); ++band)
        {
            const auto& snapshot = snapshots[band];
            heardOnlyNoise = heardOnlyNoise && GateNoiseLearner::HeardOnlyNoise(snapshot);
            bands.push_back({{"noiseFloorDb", Rounded(snapshot.noiseFloorDb)},
                             {"spreadDb", Rounded(snapshot.spreadDb)},
                             {"thresholdDb", Rounded(thresholdsDb[band])}});
        }

        if (multiband)
        {
            measured["bands"] = bands;
        }
        else
        {
            measured = bands[0];
            measured.erase("thresholdDb");
        }

        if (!heardOnlyNoise)
        {
            nlohmann::json failure = measured;
            failure["reason"] = "playing";

            if (multiband)
            {
                for (auto& band : failure["bands"])
                {
                    band.erase("thresholdDb");
                }
            }

            Send("failed", failure);
            return;
        }

        nlohmann::json result = measured;

        if (!multiband)
        {
            result["thresholdDb"] = Rounded(thresholdsDb[0]);
        }

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

    const bool current = found && !snapshots.empty() && snapshots[0].session == mSession;

    if (current && now - mLastProgressSent >= kProgressInterval)
    {
        mLastProgressSent = now;
        nlohmann::json progress{{"progress", Rounded(snapshots[0].progress * 100.0) / 100.0}};

        if (multiband)
        {
            nlohmann::json bands = nlohmann::json::array();

            for (const auto& snapshot : snapshots)
            {
                bands.push_back({{"levelDb", Rounded(snapshot.levelDb)}});
            }

            progress["bands"] = bands;
        }
        else
        {
            progress["levelDb"] = Rounded(snapshots[0].levelDb);
        }

        Send("listening", progress);
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
