/**
 * The properties panel for the canvas itself and for its background layers.
 *
 * Background layers are ordered, so most of what is here is reordering and
 * per-layer blend settings rather than a flat form.
 */

import type { LayoutBackground } from "../layoutTypes.js";
import { getLayoutImageUrl } from "./images.js";
import type { LayoutPropertiesHost } from "./propertiesHost.js";

export class CanvasProperties {
  constructor(private readonly host: LayoutPropertiesHost) {}

  renderCanvasProperties(): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    if (!sidebarContent || !layout) return;

    const containerTheme = layout.containerTheme ?? '';
    const isBackdrop = layout.useDefaultControls === true;
    const offsetX = layout.defaultControlsOffset?.x ?? 0;
    const offsetY = layout.defaultControlsOffset?.y ?? 0;
    const scaleX = layout.defaultControlsScale?.x ?? 1;
    const scaleY = layout.defaultControlsScale?.y ?? 1;

    const backdropSections = isBackdrop ? `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Default Controls — Position</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Left (px)</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-offset-x" value="${offsetX}" step="1">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Top (px)</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-offset-y" value="${offsetY}" step="1">
          </div>
        </div>
      </div>
      <div class="layout-property-group">
        <div class="layout-property-group-title">Default Controls — Scale</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Scale X</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-scale-x" value="${scaleX}" min="0.1" max="3" step="0.05">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Scale Y</span>
          <div class="layout-property-input">
            <input type="number" id="prop-dc-scale-y" value="${scaleY}" min="0.1" max="3" step="0.05">
          </div>
        </div>
        <div class="layout-property-row">
          <button id="prop-dc-scale-reset" style="font-size: 11px;">Reset to 1:1</button>
        </div>
      </div>
    ` : `
      <div class="layout-designer-sidebar-empty" style="font-size:11px; padding: 6px 0 0;">
        Select a control, label, background, or rectangle to edit its properties.
      </div>
    `;

    sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Container</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Theme</span>
          <div class="layout-property-input">
            <select id="prop-container-theme" title="Override CSS colour variables inside this container. Useful when the layout background differs from the global app theme.">
              <option value="" ${containerTheme === '' ? 'selected' : ''}>Inherit (app theme)</option>
              <option value="dark" ${containerTheme === 'dark' ? 'selected' : ''}>Dark</option>
              <option value="light" ${containerTheme === 'light' ? 'selected' : ''}>Light</option>
              <option value="classic" ${containerTheme === 'classic' ? 'selected' : ''}>Classic</option>
            </select>
          </div>
        </div>
      </div>
      ${backdropSections}
    `;

    // Container theme selector
    const themeSelect = document.getElementById("prop-container-theme") as HTMLSelectElement | null;
    themeSelect?.addEventListener("change", () => {
      if (!layout) return;
      this.host.pushSidebarUndoOnce();
      const val = themeSelect.value as 'light' | 'dark' | 'classic' | '';
      layout.containerTheme = val === '' ? undefined : val;
      this.host.renderCanvas();
    });

    if (!isBackdrop) return;

    // Backdrop offset/scale bindings
    const bindNum = (id: string, apply: (v: number) => void) => {
      const el = document.getElementById(id) as HTMLInputElement | null;
      el?.addEventListener("change", () => {
        const v = parseFloat(el.value);
        if (!isNaN(v)) {
          this.host.pushSidebarUndoOnce();
          apply(v);
          this.host.renderCanvas();
        }
      });
    };

    bindNum("prop-dc-offset-x", (v) => {
      if (!layout) return;
      layout.defaultControlsOffset = { x: Math.round(v), y: layout.defaultControlsOffset?.y ?? 0 };
    });
    bindNum("prop-dc-offset-y", (v) => {
      if (!layout) return;
      layout.defaultControlsOffset = { x: layout.defaultControlsOffset?.x ?? 0, y: Math.round(v) };
    });
    bindNum("prop-dc-scale-x", (v) => {
      if (!layout) return;
      layout.defaultControlsScale = { x: Math.max(0.1, v), y: layout.defaultControlsScale?.y ?? 1 };
    });
    bindNum("prop-dc-scale-y", (v) => {
      if (!layout) return;
      layout.defaultControlsScale = { x: layout.defaultControlsScale?.x ?? 1, y: Math.max(0.1, v) };
    });

    document.getElementById("prop-dc-scale-reset")?.addEventListener("click", () => {
      if (!layout) return;
      this.host.pushUndoState();
      layout.defaultControlsScale = { x: 1, y: 1 };
      this.host.renderCanvas();
      this.host.renderSidebar();
    });
  }

  renderBackgroundProperties(layerIndex: number): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    if (!sidebarContent || !layout) return;

    const bg = layout.backgrounds.find((b) => b.layerIndex === layerIndex);
    if (!bg) return;

    const isCustomScale = bg.size === "custom";
    const isImage = bg.type === "image";

    // Type-specific value editor
    let valueEditor = "";
    if (bg.type === "color") {
      valueEditor = `
        <div class="layout-property-row">
          <span class="layout-property-label">Color</span>
          <div class="layout-property-input">
            <input type="color" id="prop-bg-color" value="${bg.value || "#1a1a2e"}">
          </div>
        </div>
      `;
    } else if (bg.type === "gradient") {
      valueEditor = `
        <div class="layout-property-row">
          <span class="layout-property-label">Gradient</span>
          <div class="layout-property-input">
            <input type="text" id="prop-bg-gradient" value="${bg.value}" placeholder="linear-gradient(...)">
          </div>
        </div>
      `;
    }

    // Type selector
    const typeSelector = bg.type !== "image" ? `
      <div class="layout-property-row">
        <span class="layout-property-label">Type</span>
        <div class="layout-property-input">
          <select id="prop-bg-type">
            <option value="color" ${bg.type === "color" ? "selected" : ""}>Solid Color</option>
            <option value="gradient" ${bg.type === "gradient" ? "selected" : ""}>Gradient</option>
          </select>
        </div>
      </div>
    ` : `
      <div class="layout-property-row">
        <span class="layout-property-label">Type</span>
        <span class="layout-property-input">Image</span>
      </div>
    `;

    sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Background Layer ${layerIndex + 1}</div>
        ${typeSelector}
        ${valueEditor}
      </div>

      ${isImage ? `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Image Source</div>
        <div class="layout-image-preview">
          ${bg.value ? `<img src="${getLayoutImageUrl(bg.value) || ""}" alt="Background">` : `<span class="layout-image-preview-placeholder">No image</span>`}
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Use Existing</span>
          <div class="layout-property-input">
            <select id="prop-bg-image-select">
              ${this.host.renderImageOptionsHtml("background", bg.value)}
            </select>
          </div>
        </div>
        <div class="layout-image-actions">
          <button id="prop-browse-bg-image">Browse...</button>
        </div>
      </div>

