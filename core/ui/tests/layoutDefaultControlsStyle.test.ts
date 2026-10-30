import { afterEach, describe, expect, it } from "vitest";
import { CanvasProperties } from "../ts/layoutDesigner/canvasProperties.js";
import { defaultControlsPreviewHtml } from "../ts/layoutDesigner/controlPreview.js";
import type { LayoutPropertiesHost } from "../ts/layoutDesigner/propertiesHost.js";
import { mainTabOnly } from "../ts/layoutDefaultControls.js";
import { renderCustomLayoutBackdrop } from "../ts/layoutRenderer.js";
import { layoutRequirements } from "../ts/layoutRequirements.js";
import type { EffectLayout } from "../ts/layoutTypes.js";
import type { ParameterDef } from "../ts/presetV2.js";
import type { GraphNode } from "../ts/types.js";

// A default-controls layout can show only the Main tab, and drop the group panels.

const node = { id: "n", type: "fx", params: {} } as unknown as GraphNode;

/** The params panel's markup for a Main and an Advanced tab, one group each. */
const TABBED = `
  <div class="node-param-tabs" role="tablist">
    <button class="node-param-tab is-active" data-tab="main">Main</button>
    <button class="node-param-tab" data-tab="advanced">Advanced</button>
  </div>
  <div class="node-param-tab-panels">
    <div class="node-param-tab-panel" data-tab="main"><div class="params-controls" id="main-controls">
      <div class="node-param-group-block"><div class="node-param-group-title">Tone</div><div class="node-param-group-items">knobs</div></div>
    </div></div>
    <div class="node-param-tab-panel is-active" data-tab="advanced"><div class="params-controls" id="advanced-controls">advanced</div></div>
  </div>`;

function layout(extra: Partial<EffectLayout> = {}): EffectLayout {
  return {
    effectType: "fx", version: 1, dimensions: { width: 400, height: 280 }, backgrounds: [], controls: [], textLabels: [],
    useDefaultControls: true,
    ...extra,
  };
}

function mount(html: string): HTMLElement {
  const host = document.createElement("div");
  host.innerHTML = html;
  document.body.replaceChildren(host);
  return host;
}

afterEach(() => document.body.replaceChildren());

describe("Main tab only", () => {
  it("keeps the Main tab's controls in place of the tabs, whichever tab was open", () => {
    const host = mount(mainTabOnly(TABBED));
    expect(host.querySelector(".node-param-tabs, .node-param-tab-panels, .node-param-tab-panel")).toBeNull();
    expect(host.querySelector("#main-controls")).not.toBeNull();
    expect(host.querySelector("#advanced-controls")).toBeNull();
    expect(mainTabOnly("<div class=\"params-controls\">x</div>")).toBe("<div class=\"params-controls\">x</div>");
  });

  it("applies in the live panel's backdrop only when the layout asks", () => {
    expect(mount(renderCustomLayoutBackdrop(node, layout({ defaultControlsMainOnly: true }), TABBED)).querySelector(".node-param-tabs")).toBeNull();
    expect(mount(renderCustomLayoutBackdrop(node, layout(), TABBED)).querySelector(".node-param-tabs")).not.toBeNull();
  });

  it("applies in the designer's preview, built from the effect's params", () => {
    const defs = [
      { key: "gain", name: "Gain", default: 0.5, min: 0, max: 1, unit: "amount" },
      { key: "bias", name: "Bias", default: 0.5, min: 0, max: 1, unit: "amount", advanced: true },
    ] as ParameterDef[];
    const host = mount(defaultControlsPreviewHtml(layout({ defaultControlsMainOnly: true }), defs, 800));
    expect(host.querySelector(".node-param-tabs")).toBeNull();
    expect([...host.querySelectorAll<HTMLElement>(".node-param-knob")].map((knob) => knob.dataset.paramKey)).toEqual(["gain"]);
  });
});

describe("group panels", () => {
  it("are dropped by a class on the controls' wrapper, in the panel and the designer", () => {
    const panel = mount(renderCustomLayoutBackdrop(node, layout({ defaultControlsPanels: false }), TABBED));
    expect(panel.querySelector(".custom-layout-default-controls")!.classList.contains("no-group-panels")).toBe(true);
    const plain = mount(renderCustomLayoutBackdrop(node, layout(), TABBED));
    expect(plain.querySelector(".no-group-panels")).toBeNull();
    const preview = mount(defaultControlsPreviewHtml(layout({ defaultControlsPanels: false }), [], 800));
    expect(preview.querySelector(".layout-default-controls-preview")!.classList.contains("no-group-panels")).toBe(true);
  });
});

describe("as stored and edited", () => {
  it("is a newer feature only on a default-controls layout", () => {
    expect(layoutRequirements(layout({ defaultControlsMainOnly: true }))).toEqual(["default-controls-style"]);
    expect(layoutRequirements(layout({ defaultControlsPanels: false }))).toEqual(["default-controls-style"]);
    expect(layoutRequirements(layout({ defaultControlsPanels: true }))).toEqual([]);
    expect(layoutRequirements(layout({ useDefaultControls: false, defaultControlsMainOnly: true }))).toEqual([]);
  });

  it("the canvas panel's Default Controls group sets both, storing only what differs", () => {
    const l = layout();
    const sidebar = document.createElement("div");
    document.body.replaceChildren(sidebar);
    const host: LayoutPropertiesHost = {
      getLayout: () => l, getParamDefs: () => [], getResourceCandidates: () => [], getSidebarContent: () => sidebar,
      pushUndoState: () => {}, pushSidebarUndoOnce: () => {}, renderCanvas: () => {}, selectElement: () => {},
      renderSidebar: () => {}, renderImageOptionsHtml: () => "",
      browseBackgroundImage: () => {}, browseKnobImage: () => {}, isResourceControl: () => false,
    };
    new CanvasProperties(host).renderCanvasProperties();
    const check = (id: string, checked: boolean) => {
      const input = document.getElementById(id) as HTMLInputElement;
      input.checked = checked;
      input.dispatchEvent(new Event("change"));
    };
    expect((document.getElementById("prop-dc-panels") as HTMLInputElement).checked).toBe(true);
    check("prop-dc-main-only", true);
    check("prop-dc-panels", false);
    expect(l).toMatchObject({ defaultControlsMainOnly: true, defaultControlsPanels: false });
    check("prop-dc-main-only", false);
    check("prop-dc-panels", true);
    expect("defaultControlsMainOnly" in l || "defaultControlsPanels" in l).toBe(false);
  });
});
