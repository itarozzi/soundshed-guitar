#pragma once

/**
 * Metronome - the meter catalogue and grouping helpers the metronome's pickers need (the web UI's
 * metronomeMeters.ts). They follow core/src/controller/internal/MetronomeSupport.h: the engine
 * normalises whatever it is sent (and re-seeds the accent pattern when the meter changes), so
 * the UIs only list what can be picked and draw where a bar's groups start.
 */

#include <string>
#include <vector>

namespace guitarfx::uiclient
{
struct TimeSignatureOption
{
    std::string id;       // "7/8:2+2+3"
    std::string label;    // "7/8 (2+2+3)"
    int num = 4;
    int den = 4;
    std::string grouping; // empty when the meter divides evenly
    bool compound = false;
};

/// Everything the meter picker offers, in the order it shows them.
[[nodiscard]] const std::vector<TimeSignatureOption>& TimeSignatures();

/// "7/8 (2+2+3)", or "4/4" for a meter with no grouping.
[[nodiscard]] std::string DescribeTimeSignature(int num, int den, const std::string& grouping);

/// "2+2+3" as beat counts; empty when malformed or when it does not add up to the bar.
[[nodiscard]] std::vector<int> ParseGrouping(const std::string& grouping, int beatsPerBar);

/// For each beat of the bar, whether it starts a group: beat 1 always, then each group of the
/// grouping, or the threes a compound meter falls into.
[[nodiscard]] std::vector<bool> GroupHeads(int num, int den, const std::string& grouping);
} // namespace guitarfx::uiclient
