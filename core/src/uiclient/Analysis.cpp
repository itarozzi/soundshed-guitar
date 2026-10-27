/**
 * Analysis.cpp - effect response requests, the analysis feeds' leases, and the editors'
 * measuring tools (see Analysis.h).
 */

#include "uiclient/Analysis.h"

#include "uiclient/UiClient.h"
#include "util/Base64.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

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

std::vector<AnalyzerReadout> AnalyzerReadouts(const AnalyzerFrame* frame, int configuredChannels)
{
    static const std::string kNone = "\xE2\x80\x94";
    const auto number = [](double value, const char* unit, int digits) {
        if (!std::isfinite(value))
            return kNone;
        char text[48];
        std::snprintf(text, sizeof(text), "%.*f %s", digits, value, unit);
        return std::string(text);
    };
    const auto lufs = [&number, frame](const std::optional<double>& value) {
        return frame != nullptr && frame->loudnessValid && value ? number(*value, "LUFS", 1) : kNone;
    };
    const auto dbfs = [](double percent) {
        if (!std::isfinite(percent))
            return std::numeric_limits<double>::quiet_NaN();
        const double linear = std::clamp(percent / 100.0, 0.0, 1.0);
        return linear <= 1.0e-9 ? -120.0 : 20.0 * std::log10(linear);
    };

    std::vector<AnalyzerReadout> readouts = {{"Peak (dBFS)", kNone, {}},      {"RMS (dBFS)", kNone, {}},       {"Peak (%FS)", kNone, {}},
                                             {"RMS (%FS)", kNone, {}},        {"RMS (dBu)", kNone, {}},        {"RMS (dBV)", kNone, {}},
                                             {"RMS (Vrms)", kNone, {}},       {"Momentary (LUFS)", kNone, {}}, {"Short-term (LUFS)", kNone, {}},
                                             {"Integrated (LUFS)", kNone, {}}, {"Channels", kNone, {}}};
    if (frame == nullptr)
        return readouts;

    readouts[0].value = number(dbfs(frame->peakPercent), "dBFS", 1);
    readouts[1].value = number(dbfs(frame->rmsPercent), "dBFS", 1);
    readouts[2].value = number(frame->peakPercent, "%", 1);
    readouts[3].value = number(frame->rmsPercent, "%", 1);
    readouts[4].value = number(frame->rmsDbu, "dBu", 1);
    readouts[5].value = number(frame->rmsDbv, "dBV", 1);
    readouts[6].value = number(frame->rmsVolts, "Vrms", 3);
    readouts[7].value = lufs(frame->momentaryLufs);
    readouts[8].value = lufs(frame->shortTermLufs);
    readouts[9].value = lufs(frame->integratedLufs);

    // The layout the engine runs the node in, never guessed; what is measured is a hint when the
    // two disagree (a stereo input with one side silent, or both sides the same).
    auto& channels = readouts[10];
    if (configuredChannels == 1)
        channels.value = "Mono";
    else if (configuredChannels == 2)
    {
        channels.value = "Stereo";
        channels.hint = frame->activeChannelCount == 1 ? "one side silent" : frame->stereo ? "" : "sides identical";
    }
    else
    {
        channels.value = frame->stereo ? "Stereo" : "Mono";
        if (frame->activeChannelCount > 0)
            channels.hint = std::to_string(frame->activeChannelCount) + " ch";
    }
    return readouts;
}

std::string PickupDesignSummary(const std::map<std::string, double>& summary, const std::string& side)
{
    const auto get = [&summary, &side](const char* name) -> std::optional<double> {
        const auto it = summary.find(side + name);
        return it != summary.end() && std::isfinite(it->second) ? std::optional<double>(it->second) : std::nullopt;
    };
    const auto length = get("WireLengthM"), inductance = get("InductanceH"), resistance = get("ResistanceKohm"),
               capacitance = get("CapacitancePf"), peak = get("PeakHz"), q = get("Q"), level = get("LevelDb");
    if (!length || !inductance || !resistance || !capacitance || !peak || !q || !level)
        return {};

    // As toLocaleString in English: thousands grouped with commas.
    const auto format = [](double value, int digits) {
        char text[48];
        std::snprintf(text, sizeof(text), "%.*f", digits, std::abs(value));
        std::string s = text;
        const auto dot = s.find('.');
        for (auto i = static_cast<long>(dot == std::string::npos ? s.size() : dot) - 3; i > 0; i -= 3)
            s.insert(static_cast<std::size_t>(i), ",");
        return (value < 0 ? "-" : "") + s;
    };
    const std::string dotSep = " \xC2\xB7 ";
    const std::string sign = *level >= 0 ? "+" : "\xE2\x88\x92";
    return format(*length, 0) + " m of wire" + dotSep + format(*inductance, 2) + " H" + dotSep + format(*resistance, 2) + " k\xCE\xA9" +
           dotSep + format(*capacitance, 0) + " pF \xE2\x86\x92 peak " + format(*peak / 1000.0, 2) + " kHz, Q " + format(*q, 2) +
           ", output " + sign + format(std::abs(*level), 1) + " dB";
}

