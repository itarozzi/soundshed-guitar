/**
 * The properties panel for the three things placed on a layout: a control bound
 * to a parameter (or a resource), a text label, and a rectangle overlay.
 *
 * Each kind gets a render pass that writes the form and a bind pass that wires
 * it. They are separate because the form is rebuilt whenever the selection
 * changes, and rebinding on every keystroke would drop focus mid-edit.
 */

import { snapToGrid } from "../layoutTypes.js";
import type { KnobStylePreset, LabelPosition, LayoutControl, LayoutRectangleOverlay, LayoutTextLabel } from "../layoutTypes.js";
import { getLayoutImageUrl } from "./images.js";
import type { LayoutPropertiesHost } from "./propertiesHost.js";

export class ElementProperties {
  constructor(private readonly host: LayoutPropertiesHost) {}

  renderControlProperties(paramKey: string): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    const paramDefs = this.host.getParamDefs();
    const resourceCandidates = this.host.getResourceCandidates();
    if (!sidebarContent || !layout) return;

    const control = layout.controls.find((c) => c.paramKey === paramKey);
    if (!control) return;

    const isResourceControl = this.host.isResourceControl(control);
    const paramDef = isResourceControl ? undefined : paramDefs.find((p) => p.key === paramKey);
    const resourceDef = isResourceControl
      ? resourceCandidates.find((candidate) => candidate.controlKey === paramKey)
      : undefined;

    const bindingLabel = isResourceControl
      ? `${resourceDef?.displayName || paramKey} (${resourceDef?.resourceType || "resource"})`
      : (paramDef?.name || paramKey);

    const typeOptions = isResourceControl
      ? `<option value="dropdown" selected>Dropdown</option>`
      : `
              <option value="knob" ${control.type === "knob" ? "selected" : ""}>Knob</option>
              <option value="toggle" ${control.type === "toggle" ? "selected" : ""}>Toggle</option>
              <option value="slider" ${control.type === "slider" ? "selected" : ""}>Slider</option>
            `;

    const styleSection = isResourceControl
      ? ""
      : `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Style</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Knob Style</span>
          <div class="layout-property-input">
            <select id="prop-knob-style">
              <option value="default" ${control.style?.knobStyle === "default" ? "selected" : ""}>Default</option>
              <option value="pedal" ${control.style?.knobStyle === "pedal" ? "selected" : ""}>Pedal</option>
              <option value="amp" ${control.style?.knobStyle === "amp" ? "selected" : ""}>Amp</option>
              <option value="minimal" ${control.style?.knobStyle === "minimal" ? "selected" : ""}>Minimal</option>
              <option value="custom" ${control.style?.knobStyle === "custom" ? "selected" : ""}>Custom Image</option>
            </select>
          </div>
        </div>
        ${control.style?.knobStyle === "custom" ? `
        <div class="layout-image-preview">
          ${control.style?.knobImageId ? `<img src="${getLayoutImageUrl(control.style.knobImageId) || ""}" alt="Knob">` : `<span class="layout-image-preview-placeholder">No image</span>`}
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Use Existing</span>
          <div class="layout-property-input">
            <select id="prop-knob-image-select">
              ${this.host.renderImageOptionsHtml("knob", control.style?.knobImageId)}
            </select>
          </div>
        </div>
        <div class="layout-image-actions">
          <button id="prop-browse-knob-image">Browse...</button>
          ${control.style?.knobImageId ? `<button id="prop-clear-knob-image">Clear</button>` : ""}
        </div>
        ` : ""}
        <div class="layout-property-row">
          <span class="layout-property-label">Show Value</span>
          <div class="layout-property-input">
            <input type="checkbox" id="prop-show-value" ${control.style?.showValue !== false ? "checked" : ""}>
          </div>
        </div>
      </div>
      `;

    sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Control</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Binding</span>
          <span class="layout-property-input">${bindingLabel}</span>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Type</span>
          <div class="layout-property-input">
            <select id="prop-control-type">
              ${typeOptions}
            </select>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Position</div>
        <div class="layout-position-inputs">
          <label>X <input type="number" id="prop-pos-x" value="${control.position.x}" step="8"></label>
          <label>Y <input type="number" id="prop-pos-y" value="${control.position.y}" step="8"></label>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Label</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Hide Label</span>
          <div class="layout-property-input">
            <input type="checkbox" id="prop-hide-label" ${control.style?.hideLabel ? "checked" : ""}>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Override</span>
          <div class="layout-property-input">
            <input type="text" id="prop-label-override" value="${control.labelOverride || ""}" placeholder="${bindingLabel}">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Position</span>
          <div class="layout-property-input">
            <select id="prop-label-position">
              <option value="top" ${control.style?.labelPosition === "top" ? "selected" : ""}>Top</option>
              <option value="bottom" ${control.style?.labelPosition === "bottom" ? "selected" : ""}>Bottom</option>
              <option value="left" ${control.style?.labelPosition === "left" ? "selected" : ""}>Left</option>
              <option value="right" ${control.style?.labelPosition === "right" ? "selected" : ""}>Right</option>
              <option value="none" ${control.style?.labelPosition === "none" ? "selected" : ""}>Hidden</option>
            </select>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Color</span>
          <div class="layout-property-input">
            <input type="color" id="prop-label-color" value="${control.style?.labelColor || "#ffffff"}">
          </div>
        </div>
      </div>

      ${styleSection}

