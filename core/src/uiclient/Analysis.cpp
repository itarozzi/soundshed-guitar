/**
 * Analysis.cpp - effect response requests, the analysis feeds' leases, and the editors'
 * measuring tools (see Analysis.h).
 */

#include "uiclient/Analysis.h"

#include "uiclient/UiClient.h"
#include "util/Base64.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace guitarfx::uiclient
{
namespace
{
/// Leases the engine drops 5 s after the last renewal; renewed every 2 s, as the web UI does.
constexpr double kLeaseRenewalSeconds = 2.0;

std::optional<std::vector<double>> NumberArray(const nlohmann::json& value)
{
    if (!value.is_array())
    {
        return std::nullopt;
    }

    std::vector<double> numbers;
    numbers.reserve(value.size());

    for (const auto& item : value)
    {
        if (!item.is_number() || !std::isfinite(item.get<double>()))
        {
            return std::nullopt;
        }

        numbers.push_back(item.get<double>());
    }

    return numbers;
}

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

double ResponseDbAt(const std::vector<double>& frequencies, const std::vector<double>& magnitudesDb, double hz)
{
    const std::size_t count = std::min(frequencies.size(), magnitudesDb.size());

    if (count == 0)
    {
        return 0.0;
    }

    if (hz <= frequencies.front() || count == 1)
    {
        return magnitudesDb.front();
    }

    if (hz >= frequencies[count - 1])
    {
        return magnitudesDb[count - 1];
    }

    const auto high = static_cast<std::size_t>(
        std::upper_bound(frequencies.begin(), frequencies.begin() + static_cast<std::ptrdiff_t>(count), hz) - frequencies.begin());
    const std::size_t low = high - 1;
    const double t = std::log(hz / frequencies[low]) / std::log(frequencies[high] / frequencies[low]);
    return magnitudesDb[low] + t * (magnitudesDb[high] - magnitudesDb[low]);
}

double ResponseDbAt(const ResponseCurve& curve, double hz)
{
    return ResponseDbAt(curve.frequencies, curve.magnitudesDb, hz);
}

std::string NodeFeedKey(const std::string& scope, const std::string& presetId, const std::string& nodeId)
{
    return (scope == "pre" || scope == "post" || scope == "global" ? scope : presetId) + "::" + nodeId;
}

std::vector<float> DecodeFloat32Base64(const std::string& text)
{
    const auto bytes = util::DecodeBase64(text);

    if (bytes.size() % 4 != 0)
    {
        return {};
    }

    std::vector<float> samples(bytes.size() / 4);

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const std::uint32_t bits = static_cast<std::uint32_t>(bytes[i * 4]) | (static_cast<std::uint32_t>(bytes[i * 4 + 1]) << 8) |
                                   (static_cast<std::uint32_t>(bytes[i * 4 + 2]) << 16) |
                                   (static_cast<std::uint32_t>(bytes[i * 4 + 3]) << 24);
        std::memcpy(&samples[i], &bits, sizeof(float));
    }

    return samples;
}

std::optional<ResponseCurve> ParseResponseCurve(const nlohmann::json& reply)
{
    if (!reply.value("supported", false))
    {
        return std::nullopt;
    }

    auto frequencies = NumberArray(reply.value("frequencies", nlohmann::json()));
    auto magnitudes = NumberArray(reply.value("magnitudesDb", nlohmann::json()));

    if (!frequencies || !magnitudes || frequencies->size() < 2 || frequencies->size() != magnitudes->size())
    {
        return std::nullopt;
    }

    ResponseCurve curve;
    curve.frequencies = std::move(*frequencies);
    curve.magnitudesDb = std::move(*magnitudes);

    if (const auto summary = reply.find("summary"); summary != reply.end() && summary->is_object())
    {
        for (const auto& [name, value] : summary->items())
        {
            if (value.is_number() && std::isfinite(value.get<double>()))
            {
                curve.summary[name] = value.get<double>();
            }
        }
    }

    // Band curves only when every one is complete: a plot shades all of them or none.
    if (const auto bands = reply.find("bandsDb"); bands != reply.end() && bands->is_array())
    {
        std::vector<std::vector<double>> parsed;

        for (const auto& band : *bands)
        {
            auto values = NumberArray(band);

            if (!values || values->size() != curve.frequencies.size())
            {
                parsed.clear();
                break;
            }

            parsed.push_back(std::move(*values));
        }

        curve.bandsDb = std::move(parsed);
    }

    return curve;
}

std::optional<IrAlignment> ParseIrAlignment(const nlohmann::json& reply)
{
    if (reply.contains("error"))
    {
        return std::nullopt;
    }

    const auto byOffset = NumberArray(reply.value("matchByOffset", nlohmann::json()));

    if (!byOffset || !reply.contains("sampleRate"))
    {
        return std::nullopt;
    }

    IrAlignment alignment;
    alignment.sampleRate = reply.value("sampleRate", 48000.0);
    alignment.waveformA = DecodeFloat32Base64(reply.value("waveformA", ""));
    alignment.waveformB = DecodeFloat32Base64(reply.value("waveformB", ""));
    alignment.normalizeGainA = reply.value("normalizeGainA", 1.0);
    alignment.normalizeGainB = reply.value("normalizeGainB", 1.0);
    alignment.alignedOffsetMs = reply.value("alignedOffsetMs", 0.0);
    alignment.invertB = reply.value("invertB", false);
    alignment.match = reply.value("match", 0.0);
    alignment.onsetAMs = reply.value("onsetAMs", 0.0);
    alignment.onsetBMs = reply.value("onsetBMs", 0.0);
    alignment.maxOffsetSamples = reply.value("maxOffsetSamples", 0);
    alignment.matchByOffset = *byOffset;
    return alignment;
}

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
    CurveRequest request{effectType, params.is_object() ? params : nlohmann::json::object(), bands, std::move(callback)};
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
    mClient.Send("setLoudnessWatch", enabled ? nlohmann::json{{"slot", 1}, {"scope", "output"}} : nlohmann::json{{"slot", 1}});
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

std::string Analysis::ExportEffectAsIr(const std::string& effectType, const nlohmann::json& params, const std::string& name)
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
