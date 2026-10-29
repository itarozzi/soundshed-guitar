#pragma once

/**
 * SplitRouting.h - What a splitter's routing reads as on screen, as the web UI's
 * signalPath/paramsPanel/splitBranches.ts words it: which of its parameters a split of so many
 * branches uses, and a line saying what the branches get.
 */

#include "presets/PresetTypes.h"

#include <string>

namespace guitarfx::uiclient
{
/// The splitter's outgoing edges: one per branch.
[[nodiscard]] int CountSplitBranches(const SignalGraph& graph, const std::string& splitterId);

/// False for a split note the split has no branch for: Split Note 2 needs three branches,
/// Split Note 3 four. Every other key is in use.
[[nodiscard]] bool IsSplitterParamInUse(const std::string& key, int branches);

/// What each branch gets in the splitter's mode ("Soft playing to branch 1, hard to branch 2"),
/// from its mode and branch count alone, so it only changes when the page is redrawn anyway; the
/// knobs show the values. Empty in Copy, where every branch gets everything.
[[nodiscard]] std::string DescribeSplitRouting(const GraphNode& splitter, int branches);
} // namespace guitarfx::uiclient
