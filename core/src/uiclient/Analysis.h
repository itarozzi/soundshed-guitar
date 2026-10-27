#pragma once

/**
 * Analysis - what the effect editors ask the engine to measure, and the leases that keep its
 * live analysis feeds running, for the native UIs (the web UI's effectResponse.ts,
 * eqSpectrum.ts, nodeLoudness.ts, irAlignment.ts and gateLearnPanel.ts).
 *
 * Response curves are asked for on every knob move, so they are coalesced as the web UI
 * coalesces them: one request in flight per lane (a lane per plot), and whatever is asked while
 * it is out folds into one follow-up with the newest parameters. The spectrum and loudness taps
 * are leases the engine drops 5 s after the last renewal; Tick renews them every 2 s while held.
 *
 * Everything is called on the UI thread; callbacks run there too, from UiClient::DrainPending,
 * or from Tick when a request goes unanswered.
 */

#include "uiclient/ClientState.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
class UiClient;

/// An effect's response as the engine reports it (getEffectResponse).
struct ResponseCurve
{
    std::vector<double> frequencies;  // Hz, rising
    std::vector<double> magnitudesDb; // at each frequency
    /// Named figures the curve leaves out (Pickup Match's designed pickups).
    std::map<std::string, double> summary;
    /// Each band's own curve, for an EQ asked with `bands`; they sum to magnitudesDb.
    std::vector<std::vector<double>> bandsDb;
};

/// The curve's level at `hz`, interpolated linearly in log frequency and held flat beyond
/// its ends (effectResponse.ts responseDbAt). 0 for an empty curve.
[[nodiscard]] double ResponseDbAt(const ResponseCurve& curve, double hz);
[[nodiscard]] double ResponseDbAt(const std::vector<double>& frequencies, const std::vector<double>& magnitudesDb,
                                  double hz);

/// The reply to analyzeIrAlignment (irAlignment.ts IrAlignmentAnalysis).
struct IrAlignment
{
    double sampleRate = 48000.0;
    std::vector<float> waveformA; // the first 60 ms of each IR, mono
    std::vector<float> waveformB;
    double normalizeGainA = 1.0;
    double normalizeGainB = 1.0;
    double alignedOffsetMs = 0.0;
    bool invertB = false;
    double match = 0.0;
    double onsetAMs = 0.0;
    double onsetBMs = 0.0;
    int maxOffsetSamples = 0;
    std::vector<double> matchByOffset; // -maxOffsetSamples first
};

/// The reply to matchSimpleCabToIr.
struct SimpleCabMatch
{
    std::map<std::string, double> params;
    double rmsErrorDb = 0.0;
};

/// Where a node lives, as the engine's watches address it: scope "pre", "post" or "preset"
/// (a preset node without presetId is looked up in the active preset).
struct NodeAddress
{
    std::string scope;
    std::string nodeId;
    std::string presetId;

    bool operator==(const NodeAddress&) const = default;
};

/// The key the engine's feeds are stored under: "pre::id", "post::id" or "<presetId>::id",
/// as the meters key nodes, and "global::id" for the global gate's Learn. A preset node with
/// no presetId is "::id".
[[nodiscard]] std::string NodeFeedKey(const std::string& scope, const std::string& presetId, const std::string& nodeId);

/// Little-endian float32 samples, base64'd (irAlignment.ts decodeFloat32Base64). Empty for
/// text that is not.
[[nodiscard]] std::vector<float> DecodeFloat32Base64(const std::string& text);

/// The line under a noise gate's Learn meter (gateLearn.ts statusText): what it is doing, or
/// what it heard and set. `floorDb` is the meter's bottom; quieter noise reads as under it.
[[nodiscard]] std::string GateLearnStatusText(const GateLearnStatus* status, double floorDb);

/// Parses an effectResponse reply; nothing when it is not supported or malformed.
[[nodiscard]] std::optional<ResponseCurve> ParseResponseCurve(const nlohmann::json& reply);

