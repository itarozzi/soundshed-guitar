#pragma once

/**
 * Riffs.h - The riff library and the take being captured, as a UI shows them. Capture, trim,
 * preview and the library are the engine's (PluginControllerRiffs.cpp); these read what it
 * reports and word it as the web UI's riffLibrary.ts does.
 */

#include "uiclient/ClientState.h"

#include <string>
#include <vector>

namespace guitarfx::uiclient
{
/// The most a capture can hold, in bars: the web UI arms with this, and the live waveform's
/// buckets span it.
inline constexpr int kRiffCaptureMaxBars = 64;

/// The riffs of a "riffLibrary" object ({path, riffs}).
[[nodiscard]] std::vector<Riff> ParseRiffs(const nlohmann::json& library);

/// How far through its buffer a take being recorded is (0..1). The engine's live peaks span the
/// whole buffer (64 bars at the take's tempo and meter, in 256 buckets), so this is where the
/// trace stops. `sampleRate` is the device's, used until the take reports its own.
[[nodiscard]] double RiffCaptureFill(const RiffCapture& capture, double sampleRate);

/// The take's length in seconds, or 0 when unknown.
[[nodiscard]] double RiffCaptureSeconds(const RiffCapture& capture, double sampleRate);

/// The capture dialog's status line: armed, recording, captured, or ready.
[[nodiscard]] std::string RiffCaptureStatus(const RiffCapture& capture, double sampleRate);

/// A take in one line: "4 bars · 120 BPM · 4/4 · 0:08".
[[nodiscard]] std::string RiffTakeSummary(const RiffTake& take);

/// A riff's second line: its tags, whether it is used, and how many takes it has.
[[nodiscard]] std::string RiffMeta(const Riff& riff);

/// "a, b ,c" as {"a", "b", "c"}: the save form's comma-separated fields.
[[nodiscard]] std::vector<std::string> SplitRiffList(const std::string& text);

/// Seconds as the trim markers show them: "1.25 s".
[[nodiscard]] std::string FormatRiffSeconds(double seconds);
} // namespace guitarfx::uiclient