      <div class="layout-property-group">
        <div class="layout-property-group-title">Size & Position</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Size Mode</span>
          <div class="layout-property-input">
            <select id="prop-bg-size">
              <option value="cover" ${bg.size === "cover" ? "selected" : ""}>Cover</option>
              <option value="contain" ${bg.size === "contain" || !bg.size ? "selected" : ""}>Contain</option>
              <option value="stretch" ${bg.size === "stretch" ? "selected" : ""}>Stretch</option>
              <option value="tile" ${bg.size === "tile" ? "selected" : ""}>Tile</option>
              <option value="custom" ${bg.size === "custom" ? "selected" : ""}>Custom Scale</option>
            </select>
          </div>
        </div>
        ${isCustomScale ? `
        <div class="layout-property-row">
          <span class="layout-property-label">Scale</span>
          <div class="layout-property-input">
            <input type="range" id="prop-bg-scale" min="10" max="300" value="${(bg.scale || 1) * 100}" style="width: 80px;">
            <span id="prop-bg-scale-value">${Math.round((bg.scale || 1) * 100)}%</span>
          </div>
        </div>
        ` : ""}
        <div class="layout-property-row">
          <span class="layout-property-label">Offset X</span>
          <div class="layout-property-input">
            <input type="number" id="prop-bg-offset-x" value="${bg.offsetX || 0}" step="8">
          </div>
        </div>
        <div class="layout-property-row">
          <span class="layout-property-label">Offset Y</span>
          <div class="layout-property-input">
            <input type="number" id="prop-bg-offset-y" value="${bg.offsetY || 0}" step="8">
          </div>
        </div>
      </div>
      ` : ""}

      <div class="layout-property-group">
        <div class="layout-property-group-title">Appearance</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Opacity</span>
          <div class="layout-property-input">
            <input type="range" id="prop-bg-opacity" min="0" max="100" value="${(bg.opacity ?? 1) * 100}" style="width: 80px;">
            <span id="prop-bg-opacity-value">${Math.round((bg.opacity ?? 1) * 100)}%</span>
          </div>
        </div>
      </div>

      <div class="layout-property-group">
        <button id="prop-delete-bg" style="width: 100%; background: rgba(255,100,100,0.2); color: #ff6b6b;">Remove Background</button>
      </div>
    `;

