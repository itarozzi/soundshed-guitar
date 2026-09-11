/**
 * The buttons that act on the node as a whole rather than on one parameter.
 */

import { openCustomEffectDesigner } from "../../customEffectDesigner.js";
import { getBlendState, updateBlendMatchSummary } from "../../signalPathBlend.js";
import type { GraphNode, Preset } from "../../types.js";
import { toggleSignalPathNodeBypass } from "../bypass.js";
import { sendSignalPathNodeConfigUpdate } from "../commands.js";
import { promptSaveCurrentCustomEffect } from "../customEffectActions.js";
import { nodeParamsPanelElement } from "../state.js";

export function bindCustomEffectActionControls(node: GraphNode): void {
  const designButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".custom-effect-design-btn");
  designButton?.addEventListener("click", () => {
    void openCustomEffectDesigner(node);
  });

  const saveButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".custom-effect-save-btn");
  saveButton?.addEventListener("click", () => {
    promptSaveCurrentCustomEffect(node, false);
  });

  const useButton = nodeParamsPanelElement?.querySelector<HTMLButtonElement>(".custom-effect-use-btn");
  useButton?.addEventListener("click", () => {
    promptSaveCurrentCustomEffect(node, true);
  });
}

export function bindBypassButton(node: GraphNode, preset: Preset): void {
  const bypassButtons = document.querySelectorAll<HTMLButtonElement>("#node-params-panel .node-bypass-btn");
  bypassButtons.forEach((bypassBtn) => {
    bypassBtn.addEventListener("click", () => {
      toggleSignalPathNodeBypass(node, preset);
    });
  });
}

export function bindBlendModeOverride(node: GraphNode): void {
  const select = nodeParamsPanelElement?.querySelector<HTMLSelectElement>(".blend-mode-select");
  if (!select) {
    return;
  }
  select.addEventListener("change", () => {
    const value = select.value;
    node.config.blendMode = value;
    sendSignalPathNodeConfigUpdate(node.id, "blendMode", value);
    // Update the matched-model summary in place to reflect the new mode.
    const blendState = getBlendState(node);
    if (blendState) {
      updateBlendMatchSummary(nodeParamsPanelElement, node, blendState);
    }
  });
}
