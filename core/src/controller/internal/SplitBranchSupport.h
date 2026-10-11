#pragma once

/**
 * SplitBranchSupport.h — How many parallel branches a split has, and changing it.
 *
 * A split is a splitter node whose outgoing edges are its branches, joined again at a
 * mixer. Each branch is one splitter edge (its fromPort only orders the branches) and
 * arrives on its own mixer input (toPort), which the mixer's per-input level_N, pan_N,
 * delay_N and mute_N params belong to. The graph is the only record of the count: no
 * param stores it, so a preset can never say three while wiring two.
 *
 * Adding a branch adds an empty one, splitter straight to a free mixer input. Only an
 * empty branch can be removed, so changing the count never deletes an effect. Collapsing
 * a split never deletes one either: it needs every branch but one empty, and moves that
 * one's effects back into the main path.
 */

#include <string>

namespace guitarfx
{
struct GraphNode;
struct SignalGraph;
} // namespace guitarfx

namespace guitarfx::controller_detail
{
inline constexpr int kMinSplitBranches = 2;
/// One branch per mixer input: MixerEffect::kMaxInputs (asserted where both are visible).
inline constexpr int kMaxSplitBranches = 4;

[[nodiscard]] bool IsSplitterNode(const GraphNode& node);
[[nodiscard]] bool IsMixerNode(const GraphNode& node);

/// The mixer every branch of this split arrives at, walking past any split nested in a
/// branch; empty when the node is not a splitter or its branches do not meet at one mixer.
[[nodiscard]] std::string FindSplitJoinMixerId(const SignalGraph& graph, const std::string& splitterId);

/// The splitter's outgoing edges, one per branch.
[[nodiscard]] int CountSplitBranches(const SignalGraph& graph, const std::string& splitterId);

struct SplitBranchChange
{
    bool changed = false;
    /// Why nothing changed, for the user; empty on success, including a count already met.
    std::string error;
};

/// Add empty branches, or remove empty ones (highest first), until the split has `count`.
/// On an error the graph is left as it was.
SplitBranchChange SetSplitBranchCount(SignalGraph& graph, const std::string& splitterId, int count);

/// Remove a split, its splitter and its mixer. When one branch holds effects, they move back
/// into the main path where the split was, in the same order; the other branches must be empty.
/// The mixer's level, pan, delay and mute for that branch go with it. On an error the graph is
/// left as it was.
SplitBranchChange CollapseSplit(SignalGraph& graph, const std::string& splitterId, const std::string& mixerId);
} // namespace guitarfx::controller_detail
