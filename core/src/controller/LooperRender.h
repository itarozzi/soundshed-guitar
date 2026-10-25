#pragma once

// LooperRender — re-tone: a layer's DI rendered offline through a preset, on graphs of its own.
//
// The live wet sound went through the global pre-chain, the rig and the global post-chain. So the
// render builds all three detached (as PresetLevelMeter builds its preset; the controller does it on
// the message thread, without the DSP lock) and runs them on the looper's worker, never touching the
// live mixer or the DSP lock. The DI gets the layer's stored input calibration, as the chain heard
// it, and the rig's output its pan law at the default mix and pan, as a lone rig plays.
//
// The loop is rendered twice round and the second pass kept, moved back by the graphs' latency, so
// the amp has warmed up, reverbs and delays are full, and the tails that cross the loop point are
// in it: the result loops with no step at the seam.

#include "dsp/SignalGraphExecutor.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace guitarfx::looper_render
{
using Channels = std::vector<std::vector<float>>;

inline constexpr int kBlockSize = 512;

/// What a render plays through. Prepared at the render's rate and kBlockSize.
struct Graphs
{
    std::unique_ptr<SignalGraphExecutor> pre;
    std::unique_ptr<SignalGraphExecutor> preset;
    std::unique_ptr<SignalGraphExecutor> post;
    std::string presetId;
    std::string presetName;
    std::string sceneId;
    float slotGainLeft = 1.0f;
    float slotGainRight = 1.0f;
};

/// The wet sound, stereo, `di`'s length (the loop's). `di` is one or two channels, the whole loop
/// with silence outside what was played. `progress` (0..1) is updated as it goes, when given.
/// Resets the graphs first, so one Graphs can render several layers in turn.
[[nodiscard]] Channels RenderLoop(Graphs& graphs, const Channels& di, float calibrationGain,
                                  std::atomic<float>* progress = nullptr);
} // namespace guitarfx::looper_render
