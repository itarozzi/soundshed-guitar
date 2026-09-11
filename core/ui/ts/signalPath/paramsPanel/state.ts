/**
 * State the panel and its control modules both touch: the knobs currently on
 * screen, and the drag handlers attached to the EQ curve and spatial panner.
 */

import type { GenericKnob } from "../../controls.js";
import type { EqCurveInteraction } from "../../eqCurve.js";
import type { SpatialPannerInteraction } from "../../spatialPanner.js";

export const paramsPanelInteractions = {
  eq: null as EqCurveInteraction | null,
  spatial: null as SpatialPannerInteraction | null,
  spatialNodeId: null as string | null
};

/** Knob instances for the current node params panel, keyed by param key. */
export const nodeParamKnobs = new Map<string, GenericKnob>();