    this.bindBackgroundPropertyHandlers(bg);
  }

  private bindBackgroundPropertyHandlers(bg: LayoutBackground): void {
    const layout = this.host.getLayout();
    const sidebarContent = this.host.getSidebarContent();
    // Push undo once on first input/change in this sidebar session
    sidebarContent?.addEventListener("input", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });
    sidebarContent?.addEventListener("change", () => this.host.pushSidebarUndoOnce(), { once: true, capture: true });

    const typeSelect = document.getElementById("prop-bg-type") as HTMLSelectElement;
    const colorInput = document.getElementById("prop-bg-color") as HTMLInputElement;
    const gradientInput = document.getElementById("prop-bg-gradient") as HTMLInputElement;
    const sizeSelect = document.getElementById("prop-bg-size") as HTMLSelectElement;
    const scaleInput = document.getElementById("prop-bg-scale") as HTMLInputElement;
    const scaleValue = document.getElementById("prop-bg-scale-value") as HTMLElement;
    const offsetXInput = document.getElementById("prop-bg-offset-x") as HTMLInputElement;
    const offsetYInput = document.getElementById("prop-bg-offset-y") as HTMLInputElement;
    const opacityInput = document.getElementById("prop-bg-opacity") as HTMLInputElement;
    const opacityValue = document.getElementById("prop-bg-opacity-value") as HTMLElement;
    const deleteBtn = document.getElementById("prop-delete-bg") as HTMLButtonElement;
    const bgImageSelect = document.getElementById("prop-bg-image-select") as HTMLSelectElement;
    const browseBgBtn = document.getElementById("prop-browse-bg-image") as HTMLButtonElement;

    typeSelect?.addEventListener("change", () => {
      const newType = typeSelect.value as "color" | "gradient";
      bg.type = newType;
      if (newType === "color") {
        bg.value = "#1a1a2e";
      } else if (newType === "gradient") {
        bg.value = "linear-gradient(180deg, #2a2a3a 0%, #1a1a2e 100%)";
      }
      this.host.renderCanvas();
      this.host.renderSidebar();
    });

    colorInput?.addEventListener("input", () => {
      bg.value = colorInput.value;
      this.host.renderCanvas();
    });

    gradientInput?.addEventListener("change", () => {
      bg.value = gradientInput.value;
      this.host.renderCanvas();
    });

    sizeSelect?.addEventListener("change", () => {
      bg.size = sizeSelect.value as "cover" | "contain" | "stretch" | "tile" | "custom";
      if (bg.size === "custom" && bg.scale === undefined) {
        bg.scale = 1;
      }
      this.host.renderCanvas();
      this.host.renderSidebar(); // Re-render to show/hide scale slider
    });

    scaleInput?.addEventListener("input", () => {
      bg.scale = parseInt(scaleInput.value) / 100;
      if (scaleValue) scaleValue.textContent = `${scaleInput.value}%`;
      this.host.renderCanvas();
    });

    offsetXInput?.addEventListener("change", () => {
      bg.offsetX = parseInt(offsetXInput.value) || 0;
      this.host.renderCanvas();
    });

    offsetYInput?.addEventListener("change", () => {
      bg.offsetY = parseInt(offsetYInput.value) || 0;
      this.host.renderCanvas();
    });

    opacityInput?.addEventListener("input", () => {
      bg.opacity = parseInt(opacityInput.value) / 100;
      if (opacityValue) opacityValue.textContent = `${opacityInput.value}%`;
      this.host.renderCanvas();
    });

    bgImageSelect?.addEventListener("change", () => {
      const imageId = bgImageSelect.value;
      if (!imageId) return;
      bg.type = "image";
      bg.value = imageId;
      this.host.renderCanvas();
      this.host.renderSidebar();
    });

    browseBgBtn?.addEventListener("click", () => {
      this.host.browseBackgroundImage(bg.layerIndex);
    });

    deleteBtn?.addEventListener("click", () => {
      if (!layout) return;
      layout.backgrounds = layout.backgrounds.filter((b) => b.layerIndex !== bg.layerIndex);
      this.host.selectElement(null);
      this.host.renderCanvas();
    });
  }
}