      <div class="layout-property-group">
        <button id="prop-delete-control" style="width: 100%; background: rgba(255,100,100,0.2); color: #ff6b6b;">Remove from Layout</button>
      </div>
    `;

    // Bind property change handlers
    this.bindControlPropertyHandlers(control);
  }

  private bindControlPropertyHandlers(control: LayoutControl): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    sidebarContent?.addEventListener("input", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });
    sidebarContent?.addEventListener("change", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });

    const typeSelect = document.getElementById("prop-control-type") as HTMLSelectElement;
    const posXInput = document.getElementById("prop-pos-x") as HTMLInputElement;
    const posYInput = document.getElementById("prop-pos-y") as HTMLInputElement;
    const hideLabelCheck = document.getElementById("prop-hide-label") as HTMLInputElement;
    const labelOverrideInput = document.getElementById("prop-label-override") as HTMLInputElement;
    const labelPosSelect = document.getElementById("prop-label-position") as HTMLSelectElement;
    const labelColorInput = document.getElementById("prop-label-color") as HTMLInputElement;
    const knobStyleSelect = document.getElementById("prop-knob-style") as HTMLSelectElement;
    const showValueCheck = document.getElementById("prop-show-value") as HTMLInputElement;
    const deleteBtn = document.getElementById("prop-delete-control") as HTMLButtonElement;
    const browseKnobBtn = document.getElementById("prop-browse-knob-image") as HTMLButtonElement;
    const clearKnobBtn = document.getElementById("prop-clear-knob-image") as HTMLButtonElement;
    const knobImageSelect = document.getElementById("prop-knob-image-select") as HTMLSelectElement;
    const isResourceControl = this.host.isResourceControl(control);

    typeSelect?.addEventListener("change", () => {
      control.type = typeSelect.value as "knob" | "toggle" | "slider" | "dropdown";
      this.host.renderCanvas();
    });

    posXInput?.addEventListener("change", () => {
      control.position.x = snapToGrid(parseInt(posXInput.value) || 0);
      posXInput.value = String(control.position.x);
      this.host.renderCanvas();
    });

    posYInput?.addEventListener("change", () => {
      control.position.y = snapToGrid(parseInt(posYInput.value) || 0);
      posYInput.value = String(control.position.y);
      this.host.renderCanvas();
    });

    hideLabelCheck?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.hideLabel = hideLabelCheck.checked;
      this.host.renderCanvas();
    });

    labelOverrideInput?.addEventListener("change", () => {
      control.labelOverride = labelOverrideInput.value.trim() || undefined;
      this.host.renderCanvas();
    });

    labelPosSelect?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.labelPosition = labelPosSelect.value as LabelPosition;
      this.host.renderCanvas();
    });

    labelColorInput?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.labelColor = labelColorInput.value;
      this.host.renderCanvas();
    });

    knobStyleSelect?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.knobStyle = knobStyleSelect.value as KnobStylePreset;
      this.host.renderCanvas();
      this.host.renderSidebar(); // Re-render to show/hide custom image picker
    });

    showValueCheck?.addEventListener("change", () => {
      if (!control.style) control.style = {};
      control.style.showValue = showValueCheck.checked;
      this.host.renderCanvas();
    });

    deleteBtn?.addEventListener("click", () => {
      if (!layout) return;
      layout.controls = layout.controls.filter((c) => c.paramKey !== control.paramKey);
      this.host.selectElement(null);
      this.host.renderCanvas();
    });

    browseKnobBtn?.addEventListener("click", () => {
      this.host.browseKnobImage(control);
    });

    clearKnobBtn?.addEventListener("click", () => {
      if (control.style) {
        control.style.knobImageId = undefined;
        this.host.renderCanvas();
        this.host.renderSidebar();
      }
    });

    knobImageSelect?.addEventListener("change", () => {
      const imageId = knobImageSelect.value;
      if (!imageId) return;
      if (!control.style) control.style = {};
      control.style.knobStyle = "custom";
      control.style.knobImageId = imageId;
      this.host.renderCanvas();
      this.host.renderSidebar();
    });

    if (isResourceControl) {
      control.type = "dropdown";
    }
  }

  renderLabelProperties(labelId: string): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    if (!sidebarContent || !layout) return;

    const label = layout.textLabels.find((l) => l.id === labelId);
    if (!label) return;

    sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Text Label</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Text</span>
          <div class="layout-property-input">
            <input type="text" id="prop-label-text" value="${label.text}">
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Position</div>
        <div class="layout-position-inputs">
          <label>X <input type="number" id="prop-label-pos-x" value="${label.position.x}" step="8"></label>
          <label>Y <input type="number" id="prop-label-pos-y" value="${label.position.y}" step="8"></label>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Style</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Font</span>
          <div class="layout-property-input">
            <select id="prop-label-font">
              <option value="" ${!label.fontFamily ? "selected" : ""}>Default</option>
              <option value="Arial, sans-serif" ${label.fontFamily === "Arial, sans-serif" ? "selected" : ""}>Arial</option>
              <option value="'Helvetica Neue', Helvetica, sans-serif" ${label.fontFamily?.includes("Helvetica") ? "selected" : ""}>Helvetica</option>
              <option value="'Segoe UI', Tahoma, sans-serif" ${label.fontFamily?.includes("Segoe") ? "selected" : ""}>Segoe UI</option>
              <option value="Georgia, serif" ${label.fontFamily?.includes("Georgia") ? "selected" : ""}>Georgia</option>
              <option value="'Times New Roman', Times, serif" ${label.fontFamily?.includes("Times") ? "selected" : ""}>Times New Roman</option>
              <option value="'Courier New', Courier, monospace" ${label.fontFamily?.includes("Courier") ? "selected" : ""}>Courier New</option>
              <option value="Impact, sans-serif" ${label.fontFamily?.includes("Impact") ? "selected" : ""}>Impact</option>
              <option value="'Comic Sans MS', cursive" ${label.fontFamily?.includes("Comic") ? "selected" : ""}>Comic Sans</option>
            </select>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Font Size</span>
          <div class="layout-property-input">
            <input type="number" id="prop-label-font-size" value="${label.fontSize}" min="8" max="48">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Weight</span>
          <div class="layout-property-input">
            <select id="prop-label-weight">
              <option value="normal" ${label.fontWeight !== "bold" ? "selected" : ""}>Normal</option>
              <option value="bold" ${label.fontWeight === "bold" ? "selected" : ""}>Bold</option>
            </select>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Color</span>
          <div class="layout-property-input">
            <input type="color" id="prop-label-color" value="${label.color || "#ffffff"}">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Align</span>
          <div class="layout-property-input">
            <select id="prop-label-align">
              <option value="left" ${label.textAlign !== "center" && label.textAlign !== "right" ? "selected" : ""}>Left</option>
              <option value="center" ${label.textAlign === "center" ? "selected" : ""}>Center</option>
              <option value="right" ${label.textAlign === "right" ? "selected" : ""}>Right</option>
            </select>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <button id="prop-delete-label" style="width: 100%; background: rgba(255,100,100,0.2); color: #ff6b6b;">Delete Label</button>
      </div>
    `;

