/**
 * The one call a parameter change makes to bring the panel's visualisations up
 * to date: the EQ curve, the spatial panner and the cabinet response. Each
 * checks the node's type itself, so callers need not know which one it has.
 */

import type { GraphNode } from "../../types.js";
import { updateCabResponseVisualization } from "./cabResponse.js";
import { updateEqVisualization } from "./eq.js";
import { updateSpatialVisualization } from "./spatial.js";

export function refreshNodeVisualizations(node: GraphNode): void {
  updateEqVisualization(node);
  updateSpatialVisualization(node);
  updateCabResponseVisualization(node);
}
