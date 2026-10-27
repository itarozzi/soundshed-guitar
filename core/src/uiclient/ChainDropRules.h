#pragma once

/**
 * ChainDropRules.h - What dropping a dragged chain card does.
 *
 * The web UI's rules (core/ui/ts/signalPathDropTargets.ts, resolveNodeDropAction), so a drag
 * lands the same way in every UI: dropped on a card it goes after that card, or, dragged
 * leftwards onto a card with one input, into that card's slot; dropped on a cable it goes on
 * that edge; a mostly vertical flick that lands on nothing toggles its bypass. The caller
 * does the hit-testing and hands the outcome here.
 */

#include "presets/PresetTypes.h"

#include <string>
#include <vector>

namespace guitarfx::uiclient
{
/// Minimum vertical travel before a drag counts as a bypass flick.
inline constexpr double kBypassFlickDistance = 36.0;
/// How much more vertical than horizontal that travel has to be.
inline constexpr double kBypassFlickRatio = 1.2;

struct NodeDropTarget
{
    enum class Kind
    {
        None,
        Node,
        Edge
    };

    Kind kind = Kind::None;
    std::string nodeId;              // Node
    bool isLeftOfDragged = false;    // Node: it sits to the left of the dragged card on screen
    std::vector<GraphEdge> incoming; // Node: the edges arriving at it
    GraphEdge edge;                  // Edge
};

struct NodeDropAction
{
    enum class Kind
    {
        None,
        ReorderAfterNode, // into the slot directly after targetNodeId
        ReorderToEdge,    // onto `edge`
        ToggleBypass
    };

    Kind kind = Kind::None;
    std::string nodeId;
    std::string targetNodeId;
    GraphEdge edge;
};

[[nodiscard]] bool IsBypassFlick(double deltaX, double deltaY);

/// `releasedOnAnotherLine`: a wrapped chain's card let go over another line than its own, where
/// a straight up or down move that misses is a missed move, not a flick.
[[nodiscard]] NodeDropAction ResolveNodeDrop(const std::string& draggedNodeId, const NodeDropTarget& target, double deltaX,
                                             double deltaY, bool releasedOnAnotherLine = false);

/// The edges arriving at a node.
[[nodiscard]] std::vector<GraphEdge> IncomingEdges(const SignalGraph& graph, const std::string& nodeId);
} // namespace guitarfx::uiclient