std::string GateLearnStatusText(const GateLearnStatus* status, double floorDb)
{
    // Whole dB with a true minus sign, as the web UI writes them.
    const auto db = [](double value) {
        const long rounded = std::lround(value);
        return (rounded < 0 ? std::string("\xE2\x88\x92") + std::to_string(-rounded) : std::to_string(rounded)) + " dB";
    };

    if (status == nullptr || status->state.empty())
        return "Mute the strings, then Learn.";
    if (status->state == "listening")
        return "Listening\xE2\x80\xA6 keep the strings quiet";
    if (status->state == "done")
    {
        if (!status->noiseFloorDb || !status->thresholdDb)
            return "Threshold set";
        const auto noise = *status->noiseFloorDb < floorDb ? "under " + db(floorDb) : db(*status->noiseFloorDb);
        return "Noise " + noise + " \xC2\xB7 threshold set to " + db(*status->thresholdDb);
    }
    if (status->state == "failed")
    {
        if (status->reason == "playing")
            return "Heard playing. Mute the strings and try again.";
        if (status->reason == "noSignal")
            return "No audio reached the gate. Is the audio device running?";
        return "This gate is not in the running chain.";
    }
    if (status->state == "cancelled")
        return "Learn cancelled.";
    return "Mute the strings, then Learn.";
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

    const bool hasA = reply.contains("waveformA") && reply["waveformA"].is_string();
    const bool hasB = reply.contains("waveformB") && reply["waveformB"].is_string();

    if (!(reply.value("sampleRate", 0.0) > 0.0) || (!hasA && !hasB))
    {
        return std::nullopt;
    }

    // One IR alone comes back with its waveform and nothing to align (irAlignment.ts parseIrAlignment).
    if (!hasA || !hasB)
    {
        IrAlignment single;
        single.sampleRate = reply.value("sampleRate", 48000.0);
        single.waveformA = DecodeFloat32Base64(reply.value("waveformA", ""));
        single.waveformB = DecodeFloat32Base64(reply.value("waveformB", ""));
        single.normalizeGainA = reply.value("normalizeGainA", 1.0);
        single.normalizeGainB = reply.value("normalizeGainB", 1.0);
        single.matchByOffset = {0.0};
        return single;
    }

    const auto byOffset = NumberArray(reply.value("matchByOffset", nlohmann::json()));
    const int maxOffsetSamples = reply.value("maxOffsetSamples", 0);

    if (!byOffset || byOffset->size() != static_cast<std::size_t>(2 * maxOffsetSamples + 1))
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

std::optional<IrAlignmentResponse> ParseIrAlignmentResponse(const nlohmann::json& reply)
{
    if (reply.contains("error"))
    {
        return std::nullopt;
    }

    auto frequencies = NumberArray(reply.value("frequencies", nlohmann::json()));
    auto summed = NumberArray(reply.value("summedDb", nlohmann::json()));

    if (!frequencies || !summed || frequencies->size() < 2 || summed->size() != frequencies->size())
    {
        return std::nullopt;
    }

    IrAlignmentResponse response;
    const auto slots = reply.value("slots", std::string("both"));
    response.slots = slots == "a" || slots == "b" ? slots : "both";
    response.offsetMs = reply.value("offsetMs", 0.0);

    // A pair is drawn against its powers summed; without them there is nothing to compare.
    if (response.slots == "both")
    {
        auto unrelated = NumberArray(reply.value("unrelatedDb", nlohmann::json()));

        if (!unrelated || unrelated->size() != frequencies->size())
        {
            return std::nullopt;
        }

        response.unrelatedDb = std::move(*unrelated);
    }

    response.frequencies = std::move(*frequencies);
    response.summedDb = std::move(*summed);
    return response;
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
