/**
 * Analysis.cpp - effect response requests, the analysis feeds' leases, and the editors'
 * measuring tools (see Analysis.h). AnalysisParsing.cpp reads their replies.
 */

#include "uiclient/Analysis.h"

#include "uiclient/UiClient.h"

namespace guitarfx::uiclient
{
namespace
{
/// Leases the engine drops 5 s after the last renewal; renewed every 2 s, as the web UI does.
constexpr double kLeaseRenewalSeconds = 2.0;

nlohmann::json AddressPayload(const NodeAddress& node)
{
    nlohmann::json payload = {{"scope", node.scope}, {"nodeId", node.nodeId}};

    if (!node.presetId.empty())
    {
        payload["presetId"] = node.presetId;
    }

    return payload;
}
} // namespace

Analysis::Analysis(UiClient& client) : mClient(client)
{
}

std::string Analysis::NextId(const char* kind)
{
    return std::string("native-") + kind + "-" + std::to_string(++mSequence);
}

void Analysis::RequestResponse(const std::string& lane, const std::string& effectType, const nlohmann::json& params,
                               bool bands, ResponseCallback callback)
{
    CurveRequest request{effectType, params.is_object() ? params : nlohmann::json::object(), bands,
                         std::move(callback)};
    auto& state = mLanes[lane];

    if (state.inFlight)
    {
        state.queued = std::move(request);
        return;
    }

    SendCurve(lane, state, std::move(request));
}

void Analysis::SendCurve(const std::string& laneName, Lane& lane, CurveRequest request)
{
    lane.inFlightId = NextId(("response-" + laneName).c_str());
    lane.sentAt = mNow;
    nlohmann::json payload = {{"requestId", lane.inFlightId},
                              {"effectType", request.effectType},
                              {"params", request.params},
                              {"points", kResponsePoints}};

    if (request.bands)
    {
        payload["bands"] = true;
    }

    lane.inFlight = std::move(request);
    mClient.Send("getEffectResponse", std::move(payload));
}

void Analysis::OnEffectResponse(const nlohmann::json& reply)
{
    const auto requestId = reply.value("requestId", std::string());

    for (auto& [name, lane] : mLanes)
    {
        if (!lane.inFlight || lane.inFlightId != requestId)
        {
            continue;
        }

        auto request = std::move(*lane.inFlight);
        lane.inFlight.reset();
        lane.inFlightId.clear();

        if (lane.queued)
        {
            auto next = std::move(*lane.queued);
            lane.queued.reset();
            SendCurve(name, lane, std::move(next));
        }

        // Last: the callback may ask again, which must find the lane in its new state.
        if (request.callback)
        {
            request.callback(ParseResponseCurve(reply));
        }

        return;
    }
}

void Analysis::WatchSpectrum(const NodeAddress& node)
{
    if (mSpectrum == node)
    {
        return;
    }

    mSpectrum = node;
    mClient.Send("setSpectrumWatch", AddressPayload(node));
}

void Analysis::StopSpectrum()
{
    if (!mSpectrum)
    {
        return;
    }

    mSpectrum.reset();
    mClient.Send("setSpectrumWatch", nlohmann::json::object());
}

void Analysis::WatchLoudness(const NodeAddress& node)
{
    if (mLoudness == node)
    {
        return;
    }

    mLoudness = node;
    mClient.Send("setLoudnessWatch", AddressPayload(node));
}

void Analysis::StopLoudness()
{
    if (!mLoudness)
    {
        return;
    }

    mLoudness.reset();
    mClient.Send("setLoudnessWatch", nlohmann::json::object());
}

void Analysis::WatchOutputLoudness(bool enabled)
{
    if (enabled == mOutputLoudness)
    {
        return;
    }

    mOutputLoudness = enabled;
    mClient.Send("setLoudnessWatch",
                 enabled ? nlohmann::json{{"slot", 1}, {"scope", "output"}} : nlohmann::json{{"slot", 1}});
}

void Analysis::AnalyzeIrAlignment(const nlohmann::json& irA, const nlohmann::json& irB, IrAlignmentCallback callback)
{
    mAlignmentId = NextId("ir-alignment");
    mAlignmentSentAt = mNow;
    mAlignmentCallback = std::move(callback);
    nlohmann::json payload = {{"requestId", mAlignmentId}};

    if (irA.is_object())
    {
        payload["irA"] = irA;
    }

    if (irB.is_object())
    {
        payload["irB"] = irB;
    }

    mClient.Send("analyzeIrAlignment", std::move(payload));
}

void Analysis::OnIrAlignment(const nlohmann::json& reply)
{
    if (mAlignmentId.empty() || reply.value("requestId", std::string()) != mAlignmentId)
    {
        return;
    }

    mAlignmentId.clear();
    auto callback = std::move(mAlignmentCallback);
    mAlignmentCallback = nullptr;
    const auto alignment = ParseIrAlignment(reply);

    if (callback)
    {
        callback(alignment, alignment ? std::string() : reply.value("error", std::string("The analysis failed")));
    }
}

void Analysis::RequestIrAlignmentResponse(const nlohmann::json& irA, const nlohmann::json& irB,
                                          const nlohmann::json& params, double offsetMs,
                                          IrAlignmentResponseCallback callback)
{
    AlignmentResponseRequest request{irA, irB, params.is_object() ? params : nlohmann::json::object(), offsetMs,
                                     std::move(callback)};

    if (mAlignmentResponseInFlight)
    {
        mAlignmentResponseQueued = std::move(request);
        return;
    }

    SendAlignmentResponse(std::move(request));
}

void Analysis::SendAlignmentResponse(AlignmentResponseRequest request)
{
    mAlignmentResponseId = NextId("ir-alignment-response");
    mAlignmentResponseSentAt = mNow;
    nlohmann::json payload = {{"requestId", mAlignmentResponseId},
                              {"params", request.params},
                              {"offsetMs", request.offsetMs},
                              {"points", kResponsePoints}};

    if (request.irA.is_object())
    {
        payload["irA"] = request.irA;
    }

    if (request.irB.is_object())
    {
        payload["irB"] = request.irB;
    }

    mAlignmentResponseInFlight = std::move(request);
    mClient.Send("getIrAlignmentResponse", std::move(payload));
}

void Analysis::FinishAlignmentResponse(const std::optional<IrAlignmentResponse>& response, const std::string& error)
{
    auto request = std::move(*mAlignmentResponseInFlight);
    mAlignmentResponseInFlight.reset();
    mAlignmentResponseId.clear();

    if (mAlignmentResponseQueued)
    {
        auto next = std::move(*mAlignmentResponseQueued);
        mAlignmentResponseQueued.reset();
        SendAlignmentResponse(std::move(next));
    }

    // Last: the callback may ask again, which must find the queue in its new state.
    if (request.callback)
    {
        request.callback(response, error);
    }
}

void Analysis::OnIrAlignmentResponse(const nlohmann::json& reply)
{
    if (!mAlignmentResponseInFlight || reply.value("requestId", std::string()) != mAlignmentResponseId)
    {
        return;
    }

    const auto response = ParseIrAlignmentResponse(reply);
    FinishAlignmentResponse(response,
                            response ? std::string() : reply.value("error", std::string("No response came back")));
}

void Analysis::MatchSimpleCabToIr(const std::string& resourceId, MatchCallback callback)
{
    mMatchId = NextId("cab-match");
    mMatchSentAt = mNow;
    mMatchCallback = std::move(callback);
    mClient.Send("matchSimpleCabToIr", {{"requestId", mMatchId}, {"resourceId", resourceId}});
}

void Analysis::OnSimpleCabMatch(const nlohmann::json& reply)
{
    if (mMatchId.empty() || reply.value("requestId", std::string()) != mMatchId)
    {
        return;
    }

    mMatchId.clear();
    auto callback = std::move(mMatchCallback);
    mMatchCallback = nullptr;

    if (!callback)
    {
        return;
    }

    const auto params = reply.find("params");

    if (reply.contains("error") || params == reply.end() || !params->is_object())
    {
        callback(std::nullopt, reply.value("error", std::string("No match was found")));
        return;
    }

    SimpleCabMatch match;
    match.rmsErrorDb = reply.value("rmsErrorDb", 0.0);

    for (const auto& [key, value] : params->items())
    {
        if (value.is_number())
        {
            match.params[key] = value.get<double>();
        }
    }

    callback(match, {});
}

std::string Analysis::ExportEffectAsIr(const std::string& effectType, const nlohmann::json& params,
                                       const std::string& name)
{
    const auto requestId = NextId("effect-ir");
    mClient.MutableState().resourceImports[requestId] = {ResourceImport::Status::Importing, "ir", {}, {}};
    nlohmann::json payload = {{"requestId", requestId}, {"effectType", effectType}, {"params", params}};

    if (!name.empty())
    {
        payload["name"] = name;
    }

    mClient.Send("exportEffectAsIr", std::move(payload));
    return requestId;
}

void Analysis::StartGateLearn(const NodeAddress& gate)
{
    auto payload = AddressPayload(gate);
    payload["action"] = "start";
    mClient.Send("gateLearn", std::move(payload));
}

void Analysis::CancelGateLearn(const NodeAddress& gate)
{
    auto payload = AddressPayload(gate);
    payload["action"] = "cancel";
    mClient.Send("gateLearn", std::move(payload));
}

void Analysis::Tick(double nowSeconds)
{
    mNow = nowSeconds;

    if (nowSeconds - mLastRenewal >= kLeaseRenewalSeconds)
    {
        mLastRenewal = nowSeconds;

        if (mSpectrum)
        {
            mClient.Send("setSpectrumWatch", AddressPayload(*mSpectrum));
        }

        if (mLoudness)
        {
            mClient.Send("setLoudnessWatch", AddressPayload(*mLoudness));
        }

        if (mOutputLoudness)
        {
            mClient.Send("setLoudnessWatch", {{"slot", 1}, {"scope", "output"}});
        }
    }

    // A request the engine never answers must not hold back every curve after it.
    for (auto& [name, lane] : mLanes)
    {
        if (lane.inFlight && nowSeconds - lane.sentAt >= kResponseTimeoutSeconds)
        {
            lane.inFlight.reset();
            lane.inFlightId.clear();

            if (lane.queued)
            {
                auto next = std::move(*lane.queued);
                lane.queued.reset();
                SendCurve(name, lane, std::move(next));
            }
        }
    }

    if (mAlignmentResponseInFlight && nowSeconds - mAlignmentResponseSentAt >= kResponseTimeoutSeconds)
    {
        FinishAlignmentResponse(std::nullopt, "The engine did not answer");
    }

    if (!mAlignmentId.empty() && nowSeconds - mAlignmentSentAt >= kToolTimeoutSeconds)
    {
        mAlignmentId.clear();
        auto callback = std::move(mAlignmentCallback);
        mAlignmentCallback = nullptr;

        if (callback)
        {
            callback(std::nullopt, "The engine did not answer");
        }
    }

    if (!mMatchId.empty() && nowSeconds - mMatchSentAt >= kToolTimeoutSeconds)
    {
        mMatchId.clear();
        auto callback = std::move(mMatchCallback);
        mMatchCallback = nullptr;

        if (callback)
        {
            callback(std::nullopt, "The engine did not answer");
        }
    }
}
} // namespace guitarfx::uiclient