    // Bind property handlers
    this.bindLabelPropertyHandlers(label);
  }

  private bindLabelPropertyHandlers(label: LayoutTextLabel): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    sidebarContent?.addEventListener("input", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });
    sidebarContent?.addEventListener("change", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });

    const textInput = document.getElementById("prop-label-text") as HTMLInputElement;
    const posXInput = document.getElementById("prop-label-pos-x") as HTMLInputElement;
    const posYInput = document.getElementById("prop-label-pos-y") as HTMLInputElement;
    const fontSelect = document.getElementById("prop-label-font") as HTMLSelectElement;
    const fontSizeInput = document.getElementById("prop-label-font-size") as HTMLInputElement;
    const weightSelect = document.getElementById("prop-label-weight") as HTMLSelectElement;
    const colorInput = document.getElementById("prop-label-color") as HTMLInputElement;
    const alignSelect = document.getElementById("prop-label-align") as HTMLSelectElement;
    const deleteBtn = document.getElementById("prop-delete-label") as HTMLButtonElement;

    textInput?.addEventListener("change", () => {
      label.text = textInput.value || "Label";
      this.host.renderCanvas();
    });

    posXInput?.addEventListener("change", () => {
      label.position.x = snapToGrid(parseInt(posXInput.value) || 0);
      posXInput.value = String(label.position.x);
      this.host.renderCanvas();
    });

    posYInput?.addEventListener("change", () => {
      label.position.y = snapToGrid(parseInt(posYInput.value) || 0);
      posYInput.value = String(label.position.y);
      this.host.renderCanvas();
    });

    fontSelect?.addEventListener("change", () => {
      label.fontFamily = fontSelect.value || undefined;
      this.host.renderCanvas();
    });

    fontSizeInput?.addEventListener("change", () => {
      label.fontSize = Math.max(8, Math.min(48, parseInt(fontSizeInput.value) || 12));
      this.host.renderCanvas();
    });

    weightSelect?.addEventListener("change", () => {
      label.fontWeight = weightSelect.value as "normal" | "bold";
      this.host.renderCanvas();
    });

    colorInput?.addEventListener("change", () => {
      label.color = colorInput.value;
      this.host.renderCanvas();
    });

    alignSelect?.addEventListener("change", () => {
      label.textAlign = alignSelect.value as "left" | "center" | "right";
      this.host.renderCanvas();
    });

    deleteBtn?.addEventListener("click", () => {
      if (!layout) return;
      layout.textLabels = layout.textLabels.filter((l) => l.id !== label.id);
      this.host.selectElement(null);
      this.host.renderCanvas();
    });
  }

  renderOverlayProperties(overlayId: string): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    if (!sidebarContent || !layout) return;

    const overlay = (layout.overlays ?? []).find((item) => item.id === overlayId);
    if (!overlay) return;

    const style = overlay.style ?? {};
    const visibilityMode = style.visibilityMode ?? "always";
    const toggleBypassOnClick = style.toggleBypassOnClick === true;
    const backgroundColor = style.backgroundColor || "#000000";
    const backgroundOpacity = Math.round(((typeof style.backgroundOpacity === "number" ? style.backgroundOpacity : 0.25) * 100));
    const borderColor = style.borderColor || "#ffffff";
    const borderWidth = style.borderWidth ?? 1;
    const borderRadius = style.borderRadius ?? 0;

    sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Rectangle Overlay</div>
        ${toggleBypassOnClick ? `<div class="layout-overlay-sidebar-badge">Power Indicator</div>` : ""}
        <div class="layout-property-row">
          <span class="layout-property-label">Visible</span>
          <div class="layout-property-input">
            <select id="prop-overlay-visibility-mode">
              <option value="always" ${visibilityMode === "always" ? "selected" : ""}>Always</option>
              <option value="enabled" ${visibilityMode === "enabled" ? "selected" : ""}>When Enabled</option>
              <option value="bypassed" ${visibilityMode === "bypassed" ? "selected" : ""}>When Bypassed</option>
            </select>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">On Click</span>
          <div class="layout-property-input">
            <label style="display:flex;align-items:center;gap:6px;font-size:11px;color:var(--text-dark-secondary);">
              <input type="checkbox" id="prop-overlay-toggle-bypass" ${toggleBypassOnClick ? "checked" : ""}>
              Toggle Bypass
            </label>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Position</div>
        <div class="layout-position-inputs">
          <label>X <input type="number" id="prop-overlay-x" value="${overlay.position.x}" step="8"></label>
          <label>Y <input type="number" id="prop-overlay-y" value="${overlay.position.y}" step="8"></label>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Size</div>
        <div class="layout-position-inputs">
          <label>W <input type="number" id="prop-overlay-width" value="${overlay.size.width}" min="16" step="8"></label>
          <label>H <input type="number" id="prop-overlay-height" value="${overlay.size.height}" min="16" step="8"></label>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Appearance</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Fill</span>
          <div class="layout-property-input">
            <input type="color" id="prop-overlay-bg-color" value="${backgroundColor}">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Fill Opacity</span>
          <div class="layout-property-input">
            <input type="range" id="prop-overlay-bg-opacity" min="0" max="100" value="${backgroundOpacity}" style="width: 80px;">
            <span id="prop-overlay-bg-opacity-value">${backgroundOpacity}%</span>
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Border</span>
          <div class="layout-property-input">
            <input type="color" id="prop-overlay-border-color" value="${borderColor}">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Border W</span>
          <div class="layout-property-input">
            <input type="number" id="prop-overlay-border-width" value="${borderWidth}" min="0" max="24" step="1">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Radius</span>
          <div class="layout-property-input">
            <input type="number" id="prop-overlay-border-radius" value="${borderRadius}" min="0" max="64" step="1">
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <button id="prop-delete-overlay" style="width: 100%; background: rgba(255,100,100,0.2); color: #ff6b6b;">Delete Rectangle</button>
      </div>
    `;

    this.bindOverlayPropertyHandlers(overlay);
  }

  private bindOverlayPropertyHandlers(overlay: LayoutRectangleOverlay): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    sidebarContent?.addEventListener("input", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });
    sidebarContent?.addEventListener("change", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });

    const xInput = document.getElementById("prop-overlay-x") as HTMLInputElement;
    const yInput = document.getElementById("prop-overlay-y") as HTMLInputElement;
    const visibilityModeSelect = document.getElementById("prop-overlay-visibility-mode") as HTMLSelectElement;
    const toggleBypassCheck = document.getElementById("prop-overlay-toggle-bypass") as HTMLInputElement;
    const widthInput = document.getElementById("prop-overlay-width") as HTMLInputElement;
    const heightInput = document.getElementById("prop-overlay-height") as HTMLInputElement;
    const bgColorInput = document.getElementById("prop-overlay-bg-color") as HTMLInputElement;
    const bgOpacityInput = document.getElementById("prop-overlay-bg-opacity") as HTMLInputElement;
    const bgOpacityValue = document.getElementById("prop-overlay-bg-opacity-value") as HTMLElement;
    const borderColorInput = document.getElementById("prop-overlay-border-color") as HTMLInputElement;
    const borderWidthInput = document.getElementById("prop-overlay-border-width") as HTMLInputElement;
    const borderRadiusInput = document.getElementById("prop-overlay-border-radius") as HTMLInputElement;
    const deleteBtn = document.getElementById("prop-delete-overlay") as HTMLButtonElement;

    xInput?.addEventListener("change", () => {
      overlay.position.x = snapToGrid(parseInt(xInput.value) || 0);
      xInput.value = String(overlay.position.x);
      this.host.renderCanvas();
    });

    visibilityModeSelect?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.visibilityMode = visibilityModeSelect.value as "always" | "enabled" | "bypassed";
      this.host.renderCanvas();
    });

    toggleBypassCheck?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.toggleBypassOnClick = toggleBypassCheck.checked;
      this.host.renderCanvas();
    });

    yInput?.addEventListener("change", () => {
      overlay.position.y = snapToGrid(parseInt(yInput.value) || 0);
      yInput.value = String(overlay.position.y);
      this.host.renderCanvas();
    });

    widthInput?.addEventListener("change", () => {
      overlay.size.width = Math.max(16, snapToGrid(parseInt(widthInput.value) || 16));
      widthInput.value = String(overlay.size.width);
      this.host.renderCanvas();
    });

    heightInput?.addEventListener("change", () => {
      overlay.size.height = Math.max(16, snapToGrid(parseInt(heightInput.value) || 16));
      heightInput.value = String(overlay.size.height);
      this.host.renderCanvas();
    });

    bgColorInput?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.backgroundColor = bgColorInput.value;
      this.host.renderCanvas();
    });

    bgOpacityInput?.addEventListener("input", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.backgroundOpacity = (parseInt(bgOpacityInput.value) || 0) / 100;
      if (bgOpacityValue) bgOpacityValue.textContent = `${bgOpacityInput.value}%`;
      this.host.renderCanvas();
    });

    borderColorInput?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.borderColor = borderColorInput.value;
      this.host.renderCanvas();
    });

    borderWidthInput?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.borderWidth = Math.max(0, parseInt(borderWidthInput.value) || 0);
      this.host.renderCanvas();
    });

    borderRadiusInput?.addEventListener("change", () => {
      if (!overlay.style) overlay.style = {};
      overlay.style.borderRadius = Math.max(0, parseInt(borderRadiusInput.value) || 0);
      this.host.renderCanvas();
    });

    deleteBtn?.addEventListener("click", () => {
      if (!layout?.overlays) return;
      layout.overlays = layout.overlays.filter((item) => item.id !== overlay.id);
      this.host.selectElement(null);
      this.host.renderCanvas();
    });
  }
}
