#pragma once

/**
 * Looper.h - The looper as a UI shows it. Recording, layers, re-toning and projects are the
 * engine's (LooperService*.cpp); these read what it reports and word it as the web UI's
 * looper.ts and looper/strip.ts do.
 */

#include "uiclient/ClientState.h"

#include <string>
#include <vector>

namespace guitarfx::uiclient
{
/// What the one state button does next ("Rec", "Rec now", "Play", "Dub", "Dub now").
[[nodiscard]] std::string LooperMainButtonLabel(const std::string& state);

/// The state as a word: "Ready", "Count-in", "Play to start", "Recording", ...
[[nodiscard]] std::string LooperStatusLabel(const std::string& state);

/// "0:03.2 / 0:08.0", or while the first take records just how long it is.
[[nodiscard]] std::string LooperTimeText(const LooperState& looper, double positionSec);

/// "bar 2 of 4 · 3 layers", or just the layers for a free loop.
[[nodiscard]] std::string LooperBarText(const LooperState& looper);

/// The sentence a notice code stands for, or "" for none.
[[nodiscard]] std::string LooperNoticeText(const std::string& notice);

/// Whether a state is a take in progress (counting in, armed or recording).
[[nodiscard]] bool LooperTaking(const std::string& state);

/// The strip's waveform: every audible layer's peaks summed across the loop (muted and
/// recording layers left out, solo respected), scaled to the loudest moment, 256 buckets.
[[nodiscard]] std::vector<float> LooperMixPeaks(const LooperState& looper);

/// The clock LooperState::positionAt is read on, in seconds.
[[nodiscard]] double LooperClockNow();

/// The playhead now: the last reported position moved on by the time since, wrapped by the
/// loop's length while it plays.
[[nodiscard]] double LooperPositionNow(const LooperState& looper, double nowSeconds);

/// A layer's swatch hue in degrees, as the web UI picks it from its order.
[[nodiscard]] float LooperLayerHue(const LooperLayer& layer);
} // namespace guitarfx::uiclient
