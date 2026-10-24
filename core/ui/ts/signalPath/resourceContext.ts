import type {
  GraphNode,
  Preset,
} from "../types.js";
import { EffectTypeRegistry, getNodeEffectInfo } from "../presetV2.js";
import { EffectGuids } from "../effectGuids.js";
import { getNodeCategory } from "./nodeTypes.js";
import { hasCabIrInSameSignalPath } from "./chainRules.js";
import { withResourceSlotContext } from "../resourceBrowser/settings.js";
export function resolveResourceBrowserTone3000CategoryFilter(
  node: GraphNode,
  preset: Preset,
): "pedal" | "amp" | "full-rig" | undefined {
  const resolvedType = EffectTypeRegistry.resolve(node.type);

  if (resolvedType === EffectGuids.kFxNam) {
    return "pedal";
  }

  if (resolvedType === EffectGuids.kAmpNamOptimized) {
    return hasCabIrInSameSignalPath(node.id, preset) ? "amp" : "full-rig";
  }

  return undefined;
}

/// The library category an IR slot browses: what the engine files its IRs under. "ir" is the
/// resource type, not a category, since reverb IRs are type "ir" too.
export function resolveResourceBrowserLibraryCategoryHint(
  node: GraphNode,
  resourceType: "nam" | "ir",
): "cab" | "reverb" | undefined {
  if (resourceType !== "ir") {
    return undefined;
  }

  const category = getNodeEffectInfo(node)?.category || getNodeCategory(node);
  if (category === "reverb") {
    return "reverb";
  }
  if (category === "cab") {
    return "cab";
  }

  return undefined;
}

export function resolveResourceNavigationCategoryHint(
  node: GraphNode,
  preset: Preset,
  resourceType: "nam" | "ir",
): string | undefined {
  if (resourceType === "nam") {
    return resolveResourceBrowserTone3000CategoryFilter(node, preset);
  }

  return resolveResourceBrowserLibraryCategoryHint(node, resourceType);
}

/// Identifies the kind of node a resource is being picked for, so folder browsing
/// and next/prev navigation are remembered per effect role rather than globally.
/// Deliberately coarser than the category hint: a NAM Amp keeps one remembered
/// folder whether or not it is currently acting as a full rig.
///
/// The IR Cabinet's second slot is a context of its own ("ir-cab#1"), so IR A and IR B each
/// step through the list they were last picked from: the library, a folder or a Tone3000 set.
export function resolveResourceContextKey(node: GraphNode, resourceType: "nam" | "ir", resourceIndex = 0): string {
  if (resourceType === "nam") {
    return EffectTypeRegistry.resolve(node.type) === EffectGuids.kFxNam ? "nam-fx" : "nam-amp";
  }

  if (resolveResourceBrowserLibraryCategoryHint(node, resourceType) === "reverb") {
    return "ir-reverb";
  }
  return resourceIndex > 0 && EffectTypeRegistry.resolve(node.type) === EffectGuids.kCabIr
    ? withResourceSlotContext("ir-cab", resourceIndex)
    : "ir-cab";
}