/// Parses an irAlignment reply; nothing when it carries an error or is malformed.
[[nodiscard]] std::optional<IrAlignment> ParseIrAlignment(const nlohmann::json& reply);

class Analysis
{
public:
    /// Points per curve: log-spaced 20 Hz to 20 kHz, as the web UI asks.
    static constexpr int kResponsePoints = 160;
    /// A curve request the engine never answers is given up on after this.
    static constexpr double kResponseTimeoutSeconds = 3.0;
    /// IR alignment and the Simple Cabinet match normally take well under a second.
    static constexpr double kToolTimeoutSeconds = 20.0;

    using ResponseCallback = std::function<void(const std::optional<ResponseCurve>&)>;
    using IrAlignmentCallback = std::function<void(const std::optional<IrAlignment>&, const std::string& error)>;
    using MatchCallback = std::function<void(const std::optional<SimpleCabMatch>&, const std::string& error)>;

    explicit Analysis(UiClient& client);

    // ── Response curves ─────────────────────────────────────────────────────
    /// Asks for `effectType`'s response with `params`; `callback` gets the curve, or nothing
    /// when the engine has none. While another request on `lane` is out this one waits, and a
    /// later one replaces it (its callback is never called).
    void RequestResponse(const std::string& lane, const std::string& effectType, const nlohmann::json& params,
                         bool bands, ResponseCallback callback);

    // ── Live feeds ──────────────────────────────────────────────────────────
    /// The spectrum of one node's input behind an EQ curve (sldS), held until stopped.
    void WatchSpectrum(const NodeAddress& node);
    void StopSpectrum();
    /// The loudness of one node's output for its DSP status readout (sldL).
    void WatchLoudness(const NodeAddress& node);
    void StopLoudness();
    /// The loudness of the app's output, after the master volume (sldL slot 1).
    void WatchOutputLoudness(bool enabled);

    // ── Tools ───────────────────────────────────────────────────────────────
    /// Where an IR Cabinet's IR B lines up with IR A; each the slot's ref as the node holds
    /// it, or null for an empty slot. A newer request supersedes one still out.
    void AnalyzeIrAlignment(const nlohmann::json& irA, const nlohmann::json& irB, IrAlignmentCallback callback);
    /// The Simple Cabinet settings closest to library IR `resourceId`.
    void MatchSimpleCabToIr(const std::string& resourceId, MatchCallback callback);
    /// Renders an effect as an IR into the library; the result arrives as resourceImports[id].
    std::string ExportEffectAsIr(const std::string& effectType, const nlohmann::json& params, const std::string& name);
    /// Noise gate Learn; its progress arrives as ClientState::analysis.gateLearn.
    void StartGateLearn(const NodeAddress& gate);
    void CancelGateLearn(const NodeAddress& gate);

    // ── Called by UiClient ──────────────────────────────────────────────────
    void Tick(double nowSeconds);
    void OnEffectResponse(const nlohmann::json& reply);
    void OnIrAlignment(const nlohmann::json& reply);
    void OnSimpleCabMatch(const nlohmann::json& reply);

private:
    struct CurveRequest
    {
        std::string effectType;
        nlohmann::json params;
        bool bands = false;
        ResponseCallback callback;
    };

    struct Lane
    {
        std::string inFlightId;
        double sentAt = 0.0;
        std::optional<CurveRequest> inFlight;
        std::optional<CurveRequest> queued;
    };

    void SendCurve(const std::string& laneName, Lane& lane, CurveRequest request);
    std::string NextId(const char* kind);

    UiClient& mClient;
    std::map<std::string, Lane> mLanes;
    int mSequence = 0;
    double mNow = 0.0;

    std::optional<NodeAddress> mSpectrum;
    std::optional<NodeAddress> mLoudness;
    bool mOutputLoudness = false;
    double mLastRenewal = -1.0e9;

    std::string mAlignmentId;
    double mAlignmentSentAt = 0.0;
    IrAlignmentCallback mAlignmentCallback;
    std::string mMatchId;
    double mMatchSentAt = 0.0;
    MatchCallback mMatchCallback;
};
} // namespace guitarfx::uiclient
