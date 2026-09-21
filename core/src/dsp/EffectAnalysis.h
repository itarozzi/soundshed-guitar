#pragma once

#include "dsp/EffectProcessor.h"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * Offline questions about an effect type with a given set of parameters: its frequency
 * response and its impulse response. Each builds a fresh instance on the calling thread and
 * never touches a running graph, so the message thread can answer the UI without the DSP
 * lock, and the answer is the same whether or not the node is playing.
 *
 * Only effects that report a linear response (EffectProcessor::GetFrequencyResponse) take
 * part; for anything else each question returns nothing.
 */
namespace guitarfx::effect_analysis
{
/// The rate the analyses run at: the usual rate for a cabinet IR, and high enough that a
/// curve drawn to 20 kHz is not shaped by the clamp every filter gets near Nyquist.
inline constexpr double kSampleRate = 48000.0;

/// `count` frequencies from 20 Hz to 20 kHz, evenly spaced on a log scale, ends included.
[[nodiscard]] std::vector<double> LogFrequencies(int count);

/// A prepared instance of `type` with `params` applied, or nullptr for an unknown type.
[[nodiscard]] std::unique_ptr<EffectProcessor> CreateConfigured(const std::string& type,
                                                                const std::map<std::string, double>& params);

/// The small-signal magnitude response in dB at each of `frequencies`, or nothing when the
/// type is unknown or has no linear response.
[[nodiscard]] std::optional<std::vector<double>> MagnitudeResponseDb(const std::string& type,
                                                                     const std::map<std::string, double>& params,
                                                                     const std::vector<double>& frequencies);

/// The impulse response, `length` samples per channel at kSampleRate, or nothing when the
/// type is unknown or has no linear response. The impulse goes in quietly and the result is
/// scaled back up, so level-dependent stages (speaker drive) stay out of it, as the
/// frequency response leaves them out. The last tenth fades to silence so a tail cut short
/// cannot click. `channels` holds one vector when both sides came out the same, two when
/// they differ (stereo spread).
struct ImpulseResponse
{
    std::vector<std::vector<float>> channels;
};

[[nodiscard]] std::optional<ImpulseResponse> RenderImpulse(const std::string& type,
                                                           const std::map<std::string, double>& params, int length);
} // namespace guitarfx::effect_analysis
