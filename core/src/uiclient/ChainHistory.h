#pragma once

/**
 * ChainHistory - undo/redo for the signal chain, and the A/B pair that owns two stacks of it,
 * as the web UI keeps them (core/ui/ts/signalPath/historyModel.ts and history.ts).
 *
 * Pure data: it holds graphs and says which one to apply; the client that owns it reads the live
 * graph and talks to the engine.
 *
 * - A snapshot is a copy of a graph plus a signature, a serialisation with sorted keys, so "did
 *   this edit change anything?" is a string compare.
 * - A slot is one undo stack. There are two, A and B, and one is live at a time. Switching is
 *   not an undo step in either: each keeps its cursor where the user left it.
 * - A restore whose graph has the same nodes, types and wiring as the live one is replayed as
 *   parameter, config and bypass messages (DiffNodeStates); anything else needs the whole graph
 *   loaded, which rebuilds the chain and reloads its models.
 */

#include "presets/PresetTypes.h"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace guitarfx::uiclient
{
/// How many edits one slot remembers before the oldest falls off.
inline constexpr std::size_t kChainHistoryLimit = 100;

struct ChainSnapshot
{
    std::string presetId; // a mismatch with the live preset means the snapshot is stale
    std::string sceneId;
    SignalGraph graph;
    std::string signature;
    std::string label; // what the edit that produced it did, for the undo button's tooltip
};

/// One node's worth of state to send again when replaying a same-topology change.
struct NodeStateChange
{
    std::string nodeId;
    std::vector<std::pair<std::string, double>> params;
    std::vector<std::pair<std::string, std::string>> config;
    std::optional<bool> bypassed; // set when the bypass differs
};

/// The graph's comparable state as one string: every part of a node a user edit can change, and
/// the edges in a fixed order. Engine-owned config (a hosted plugin's state blob) is left out.
[[nodiscard]] std::string GraphSignature(const SignalGraph& graph);

[[nodiscard]] ChainSnapshot MakeSnapshot(const SignalGraph& graph, std::string presetId, std::string sceneId,
                                         std::string label);

/// The per-node changes that turn `from` into `to`, or nullopt when the targeted messages cannot
/// say it: a different topology, a renamed node, a swapped resource, or a key that appeared or
/// disappeared (there is no message that unsets one).
[[nodiscard]] std::optional<std::vector<NodeStateChange>> DiffNodeStates(const SignalGraph& from,
                                                                         const SignalGraph& to);

using NodeLabelResolver = std::function<std::string(const GraphNode&)>;

/// A short description of what one edit did ("Add Doubler", "Noise Gate off"). Best effort: it
/// reads the difference, it is not told what happened.
[[nodiscard]] std::string DescribeGraphChange(const SignalGraph* from, const SignalGraph& to,
                                              const NodeLabelResolver& label);

class ChainHistory
{
  public:
    enum class Slot
    {
        A,
        B
    };

    /// Starts over from `seed`: both slots hold it, A live.
    void Reset(const ChainSnapshot& seed);

    [[nodiscard]] bool IsSeeded() const;

    [[nodiscard]] Slot ActiveSlot() const
    {
        return mActive;
    }

    [[nodiscard]] const ChainSnapshot* Current() const;

    /// Records a new state; from a past cursor position the redo entries go.
    void Push(ChainSnapshot snapshot);
    /// Rewrites the state under the cursor without an undo step: the engine's echo of a restore.
    void AmendCurrent(ChainSnapshot snapshot);

    [[nodiscard]] bool CanUndo() const;
    [[nodiscard]] bool CanRedo() const;
    /// The snapshot to apply, or null.
    const ChainSnapshot* Undo();
    const ChainSnapshot* Redo();
    [[nodiscard]] std::string UndoLabel() const;
    [[nodiscard]] std::string RedoLabel() const;

    /// Makes `slot` live and returns its state to apply, or null when it already was.
    const ChainSnapshot* SetActiveSlot(Slot slot);
    /// Copies the live state over the other slot as a new step there.
    bool CopyActiveToOther();
    /// Edits behind a slot, for its tooltip.
    [[nodiscard]] int EditCount(Slot slot) const;

  private:
    struct Stack
    {
        std::vector<ChainSnapshot> entries;
        int index = -1; // entries[index] is the state the chain is in now
    };

    Stack& Live()
    {
        return mActive == Slot::A ? mA : mB;
    }

    [[nodiscard]] const Stack& Live() const
    {
        return mActive == Slot::A ? mA : mB;
    }

    static void PushTo(Stack& stack, ChainSnapshot snapshot);

    Stack mA, mB;
    Slot mActive = Slot::A;
};
} // namespace guitarfx::uiclient
