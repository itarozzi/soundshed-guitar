#include "controller/TunerService.h"

#include "MessageHandlerRegistry.h"
#include "dsp/MultiPresetMixer.h"

#include <utility>

#include <nlohmann/json.hpp>

namespace guitarfx
{
TunerService::TunerService(SendMessageFn sendMessage, MultiPresetMixer& mixer, std::mutex& dspMutex)
    : mSendMessage(std::move(sendMessage)), mMixer(mixer), mDSPMutex(dspMutex)
{
}

void TunerService::RegisterMessageHandlers(MessageHandlerRegistry& registry)
{
    registry.Register("tuner", [this](const nlohmann::json& payload) { HandleTunerRequest(payload); });
}

void TunerService::HandleTunerRequest(const nlohmann::json& payload)
{
    const std::string action = payload.value("action", "");

    if (action == "start")
    {
        SetActive(true);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);

            if (payload.contains("liveMode"))
            {
                mMixer.SetLiveTunerMode(payload.value("liveMode", true));
            }

            if (payload.contains("referenceFrequency"))
            {
                mMixer.SetTunerReferenceFrequency(payload["referenceFrequency"].get<double>());
            }

            mMixer.SetTunerEnabled(true);
            referenceFrequency = mMixer.GetTunerReferenceFrequency();
            liveMode = mMixer.IsLiveTunerMode();
        }

        nlohmann::json message;
        message["type"] = "tunerStarted";
        message["referenceFrequency"] = referenceFrequency;
        message["liveMode"] = liveMode;
        mSendMessage(message.dump());
        return;
    }

    if (action == "stop")
    {
        SetActive(false);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mMixer.SetTunerEnabled(false);
        }

        nlohmann::json message;
        message["type"] = "tunerStopped";
        mSendMessage(message.dump());
        return;
    }

    if (action == "setLiveMode")
    {
        bool liveMode = payload.value("liveMode", true);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mMixer.SetLiveTunerMode(liveMode);
        }

        nlohmann::json message;
        message["type"] = "tunerLiveModeChanged";
        message["liveMode"] = liveMode;
        mSendMessage(message.dump());
        return;
    }

    if (action == "setReference")
    {
        double freq = payload.value("referenceFrequency", 440.0);
        double effectiveFrequency = 440.0;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mMixer.SetTunerReferenceFrequency(freq);
            effectiveFrequency = mMixer.GetTunerReferenceFrequency();
        }

        nlohmann::json message;
        message["type"] = "tunerReferenceChanged";
        message["referenceFrequency"] = effectiveFrequency;
        mSendMessage(message.dump());
        return;
    }

    if (payload.contains("enabled"))
    {
        bool enabled = payload.value("enabled", false);
        SetActive(enabled);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mMixer.SetTunerEnabled(enabled);
            referenceFrequency = mMixer.GetTunerReferenceFrequency();
            liveMode = mMixer.IsLiveTunerMode();
        }

        nlohmann::json reply;
        reply["type"] = enabled ? "tunerStarted" : "tunerStopped";
        reply["referenceFrequency"] = referenceFrequency;
        reply["liveMode"] = liveMode;
        mSendMessage(reply.dump());
    }
}

void TunerService::PostReading(const Reading& reading)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mReading = reading;
    }
    mPending.store(true, std::memory_order_release);
}

void TunerService::OnIdle()
{
    if (!mPending.load(std::memory_order_acquire))
    {
        return;
    }

    mPending.store(false, std::memory_order_release);

    Reading reading;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        reading = mReading;
    }

    nlohmann::json msg;
    msg["type"] = "tunerUpdate";
    msg["noteName"] = reading.noteName;
    msg["octave"] = reading.octave;
    msg["frequency"] = reading.frequency;
    msg["centOffset"] = reading.centOffset;
    msg["confidence"] = reading.confidence;
    msg["detected"] = reading.detected;

    if (mSendMessage)
    {
        mSendMessage(msg.dump());
    }
}
} // namespace guitarfx
