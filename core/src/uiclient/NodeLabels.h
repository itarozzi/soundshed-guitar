#pragma once

/**
 * NodeLabels.h - What a node is called on screen, as core/ui/ts/signalPath/nodeLabels.ts
 * names it: Input and Output by role; a name the user gave it; an effect that plays a
 * resource by the resource (a NAM model, an IR); otherwise its label, or its effect type's name.
 */

#include "uiclient/ClientState.h"

#include <string>

namespace guitarfx::uiclient
{
[[nodiscard]] std::string NodeDisplayName(const ClientState& state, const GraphNode& node);

/// The loaded resource's name: the library entry's, or the file's name without extension.
[[nodiscard]] std::string NodeResourceName(const ClientState& state, const GraphNode& node, std::size_t index = 0);

/// "A2", "A1", "Custom", or empty: the NAM architecture of the node's model, from its
/// library metadata.
[[nodiscard]] std::string NodeArchitectureBadge(const ClientState& state, const GraphNode& node);

/// The node's category ("amp", "cab", "drive", ...) as the web UI's getNodeCategory reads it:
/// the node's own when it names one (a Neural FX node is "fx"), else its effect type's, else
/// "utility". The caller folds aliases (EffectPresentation::AliasCategory) where it shows one.
[[nodiscard]] std::string NodeCategory(const ClientState& state, const GraphNode& node);

/// Whether a neural model node's every loaded model carries both its input and output
/// calibration (inputLevelDbu and outputLevelDbu): the editor's Calibrated chip
/// (nodeLabels.ts isNodeFullyCalibratedFromNamMetadata).
[[nodiscard]] bool NodeNamFullyCalibrated(const ClientState& state, const GraphNode& node);
} // namespace guitarfx::uiclient
