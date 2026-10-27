#include "uiclient/ChainDropRules.h"

#include <cmath>

namespace guitarfx::uiclient
{
bool IsBypassFlick(double deltaX, double deltaY)
{
    return std::abs(deltaY) >= kBypassFlickDistance && std::abs(deltaY) > std::abs(deltaX) * kBypassFlickRatio;
}

NodeDropAction ResolveNodeDrop(const std::string& draggedNodeId, const NodeDropTarget& target, double deltaX, double deltaY,
                               bool releasedOnAnotherLine)
{
    NodeDropAction action;
    if (draggedNodeId.empty())
        return action;
    action.nodeId = draggedNodeId;

    switch (target.kind)
    {
        case NodeDropTarget::Kind::None:
            if (!releasedOnAnotherLine && IsBypassFlick(deltaX, deltaY))
                action.kind = NodeDropAction::Kind::ToggleBypass;
            return action;

        case NodeDropTarget::Kind::Edge:
            // Dropping back onto one of the node's own connections changes nothing.
            if (target.edge.from == draggedNodeId || target.edge.to == draggedNodeId)
                return action;
            action.kind = NodeDropAction::Kind::ReorderToEdge;
            action.edge = target.edge;
            return action;

        case NodeDropTarget::Kind::Node:
            break;
    }

    if (target.nodeId == draggedNodeId)
        return action;

    // Dragged leftwards, the user aims at the slot the target holds now, so its incoming edge
    // is used. A target fed by more than one edge joins parallel branches, where "the slot
    // before it" is ambiguous, so that falls back to inserting after.
    if (target.isLeftOfDragged && target.incoming.size() == 1)
    {
        const auto& edge = target.incoming.front();
        if (edge.from != draggedNodeId && edge.to != draggedNodeId)
        {
            action.kind = NodeDropAction::Kind::ReorderToEdge;
            action.edge = edge;
            return action;
        }
    }

    action.kind = NodeDropAction::Kind::ReorderAfterNode;
    action.targetNodeId = target.nodeId;
    return action;
}

std::vector<GraphEdge> IncomingEdges(const SignalGraph& graph, const std::string& nodeId)
{
    std::vector<GraphEdge> edges;
    for (const auto& edge : graph.edges)
        if (edge.to == nodeId)
            edges.push_back(edge);
    return edges;
}
} // namespace guitarfx::uiclient
