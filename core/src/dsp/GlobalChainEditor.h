#pragma once

#include "dsp/SignalGraphExecutor.h"
#include "presets/PresetTypes.h"

namespace guitarfx
{
/// Message-thread view of the global chain's config and live pre/post executors.
/// The mixer retains executor lifetime and staged swaps; this class owns edit rules.
class GlobalChainEditor
{
  public:
    GlobalChainEditor(GlobalSignalChainConfig& config, SignalGraphExecutor& pre, SignalGraphExecutor& post)
        : mConfig(config), mPre(pre), mPost(post)
    {
    }

    static void NormalizeConfig(GlobalSignalChainConfig& config);

    /// The pre-chain as the executor runs it, from the configured one. The config's transpose
    /// node is on only while it transposes, which is what both UIs show and what is saved; the
    /// running node never stops. It is transparent at 0 st and fades into and out of that by
    /// itself, and it keeps its input history, so neither a shift starting nor one ending
    /// jumps the audio, and a shift never starts on stale audio. It runs the Low Latency engine,
    /// at the configured shift while the node is on and 0 while it is off.
    [[nodiscard]] static SignalGraph LivePreChain(const SignalGraph& configured);

    void SetGateEnabled(bool enabled);
    void SetGateThreshold(double thresholdDb);
    void SetGateAttack(double attackMs);
    void SetGateHold(double holdMs);
    void SetGateRelease(double releaseMs);
    void SetGateHysteresis(double hysteresisDb);
    void SetGateRange(double rangeDb);
    void SetGateStereoLink(bool linked);
    void SetTransposeEnabled(bool enabled);
    void SetTranspose(int semitones);
    void SetEQEnabled(bool enabled);
    void SetEQBandGain(int band, double dB);
    void SetEQBandFrequency(int band, double freq);
    void SetEQBandQ(int band, double q);
    void SetDoublerEnabled(bool enabled);
    void SetDoublerDelay(double delayMs);
    void SetDoublerMix(double mix);
    void SetDoublerDetune(double cents);
    void SetInputGain(double dB);
    [[nodiscard]] double SetOutputGain(double dB);

  private:
    /// Writes one parameter onto the global gate node and the live pre-chain executor.
    /// Every SetGate* value setter is this call: the node is the copy the config is
    /// serialized from, so the two have to move together or a restart loses the edit.
    void SetGateParam(const char* key, double value);

    /// The shift the running transpose node plays for a configured one (see LivePreChain).
    [[nodiscard]] static double LiveTransposeSemitones(const GraphNode& node);

    /// Hands the configured transpose node to the running one, the way LivePreChain does.
    void PushLiveTranspose(const GraphNode& node);

    [[nodiscard]] GraphNode* FindPreNode(const char* id, const char* type);
    [[nodiscard]] GraphNode* FindPostNode(const char* id, const char* type);

    GlobalSignalChainConfig& mConfig;
    SignalGraphExecutor& mPre;
    SignalGraphExecutor& mPost;
};
} // namespace guitarfx
