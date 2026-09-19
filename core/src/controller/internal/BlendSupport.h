#pragma once

/**
 * BlendSupport.h — Blend definitions (amp_nam_blend) as the controller reads them.
 *
 * A blend definition is JSON the UI writes: an id, a name, the NAM models it plays and,
 * per model, the parameter values it was captured at. A preset stores only a blend node's
 * blendId. These helpers turn a definition into the node's model list, and answer which
 * graphs, blends and models refer to which.
 */

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
struct GraphNode;
struct Preset;
struct SignalGraph;
} // namespace guitarfx

namespace guitarfx::controller_detail
{
/// Node config naming the blend definition a blend node plays.
inline constexpr const char* kBlendIdConfigKey = "blendId";

/// Marks a definition registered from a bundled factory archive. Never stored.
inline constexpr const char* kFactoryBlendFlag = "factory";

/// The definition with `blendId` in `library`, or nullptr.
[[nodiscard]] const nlohmann::json* FindBlendDefinition(const nlohmann::json& library, const std::string& blendId);

/// Every model id a definition names, each once: its mappings' first, then any listed only
/// in `models`.
[[nodiscard]] std::vector<std::string> CollectBlendModelIds(const nlohmann::json& blend);

/// The blend a node plays, or empty if it is not a blend node with a blendId.
[[nodiscard]] std::string GetNodeBlendId(const GraphNode& node);

[[nodiscard]] bool GraphPlaysBlend(const SignalGraph& graph, const std::string& blendId);

/// The preset's graph or any of its scenes.
[[nodiscard]] bool PresetUsesBlend(const Preset& preset, const std::string& blendId);

/**
 * Fills each blend node of `graph` in from `library`: the models and the values they were
 * captured at, the definition's blendMode, a label if the node has none, and a starting
 * value for each mapped parameter the node has no value for (the median of the captured
 * values, which is where the UI draws that knob). A node whose blend is not in the library
 * is left with no models, as a stored preset would be.
 */
void ApplyBlendDefinitionsToGraph(SignalGraph& graph, const nlohmann::json& library);
} // namespace guitarfx::controller_detail
