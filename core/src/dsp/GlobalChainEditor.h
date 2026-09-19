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

    void SetGateEnabled(bool enabled);
    void SetGateThreshold(double thresholdDb);
    void SetGateAttack(double attackMs);
    void SetGateHold(double holdMs);
    void SetGateRelease(double releaseMs);
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
    [[nodiscard]] GraphNode* FindPreNode(const char* id, const char* type);
    [[nodiscard]] GraphNode* FindPostNode(const char* id, const char* type);

    GlobalSignalChainConfig& mConfig;
    SignalGraphExecutor& mPre;
    SignalGraphExecutor& mPost;
};
} // namespace guitarfx
