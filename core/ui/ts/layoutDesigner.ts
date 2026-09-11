/**
 * Layout Designer — the editor for a custom effect layout.
 *
 * The panels and interactions it is made of live in ./layoutDesigner/; this file
 * owns the modal itself, the canvas it draws onto, and the undo stack behind both.
 */

import { postMessage } from "./bridge.js";
import { CanvasProperties } from "./layoutDesigner/canvasProperties.js";
import { LayoutStore } from "./layoutDesigner/layoutStore.js";
import { ElementProperties } from "./layoutDesigner/elementProperties.js";
import { getLayoutImageUrl } from "./layoutDesigner/images.js";
import { showConfirm } from "./dialogs.js";
import type { CopiedTextLabelPayload, DragState, LayoutResourceCandidate, SelectedElement } from "./layoutDesigner/types.js";
import { ensureLayoutImagesLoaded } from "./layoutImages.js";
import { colorWithAlpha, renderCustomLayoutPreviewLayers } from "./layoutRenderer.js";
import type { LayoutResourceControlDef } from "./layoutRenderer.js";
import { DEFAULT_LAYOUT_DIMENSIONS, LAYOUT_DIMENSION_LIMITS, LAYOUT_GRID_SIZE, generateLabelId, generateLayoutId, generateOverlayId, layoutLookupKey, snapToGrid } from "./layoutTypes.js";
import type { EffectLayout, LayoutBackground, LayoutControl, LayoutImageRef, LayoutLibraryEntry, LayoutRectangleOverlay, LayoutTextLabel } from "./layoutTypes.js";
import { showNotification } from "./notifications.js";
import { EffectTypeRegistry } from "./presetV2.js";
import type { ParameterDef } from "./presetV2.js";
import { buildDefaultParamControlsHtml } from "./signalPath.js";
import { uiState } from "./state.js";
import type { GraphNode } from "./types.js";

export class LayoutDesignerModal {
  /**
   * The canvas and background properties panels, handed an adapter of closures
   * so the members they read stay private to this class.
   */
  /** Saving, deleting, exporting and importing the layout being edited. */
  private readonly store = new LayoutStore({
    getLayout: () => this.layout,
    setLayout: (layout) => {
      this.layout = layout;
    },
    getEffectType: () => this.effectType,
    getBlendId: () => this.blendId,
    getFallbackLayoutName: () => this.fallbackLayoutName,
    isFactoryLayout: () => this.isFactoryLayout,
    setFactoryLayout: (value) => {
      this.isFactoryLayout = value;
    },
    isNewLayout: () => this.isNewLayout,
    setNewLayout: (value) => {
      this.isNewLayout = value;
    },
    getParamDefs: () => this.paramDefs,
    applyLayoutName: (pushUndo) => this.applyLayoutName(pushUndo),
    collectReferencedImageIds: () => this.collectReferencedImageIds(),
    updateDeleteButtonVisibility: () => this.updateDeleteButtonVisibility(),
    takeImportFile: () => {
      const file = this.importFileInput?.files?.[0] ?? null;
      if (this.importFileInput) this.importFileInput.value = "";
      return file;
    },
    closeModal: (didSave) => this.close(didSave),
    notifySaved: (layout) => this.onSaveCallback?.(layout),
    pushUndoState: () => this.pushUndoState(),
    updateDimensionInputs: () => this.updateDimensionInputs(),
    renderCanvas: () => this.renderCanvas(),
    selectElement: (element) => this.selectElement(element),
  });

  private readonly canvasProperties = new CanvasProperties({
    getLayout: () => this.layout,
    getParamDefs: () => this.paramDefs,
    getResourceCandidates: () => this.resourceCandidates,
    getSidebarContent: () => this.sidebarContent,
    pushUndoState: () => this.pushUndoState(),
    pushSidebarUndoOnce: () => this.pushSidebarUndoOnce(),
    renderCanvas: () => this.renderCanvas(),
    renderSidebar: () => this.renderSidebar(),
    selectElement: (element) => this.selectElement(element),
    renderImageOptionsHtml: (purpose, selectedImageId) => this.renderImageOptionsHtml(purpose, selectedImageId),
    browseBackgroundImage: (layerIndex) => this.browseBackgroundImage(layerIndex),
    browseKnobImage: (control) => this.browseKnobImage(control),
    isResourceControl: (control) => this.isResourceControl(control),
  });

  /** The properties panels for a control, a text label, or a rectangle overlay. */
  private readonly elementProperties = new ElementProperties({
    getLayout: () => this.layout,
    getParamDefs: () => this.paramDefs,
    getResourceCandidates: () => this.resourceCandidates,
    getSidebarContent: () => this.sidebarContent,
    pushUndoState: () => this.pushUndoState(),
    pushSidebarUndoOnce: () => this.pushSidebarUndoOnce(),
    renderCanvas: () => this.renderCanvas(),
    renderSidebar: () => this.renderSidebar(),
    selectElement: (element) => this.selectElement(element),
    renderImageOptionsHtml: (purpose, selectedImageId) => this.renderImageOptionsHtml(purpose, selectedImageId),
    browseBackgroundImage: (layerIndex) => this.browseBackgroundImage(layerIndex),
    browseKnobImage: (control) => this.browseKnobImage(control),
    isResourceControl: (control) => this.isResourceControl(control),
  });

  private initialized = false;
  private effectType = "";
  private blendId = "";
  private layout: EffectLayout | null = null;
  private isFactoryLayout = false;
  private isNewLayout = false;  // true for layouts never persisted to disk yet
  private paramDefs: ParameterDef[] = [];
  private resourceCandidates: LayoutResourceCandidate[] = [];

  // Selection and drag state
  private selectedElement: SelectedElement = null;
  private dragState: DragState = {
    active: false,
    element: null,
    startX: 0,
    startY: 0,
    elementStartX: 0,
    elementStartY: 0,
    elementStartWidth: 0,
    elementStartHeight: 0,
    type: null,
    mode: "move",
    resizeHandle: null,
    id: "",
  };

  // UI options
  private gridVisible = true;
  private previewMode = false;
  private zoom = 1;

  // Undo/redo history
  private undoStack: string[] = [];
  private redoStack: string[] = [];
  private static readonly MAX_UNDO = 50;
  private copiedTextLabel: CopiedTextLabelPayload | null = null;
  /** Tracks whether undo was pushed for current sidebar edit session */
  private sidebarUndoPushed = false;
  /** Timer for nudge undo debouncing */
  private nudgeUndoTimer: ReturnType<typeof setTimeout> | null = null;

  // DOM references
  private modal: HTMLElement | null = null;
  private closeBtn: HTMLButtonElement | null = null;
  private cancelBtn: HTMLButtonElement | null = null;
  private saveBtn: HTMLButtonElement | null = null;
  private deleteBtn: HTMLButtonElement | null = null;
  private exportBtn: HTMLButtonElement | null = null;
  private importBtn: HTMLButtonElement | null = null;
  private importFileInput: HTMLInputElement | null = null;
  private canvas: HTMLElement | null = null;
  private canvasWrapper: HTMLElement | null = null;
  private grid: HTMLElement | null = null;
  private controlsLayer: HTMLElement | null = null;
  private sidebar: HTMLElement | null = null;
  private sidebarContent: HTMLElement | null = null;
  private titleEl: HTMLElement | null = null;

  // Toolbar buttons
  private gridToggleBtn: HTMLButtonElement | null = null;
  private previewToggleBtn: HTMLButtonElement | null = null;
  private addLabelBtn: HTMLButtonElement | null = null;
  private addControlBtn: HTMLButtonElement | null = null;
  private addRectBtn: HTMLButtonElement | null = null;
  private addBgBtn: HTMLButtonElement | null = null;
  private addColorBgBtn: HTMLButtonElement | null = null;
  private useDefaultControlsCheckbox: HTMLInputElement | null = null;
  private resetLayoutBtn: HTMLButtonElement | null = null;
  private undoBtn: HTMLButtonElement | null = null;
  private redoBtn: HTMLButtonElement | null = null;
  private zoomInBtn: HTMLButtonElement | null = null;
  private zoomOutBtn: HTMLButtonElement | null = null;
  private zoomResetBtn: HTMLButtonElement | null = null;
  private zoomLabel: HTMLElement | null = null;

  // Dimension inputs
  private widthInput: HTMLInputElement | null = null;
  private heightInput: HTMLInputElement | null = null;

  // Layout name
  private nameInput: HTMLInputElement | null = null;
  /** Name used when the user leaves the name field blank (effect display name). */
  private fallbackLayoutName = "";

  // Callbacks
  private onSaveCallback?: (layout: EffectLayout) => void;
  private onCloseCallback?: (didSave: boolean) => void;

  initialize(): void {
    if (this.initialized) return;
    this.initialized = true;

    this.modal = document.getElementById("layout-designer-modal");
    if (!this.modal) {
      console.warn("LayoutDesignerModal: modal element not found");
      return;
    }

    // Get DOM references
    this.titleEl = document.getElementById("layout-designer-title");
    this.closeBtn = document.getElementById("layout-designer-close") as HTMLButtonElement;
    this.cancelBtn = document.getElementById("layout-designer-cancel") as HTMLButtonElement;
    this.saveBtn = document.getElementById("layout-designer-save") as HTMLButtonElement;
    this.deleteBtn = document.getElementById("layout-designer-delete") as HTMLButtonElement;
    this.exportBtn = document.getElementById("layout-designer-export") as HTMLButtonElement;
    this.importBtn = document.getElementById("layout-designer-import") as HTMLButtonElement;
    this.importFileInput = document.getElementById("layout-designer-import-file") as HTMLInputElement;
    this.canvas = document.getElementById("layout-designer-canvas");
    this.canvasWrapper = document.getElementById("layout-designer-canvas-wrapper");
    this.grid = document.getElementById("layout-designer-grid");
    this.controlsLayer = document.getElementById("layout-designer-controls");
    this.sidebar = document.getElementById("layout-designer-sidebar");
    this.sidebarContent = document.getElementById("layout-designer-sidebar-content");

    // Toolbar buttons
    this.gridToggleBtn = document.getElementById("layout-designer-grid-toggle") as HTMLButtonElement;
    this.previewToggleBtn = document.getElementById("layout-designer-preview-toggle") as HTMLButtonElement;
    this.addLabelBtn = document.getElementById("layout-designer-add-label") as HTMLButtonElement;
    this.addControlBtn = document.getElementById("layout-designer-add-control") as HTMLButtonElement;
    this.addRectBtn = document.getElementById("layout-designer-add-rect") as HTMLButtonElement;
    this.addBgBtn = document.getElementById("layout-designer-add-bg") as HTMLButtonElement;
    this.addColorBgBtn = document.getElementById("layout-designer-add-color-bg") as HTMLButtonElement;
    this.useDefaultControlsCheckbox = document.getElementById("layout-designer-use-default-controls") as HTMLInputElement;
    this.resetLayoutBtn = document.getElementById("layout-designer-reset") as HTMLButtonElement;
    this.undoBtn = document.getElementById("layout-designer-undo") as HTMLButtonElement;
    this.redoBtn = document.getElementById("layout-designer-redo") as HTMLButtonElement;
    this.zoomInBtn = document.getElementById("layout-designer-zoom-in") as HTMLButtonElement;
    this.zoomOutBtn = document.getElementById("layout-designer-zoom-out") as HTMLButtonElement;
    this.zoomResetBtn = document.getElementById("layout-designer-zoom-reset") as HTMLButtonElement;
    this.zoomLabel = document.getElementById("layout-designer-zoom-label");

    console.log("[LayoutDesigner] initialize - addBgBtn found:", !!this.addBgBtn);

    // Dimension inputs
    this.widthInput = document.getElementById("layout-designer-width") as HTMLInputElement;
    this.heightInput = document.getElementById("layout-designer-height") as HTMLInputElement;

    // Layout name input
    this.nameInput = document.getElementById("layout-designer-name") as HTMLInputElement;

    this.bindEvents();
  }

  private bindEvents(): void {
    // Close buttons
    this.closeBtn?.addEventListener("click", () => this.close());
    this.cancelBtn?.addEventListener("click", () => this.close());
    this.saveBtn?.addEventListener("click", () => { void this.store.save(); });
    this.deleteBtn?.addEventListener("click", () => { void this.confirmDeleteCurrentLayout(); });
    this.exportBtn?.addEventListener("click", () => this.store.exportLayout());
    this.importBtn?.addEventListener("click", () => this.importFileInput?.click());
    this.importFileInput?.addEventListener("change", () => this.store.handleImportFileSelected());

    // Modal backdrop click
    this.modal?.addEventListener("mousedown", (e) => {
      if (e.target === this.modal) {
        this.close();
      }
    });

    // Toolbar buttons
    this.gridToggleBtn?.addEventListener("click", () => this.toggleGrid());
    this.previewToggleBtn?.addEventListener("click", () => this.togglePreview());
    this.addLabelBtn?.addEventListener("click", () => this.addTextLabel());
    this.addControlBtn?.addEventListener("click", () => this.showAddControlMenu());
    this.addRectBtn?.addEventListener("click", () => this.addRectangleOverlay());
    this.addBgBtn?.addEventListener("click", () => this.showAddBackgroundMenu());
    this.addColorBgBtn?.addEventListener("click", () => this.addColorBackground());
    this.useDefaultControlsCheckbox?.addEventListener("change", () => this.toggleUseDefaultControls());
    this.resetLayoutBtn?.addEventListener("click", () => this.resetLayout());
    this.undoBtn?.addEventListener("click", () => this.undo());
    this.redoBtn?.addEventListener("click", () => this.redo());
    this.zoomInBtn?.addEventListener("click", () => this.setZoom(this.zoom + 0.25));
    this.zoomOutBtn?.addEventListener("click", () => this.setZoom(this.zoom - 0.25));
    this.zoomResetBtn?.addEventListener("click", () => this.setZoom(1));

    // Dimension inputs
    this.widthInput?.addEventListener("change", () => this.updateDimensions());
    this.heightInput?.addEventListener("change", () => this.updateDimensions());

    // Layout name — committed on blur/Enter so a rename is a single undo step
    this.nameInput?.addEventListener("change", () => this.applyLayoutName(true));
    this.nameInput?.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        this.nameInput?.blur();
      }
    });

    // Canvas mouse events for drag
    this.canvas?.addEventListener("mousedown", (e) => this.onCanvasMouseDown(e));
    document.addEventListener("mousemove", (e) => this.onDocumentMouseMove(e));
    document.addEventListener("mouseup", () => this.onDocumentMouseUp());

    // Keyboard shortcuts
    document.addEventListener("keydown", (e) => this.onKeyDown(e));

    // Canvas click to deselect
    this.canvas?.addEventListener("click", (e) => {
      if (e.target === this.canvas || e.target === this.grid || e.target === this.controlsLayer) {
        this.selectElement(null);
      }
    });
  }

  /**
   * Open the layout designer.
   * @param effectType The effect type to design a layout for
   * @param existingLayout Optional existing layout to edit
   * @param options.blendId Optional blend definition ID (for per-blend layouts)
   * @param options.blendName Optional blend display name for the title
   * @param options.blendParamDefs Optional override parameter definitions (blend params + base params)
   */
  open(
    effectType: string,
    existingLayout?: EffectLayout,
    options?: { blendId?: string; blendName?: string; blendParamDefs?: ParameterDef[] },
  ): void {
    this.initialize();

    if (!this.modal) return;

    // Layout background images are loaded on demand; request them so the canvas
    // can render any referenced backgrounds (a re-render is triggered on arrival).
    ensureLayoutImagesLoaded();

    this.effectType = effectType;
    this.blendId = options?.blendId || "";
    const typeInfo = EffectTypeRegistry.get(effectType);

    // Use blend-specific params if provided, otherwise fall back to registry
    if (options?.blendParamDefs?.length) {
      this.paramDefs = options.blendParamDefs;
    } else {
      this.paramDefs = typeInfo?.parameters || [];
    }
    this.resourceCandidates = this.buildResourceCandidates(typeInfo);

    // Set title — include blend name when designing a per-blend layout
    const baseName = typeInfo?.displayName || effectType;
    const blendSuffix = options?.blendName ? ` — ${options.blendName}` : "";
    if (this.titleEl) {
      this.titleEl.textContent = `Layout Designer - ${baseName}${blendSuffix}`;
    }
    this.fallbackLayoutName = `${baseName}${blendSuffix}`;

    // Load or create layout
    if (existingLayout) {
      this.layout = JSON.parse(JSON.stringify(existingLayout)); // Deep clone
    } else {
      this.layout = this.store.createDefaultLayout(effectType);
      this.layout.name = this.fallbackLayoutName;
    }

    // Ensure layoutId is always set at open time so images can reference it before save.
    if (this.layout && !this.layout.layoutId) {
      this.layout.layoutId = generateLayoutId();
    }

    // Determine whether this is a factory layout (read-only origin).
    // If so, fork a new layoutId so it saves as a brand-new user layout.
    const blendKey = (options?.blendId || "") ? layoutLookupKey(effectType, options?.blendId) : effectType;

    // Look up the library entry by layoutId. Also fall back to the base effectType key:
    // getCustomLayout() may return a non-blend layout as fallback for a blend context,
    // which would cause a key mismatch and incorrectly set isNewLayout = true.
    let libraryEntry = uiState.layoutLibrary?.byEffectType[blendKey]?.find(
      (e) => e.layoutId === this.layout?.layoutId
    );
    if (!libraryEntry && blendKey !== effectType) {
      libraryEntry = uiState.layoutLibrary?.byEffectType[effectType]?.find(
        (e) => e.layoutId === this.layout?.layoutId
      );
    }

    this.isFactoryLayout = libraryEntry?.isFactory === true;
    this.isNewLayout = !libraryEntry || this.isFactoryLayout;

    if (this.isFactoryLayout && this.layout) {
      // Before forking, check if a non-factory user layout already exists for this key.
      // If one does, redirect to editing it instead of creating yet another copy.
      const existingUserEntry =
        uiState.layoutLibrary?.byEffectType[blendKey]?.find(
          (e) => !e.isFactory && e.isDefault
        ) ??
        uiState.layoutLibrary?.byEffectType[blendKey]?.find((e) => !e.isFactory);

      if (existingUserEntry) {
        // Redirect: open the existing user copy instead of forking again
        this.layout = JSON.parse(JSON.stringify(existingUserEntry.layout));
        this.isFactoryLayout = false;
        this.isNewLayout = false;
      } else {
        // No user copy yet: fork a new one from this factory layout
        this.layout.layoutId = generateLayoutId();
        // Rename the fork so it is distinguishable from the factory original
        const forkedFrom = this.layout.name || this.fallbackLayoutName;
        this.layout.name = `${forkedFrom} (copy)`;
      }
    }

    if (this.layout && !Array.isArray(this.layout.overlays)) {
      this.layout.overlays = [];
    }

    // Tag the layout with blendId so it persists across save/load
    if (this.layout && this.blendId) {
      this.layout.blendId = this.blendId;
    }

    // Reset state
    this.selectedElement = null;
    this.gridVisible = true;
    this.previewMode = false;
    this.zoom = 1;
    this.undoStack = [];
    this.redoStack = [];

    // Sync toolbar checkbox
    if (this.useDefaultControlsCheckbox) {
      this.useDefaultControlsCheckbox.checked = this.layout?.useDefaultControls === true;
    }
    // Sync Add Control button dim state
    if (this.addControlBtn) {
      const isDefaultControls = this.layout?.useDefaultControls === true;
      this.addControlBtn.style.opacity = isDefaultControls ? "0.4" : "";
      this.addControlBtn.title = isDefaultControls
        ? "Add Control (disabled \u2014 Default Controls mode)"
        : "Add Parameter Control";
    }

    // Update UI
    this.updateNameInput();
    this.updateDimensionInputs();
    this.updateZoomUI();
    this.updateUndoRedoButtons();
    this.updateDeleteButtonVisibility();
    this.renderCanvas();
    this.renderSidebar();

    // Show modal
    this.modal.style.display = "flex";
  }

  close(didSave: boolean = false): void {
    if (this.modal) {
      this.modal.style.display = "none";
    }
    this.layout = null;
    this.effectType = "";
    this.blendId = "";
    this.selectedElement = null;
    this.onCloseCallback?.(didSave);
  }

  onSave(callback: (layout: EffectLayout) => void): void {
    this.onSaveCallback = callback;
  }

  /** Re-render the canvas when layout images finish loading, if the designer is open. */
  notifyImagesLoaded(): void {
    if (this.modal && this.modal.style.display !== "none") {
      this.renderCanvas();
    }
  }

  onClose(callback: (didSave: boolean) => void): void {
    this.onCloseCallback = callback;
  }


  private updateDeleteButtonVisibility(): void {
    if (!this.deleteBtn) {
      return;
    }
    const canDelete = !!this.layout?.layoutId && !this.isFactoryLayout && !this.isNewLayout;
    this.deleteBtn.hidden = !canDelete;
    this.deleteBtn.disabled = !canDelete;
  }

  private findCurrentLibraryEntry(): { key: string; entries: LayoutLibraryEntry[]; entry: LayoutLibraryEntry } | null {
    const library = uiState.layoutLibrary;
    const layoutId = this.layout?.layoutId;
    if (!library || !layoutId || !this.layout) {
      return null;
    }

    const lookupKeys = Array.from(new Set([
      layoutLookupKey(this.effectType || this.layout.effectType, this.blendId || this.layout.blendId || undefined),
      layoutLookupKey(this.layout.effectType, this.layout.blendId || undefined),
      this.effectType || this.layout.effectType,
      this.layout.effectType,
    ].filter(Boolean)));

    for (const key of lookupKeys) {
      const entries = library.byEffectType[key] ?? [];
      const entry = entries.find((candidate) => candidate.layoutId === layoutId);
      if (entry) {
        return { key, entries, entry };
      }
    }

    return null;
  }

  private async confirmDeleteCurrentLayout(): Promise<void> {
    const current = this.findCurrentLibraryEntry();
    if (!current || current.entry.isFactory) {
      return;
    }

    const name = current.entry.layout.name || current.entry.layout.effectType;
    const confirmed = await showConfirm(`Delete layout "${name}"? This cannot be undone.`, "Delete Layout");
    if (!confirmed) {
      return;
    }

    this.store.deleteCurrentLayout(current.key, current.entries, current.entry);
  }


  /** Collect all image IDs referenced by the current layout. */
  private collectReferencedImageIds(): string[] {
    if (!this.layout) return [];
    const ids = new Set<string>();
    for (const bg of this.layout.backgrounds) {
      if (bg.type === "image" && bg.value) ids.add(bg.value);
    }
    for (const control of this.layout.controls) {
      if (control.style?.knobImageId) ids.add(control.style.knobImageId);
    }
    return Array.from(ids);
  }






  private resetLayout(): void {
    if (!this.effectType) return;

    this.pushUndoState();
    this.layout = this.store.createDefaultLayout(this.effectType);
    this.selectedElement = null;
    this.updateDimensionInputs();
    this.renderCanvas();
    this.renderSidebar();
    showNotification("Layout reset to default");
  }

  private toggleGrid(): void {
    this.gridVisible = !this.gridVisible;
    this.grid?.classList.toggle("grid-visible", this.gridVisible);
    this.gridToggleBtn?.classList.toggle("active", this.gridVisible);
  }

  private togglePreview(): void {
    this.previewMode = !this.previewMode;
    this.canvas?.classList.toggle("preview-mode", this.previewMode);
    this.previewToggleBtn?.classList.toggle("active", this.previewMode);
    
    if (this.previewMode) {
      this.selectElement(null);
      return;
    }

    this.renderCanvas();
  }

  // === Undo / Redo ===

  /** Snapshot the current layout state onto the undo stack. Call before any mutation. */
  private pushUndoState(): void {
    if (!this.layout) return;
    this.undoStack.push(JSON.stringify(this.layout));
    if (this.undoStack.length > LayoutDesignerModal.MAX_UNDO) {
      this.undoStack.shift();
    }
    this.redoStack = [];
    this.updateUndoRedoButtons();
  }

  private undo(): void {
    if (!this.undoStack.length || !this.layout) return;
    this.redoStack.push(JSON.stringify(this.layout));
    this.layout = JSON.parse(this.undoStack.pop()!) as EffectLayout;
    this.selectedElement = null;
    this.updateNameInput();
    this.updateDimensionInputs();
    this.renderCanvas();
    this.renderSidebar();
    this.updateUndoRedoButtons();
  }

  private redo(): void {
    if (!this.redoStack.length || !this.layout) return;
    this.undoStack.push(JSON.stringify(this.layout));
    this.layout = JSON.parse(this.redoStack.pop()!) as EffectLayout;
    this.selectedElement = null;
    this.updateNameInput();
    this.updateDimensionInputs();
    this.renderCanvas();
    this.renderSidebar();
    this.updateUndoRedoButtons();
  }

  private updateUndoRedoButtons(): void {
    if (this.undoBtn) {
      this.undoBtn.disabled = this.undoStack.length === 0;
      this.undoBtn.title = this.undoStack.length ? `Undo (Ctrl+Z) — ${this.undoStack.length} step${this.undoStack.length > 1 ? "s" : ""}` : "Undo (Ctrl+Z)";
    }
    if (this.redoBtn) {
      this.redoBtn.disabled = this.redoStack.length === 0;
      this.redoBtn.title = this.redoStack.length ? `Redo (Ctrl+Y) — ${this.redoStack.length} step${this.redoStack.length > 1 ? "s" : ""}` : "Redo (Ctrl+Y)";
    }
  }

  /** Push undo state once per sidebar editing session (first property change after selection). */
  private pushSidebarUndoOnce(): void {
    if (!this.sidebarUndoPushed) {
      this.sidebarUndoPushed = true;
      this.pushUndoState();
    }
  }

  // === Zoom ===

  private setZoom(level: number): void {
    this.zoom = Math.max(0.25, Math.min(3, level));
    this.updateZoomUI();
    this.updateCanvasSize();
  }

  private updateZoomUI(): void {
    if (this.zoomLabel) {
      this.zoomLabel.textContent = `${Math.round(this.zoom * 100)}%`;
    }
    if (this.zoomInBtn) this.zoomInBtn.disabled = this.zoom >= 3;
    if (this.zoomOutBtn) this.zoomOutBtn.disabled = this.zoom <= 0.25;
  }

  private updateDimensions(): void {
    if (!this.layout || !this.widthInput || !this.heightInput) return;

    this.pushUndoState();
    const width = snapToGrid(Math.max(
      LAYOUT_DIMENSION_LIMITS.minWidth,
      Math.min(LAYOUT_DIMENSION_LIMITS.maxWidth, parseInt(this.widthInput.value) || DEFAULT_LAYOUT_DIMENSIONS.width)
    ));
    const height = snapToGrid(Math.max(
      LAYOUT_DIMENSION_LIMITS.minHeight,
      Math.min(LAYOUT_DIMENSION_LIMITS.maxHeight, parseInt(this.heightInput.value) || DEFAULT_LAYOUT_DIMENSIONS.height)
    ));

    this.layout.dimensions = { width, height };
    this.updateDimensionInputs();
    this.updateCanvasSize();
  }

  private updateDimensionInputs(): void {
    if (!this.layout) return;
    if (this.widthInput) this.widthInput.value = String(this.layout.dimensions.width);
    if (this.heightInput) this.heightInput.value = String(this.layout.dimensions.height);
  }

  /**
   * Copy the name field into the layout. Falls back to the effect display name so
   * the layout picker and layout library never have to show "(Unnamed layout)".
   */
  private applyLayoutName(pushUndo = false): void {
    if (!this.layout || !this.nameInput) return;
    const name = this.nameInput.value.trim() || this.fallbackLayoutName;
    if ((this.layout.name ?? "") === name) return;
    if (pushUndo) this.pushUndoState();
    this.layout.name = name;
    this.updateNameInput();
  }

  private updateNameInput(): void {
    if (!this.nameInput) return;
    this.nameInput.value = this.layout?.name ?? "";
    this.nameInput.placeholder = this.fallbackLayoutName || "Untitled layout";
  }

  private updateCanvasSize(): void {
    if (!this.canvas || !this.layout) return;
    // Canvas element stays at design dimensions; zoom is applied via CSS transform
    this.canvas.style.width = `${this.layout.dimensions.width}px`;
    this.canvas.style.height = `${this.layout.dimensions.height}px`;
    this.canvas.style.transform = `scale(${this.zoom})`;
    this.canvas.style.transformOrigin = "top left";
    // Wrapper takes the zoomed dimensions so parent container scrolls correctly
    if (this.canvasWrapper) {
      this.canvasWrapper.style.width = `${Math.round(this.layout.dimensions.width * this.zoom)}px`;
      this.canvasWrapper.style.height = `${Math.round(this.layout.dimensions.height * this.zoom)}px`;
    }
  }

  private renderCanvas(): void {
    if (!this.canvas || !this.controlsLayer || !this.layout) return;

    this.updateCanvasSize();

    // Mirror the runtime container theme so preview colours match the live experience
    this.canvas.classList.remove('theme-light', 'theme-dark', 'theme-classic');
    if (this.layout.containerTheme) {
      this.canvas.classList.add(`theme-${this.layout.containerTheme}`);
    }

    this.renderBackgrounds();
    this.renderRectangleOverlays();

    if (this.previewMode) {
      this.renderRuntimePreview();
      return;
    }

    this.renderControls();
    this.renderTextLabels();
  }


  private renderRuntimePreview(): void {
    if (!this.controlsLayer || !this.layout) return;

    if (this.layout.useDefaultControls) {
      this.renderDefaultControlsPreview();
      return;
    }

    const previewNode = {
      id: "layout-designer-preview",
      type: this.effectType,
      params: Object.fromEntries(this.paramDefs.map((paramDef) => [paramDef.key, paramDef.default ?? 0])),
    } as unknown as GraphNode;

    const resourceControls: LayoutResourceControlDef[] = this.resourceCandidates.map((candidate) => ({
      resourceControlKey: candidate.controlKey,
      displayName: candidate.displayName,
      resourceType: candidate.resourceType,
      resourceIndex: candidate.resourceIndex,
      exposedResourceId: candidate.exposedResourceId,
      allowBrowseFile: candidate.allowBrowseFile,
      currentDisplayName: candidate.displayName,
      currentFilePath: "",
      isMissing: false,
    }));

    this.controlsLayer.innerHTML = `
      <div class="layout-runtime-preview" style="position: absolute; inset: 0; pointer-events: none;">
        ${renderCustomLayoutPreviewLayers(previewNode, this.layout, this.paramDefs, resourceControls)}
      </div>
    `;
  }

  /**
   * Render the real default controls preview inside the designer canvas for useDefaultControls layouts.
   * Applies the configured offset and scale so the user can visually dial them in.
   * Mirrors the Main / Advanced tab split used by the runtime signal-path panel.
   */
  private renderDefaultControlsPreview(): void {
    if (!this.controlsLayer || !this.layout) return;

    // Mirror the main/advanced split from renderNodeParamsPanel
    let advancedDefs = this.paramDefs.filter((p) => Boolean(p.advanced));
    let mainDefs = this.paramDefs.filter((p) => !p.advanced);
    if (mainDefs.length === 0) {
      mainDefs = this.paramDefs;
      advancedDefs = [];
    }
    const hasAdvancedTab = advancedDefs.length > 0;

    const innerHtml = hasAdvancedTab ? `
      <div class="node-param-tabs" role="tablist" aria-label="Parameter Groups">
        <button class="node-param-tab is-active" data-tab="main" type="button" role="tab" aria-selected="true">Main</button>
        <button class="node-param-tab" data-tab="advanced" type="button" role="tab" aria-selected="false">Advanced</button>
      </div>
      <div class="node-param-tab-panels">
        <div class="node-param-tab-panel is-active" data-tab="main" role="tabpanel">
          <div class="params-controls">
            ${buildDefaultParamControlsHtml(mainDefs, "layout-designer-preview")}
          </div>
        </div>
        <div class="node-param-tab-panel" data-tab="advanced" role="tabpanel">
          <div class="params-controls">
            ${buildDefaultParamControlsHtml(advancedDefs, "layout-designer-preview")}
          </div>
        </div>
      </div>
    ` : `
      <div class="params-controls">
        ${buildDefaultParamControlsHtml(mainDefs, "layout-designer-preview")}
      </div>
    `;

    const offsetX = this.layout.defaultControlsOffset?.x ?? 0;
    const offsetY = this.layout.defaultControlsOffset?.y ?? 0;
    const scaleX = this.layout.defaultControlsScale?.x ?? 1;
    const scaleY = this.layout.defaultControlsScale?.y ?? 1;

    // Second-pass runtime parity: in the live panel, the custom layout sits inside a
    // padded section and may be auto-scaled down to fit narrower widths. Mirror that
    // fit behavior in the designer preview so default-controls wrapping/overflow is
    // closer to what users will see at runtime.
    const canvasViewport = document.getElementById("layout-designer-canvas-container") as HTMLElement | null;
    const viewportWidth = canvasViewport?.clientWidth ?? this.layout.dimensions.width;
    const runtimeSectionHorizontalPadding = 36; // 18px left + 18px right in .default-effect-section-controls
    const runtimeAvailableWidth = Math.max(1, viewportWidth - runtimeSectionHorizontalPadding);
    const runtimeFitScale = Math.min(1, runtimeAvailableWidth / this.layout.dimensions.width);

    const effectiveOffsetX = Math.round(offsetX * runtimeFitScale);
    const effectiveOffsetY = Math.round(offsetY * runtimeFitScale);
    const effectiveScaleX = scaleX * runtimeFitScale;
    const effectiveScaleY = scaleY * runtimeFitScale;
    const hasTransformOrOffset = effectiveOffsetX !== 0 || effectiveOffsetY !== 0 || effectiveScaleX !== 1 || effectiveScaleY !== 1;
    // Pin wrapper to canvas width so flex-wrap break points are identical in the runtime panel.
    const wrapperWidth = `width: ${this.layout.dimensions.width}px;`;
    const wrapperStyle = hasTransformOrOffset
      ? `position: absolute; left: ${effectiveOffsetX}px; top: ${effectiveOffsetY}px; transform: scale(${effectiveScaleX}, ${effectiveScaleY}); transform-origin: top left; z-index: 2; ${wrapperWidth}`
      : `position: relative; z-index: 2; ${wrapperWidth}`;

    // Use the same class structure as runtime (custom-layout-backdrop + theme) so that
    // .custom-layout-backdrop.theme-* CSS rules apply identically in designer and runtime.
    const themeClass = this.layout.containerTheme ? ` theme-${this.layout.containerTheme}` : '';

    this.controlsLayer.innerHTML = `
      <div class="layout-default-controls-preview custom-layout-backdrop${themeClass}" style="${wrapperStyle} pointer-events: none;">
        ${innerHtml}
      </div>
    `;
  }

  private renderBackgrounds(): void {
    if (!this.canvas || !this.layout) return;

    // Remove existing background layers
    this.canvas.querySelectorAll(".layout-designer-background").forEach((el) => el.remove());

    // Render each background layer
    this.layout.backgrounds.forEach((bg) => {
      const isSelected = this.selectedElement?.type === "background" && this.selectedElement.layerIndex === bg.layerIndex;
      const layer = document.createElement("div");
      layer.className = `layout-designer-background layer-${bg.layerIndex}${isSelected ? " selected" : ""}`;
      layer.dataset.layerIndex = String(bg.layerIndex);

      if (bg.type === "color") {
        layer.style.backgroundColor = bg.value;
      } else if (bg.type === "gradient") {
        layer.style.background = bg.value;
      } else if (bg.type === "image") {
        // bg.value is imageId - resolve to actual URL
        const imageUrl = getLayoutImageUrl(bg.value);
        if (imageUrl) {
          layer.style.backgroundImage = `url(${imageUrl})`;
          // Apply size mode or custom scale
          if (bg.size === "custom" && bg.scale !== undefined) {
            layer.style.backgroundSize = `${bg.scale * 100}%`;
          } else if (bg.size === "stretch") {
            layer.style.backgroundSize = "100% 100%";
          } else {
            layer.style.backgroundSize = bg.size || "cover";
          }
          layer.style.backgroundRepeat = bg.size === "tile" ? "repeat" : "no-repeat";
          layer.style.backgroundPosition = `${bg.offsetX || 0}px ${bg.offsetY || 0}px`;
        }
      }

      if (bg.opacity !== undefined && bg.opacity < 1) {
        layer.style.opacity = String(bg.opacity);
      }

      // Make background clickable for selection
      layer.addEventListener("click", (e) => {
        e.stopPropagation();
        this.selectElement({ type: "background", layerIndex: bg.layerIndex });
      });

      if (this.canvas && this.grid) {
        this.canvas.insertBefore(layer, this.grid);
      }
    });
  }

  private renderRectangleOverlays(): void {
    if (!this.canvas || !this.layout) return;

    this.canvas.querySelectorAll(".layout-rectangle-overlay").forEach((el) => el.remove());

    const overlays = this.layout.overlays ?? [];
    overlays.forEach((overlay) => {
      const el = this.createRectangleOverlayElement(overlay);
      if (this.canvas) {
        this.canvas.appendChild(el);
      }
    });
  }

  private createRectangleOverlayElement(overlay: LayoutRectangleOverlay): HTMLElement {
    const isSelected = this.selectedElement?.type === "overlay" && this.selectedElement.id === overlay.id;
    const backgroundColor = overlay.style?.backgroundColor || "#000000";
    const backgroundOpacity = typeof overlay.style?.backgroundOpacity === "number"
      ? Math.max(0, Math.min(1, overlay.style.backgroundOpacity))
      : 0.25;
    const borderColor = overlay.style?.borderColor || "#ffffff";
    const borderWidth = Math.max(0, overlay.style?.borderWidth ?? 1);
    const borderRadius = Math.max(0, overlay.style?.borderRadius ?? 0);
    const toggleBypassOnClick = overlay.style?.toggleBypassOnClick === true;
    const fill = colorWithAlpha(backgroundColor, backgroundOpacity);

    const el = document.createElement("div");
    el.className = `layout-rectangle-overlay${isSelected ? " selected" : ""}`;
    el.dataset.overlayId = overlay.id;
    el.style.left = `${overlay.position.x}px`;
    el.style.top = `${overlay.position.y}px`;
    el.style.width = `${overlay.size.width}px`;
    el.style.height = `${overlay.size.height}px`;
    el.style.backgroundColor = fill;
    el.style.border = `${borderWidth}px solid ${borderColor}`;
    el.style.borderRadius = `${borderRadius}px`;

    if (toggleBypassOnClick && !this.previewMode) {
      const badgeEl = document.createElement("span");
      badgeEl.className = "layout-overlay-power-badge";
      badgeEl.textContent = "PWR";
      el.appendChild(badgeEl);
    }

    el.addEventListener("click", (e) => {
      e.stopPropagation();
      if (!this.previewMode) {
        this.selectElement({ type: "overlay", id: overlay.id });
      }
    });

    if (isSelected && !this.previewMode) {
      (["top-left", "top-right", "bottom-left", "bottom-right"] as const).forEach((handle) => {
        const handleEl = document.createElement("div");
        handleEl.className = `layout-resize-handle ${handle}`;
        handleEl.dataset.overlayHandle = handle;
        el.appendChild(handleEl);
      });
    }

    return el;
  }

  private getReusableLayoutImages(purpose: "background" | "knob", selectedImageId?: string): LayoutImageRef[] {
    const images = uiState.layoutLibrary?.images ?? [];
    const available = images.filter((img) => Boolean(img.imageId) && (Boolean(img.dataUrl) || Boolean(img.fileName)));
    const preferredType = purpose === "background" ? "background" : "knob";
    const compatible = available.filter((img) => {
      if (img.imageId === selectedImageId) {
        return true;
      }
      if (!img.type || img.type === "general") {
        return true;
      }
      return img.type === preferredType;
    });
    return [...compatible].sort((a, b) => {
      const aPriority = a.type === preferredType ? 0 : a.imageId === selectedImageId ? 1 : 2;
      const bPriority = b.type === preferredType ? 0 : b.imageId === selectedImageId ? 1 : 2;
      if (aPriority !== bPriority) {
        return aPriority - bPriority;
      }
      const aName = (a.fileName || a.imageId).toLowerCase();
      const bName = (b.fileName || b.imageId).toLowerCase();
      return aName.localeCompare(bName);
    });
  }

  private renderImageOptionsHtml(purpose: "background" | "knob", selectedImageId?: string): string {
    const images = this.getReusableLayoutImages(purpose, selectedImageId);
    const options = [
      `<option value="">Select existing image...</option>`,
      ...images.map((img) => {
        const selected = selectedImageId && img.imageId === selectedImageId ? "selected" : "";
        const typeLabel = img.type ? ` [${img.type}]` : "";
        const name = img.fileName || img.imageId;
        return `<option value="${img.imageId}" ${selected}>${name}${typeLabel}</option>`;
      }),
    ];
    return options.join("");
  }

  private renderControls(): void {
    if (!this.controlsLayer || !this.layout) return;

    // Clear existing controls
    this.controlsLayer.querySelectorAll(".layout-control-placeholder, .layout-runtime-preview, .layout-default-controls-placeholder, .layout-default-controls-preview").forEach((el) => el.remove());

    // When useDefaultControls is on, render a live preview of the real controls at the configured position/scale
    if (this.layout.useDefaultControls) {
      this.renderDefaultControlsPreview();
      return;
    }

    // Render each control
    this.layout.controls.forEach((control) => {
      const el = this.createControlElement(control);
      this.controlsLayer!.appendChild(el);
    });
  }

  private toggleUseDefaultControls(): void {
    if (!this.layout || !this.useDefaultControlsCheckbox) return;
    this.pushUndoState();
    this.layout.useDefaultControls = this.useDefaultControlsCheckbox.checked;
    // Dim the "Add Control" button to signal it has no effect in this mode
    if (this.addControlBtn) {
      this.addControlBtn.style.opacity = this.layout.useDefaultControls ? "0.4" : "";
      this.addControlBtn.title = this.layout.useDefaultControls
        ? "Add Control (disabled — Default Controls mode)"
        : "Add Parameter Control";
    }
    this.selectedElement = null;
    this.renderCanvas();
    this.renderSidebar();
  }

  private createControlElement(control: LayoutControl): HTMLElement {
    const isResourceControl = this.isResourceControl(control);
    const paramDef = isResourceControl ? undefined : this.paramDefs.find((p) => p.key === control.paramKey);
    const resourceDef = isResourceControl ? this.resourceCandidates.find((candidate) => candidate.controlKey === control.paramKey) : undefined;
    const label = control.labelOverride || resourceDef?.displayName || paramDef?.name || control.paramKey;
    const isSelected =
      this.selectedElement?.type === "control" && this.selectedElement.paramKey === control.paramKey;

    const el = document.createElement("div");
    el.className = `layout-control-placeholder${isSelected ? " selected" : ""}`;
    el.dataset.paramKey = control.paramKey;
    el.style.left = `${control.position.x}px`;
    el.style.top = `${control.position.y}px`;

    if (control.size) {
      el.style.width = `${control.size.width}px`;
      el.style.height = `${control.size.height}px`;
    }

    const labelPos = control.style?.labelPosition || "top";
    const hideLabel = control.style?.hideLabel === true || labelPos === "none";
    const showValue = control.style?.showValue !== false;

    // Build control HTML
    let html = "";

    if (!hideLabel && labelPos === "top") {
      html += `<span class="control-label">${label}</span>`;
    }

    if (isResourceControl || control.type === "dropdown") {
      html += `<div class="control-dropdown">${label}</div>`;
    } else if (control.type === "toggle") {
      html += `<div class="control-toggle"></div>`;
    } else if (control.type === "slider") {
      html += `<div class="control-slider"><div class="control-slider-track"><div class="control-slider-thumb"></div></div></div>`;
    } else {
      const knobStyle = control.style?.knobStyle || "default";
      const knobImageUrl = knobStyle === "custom" && control.style?.knobImageId
        ? getLayoutImageUrl(control.style.knobImageId)
        : null;
      const customKnobClass = knobImageUrl ? " is-custom-image" : "";
      const customKnobStyle = knobImageUrl ? ` style="background-image: url('${knobImageUrl}');"` : "";
      html += `<div class="control-knob${customKnobClass}" data-style="${knobStyle}"${customKnobStyle}></div>`;
    }

    if (!hideLabel && labelPos === "bottom") {
      html += `<span class="control-label position-bottom">${label}</span>`;
    }

    if (!isResourceControl && showValue) {
      html += `<span class="control-value">0.00</span>`;
    }

    el.innerHTML = html;

    // Click to select
    el.addEventListener("click", (e) => {
      e.stopPropagation();
      if (!this.previewMode) {
        this.selectElement({ type: "control", paramKey: control.paramKey });
      }
    });

    return el;
  }

  private renderTextLabels(): void {
    if (!this.controlsLayer || !this.layout) return;

    // Clear existing text labels
    this.controlsLayer.querySelectorAll(".layout-text-label, .layout-runtime-preview").forEach((el) => el.remove());

    // Render each label
    this.layout.textLabels.forEach((label) => {
      const el = this.createTextLabelElement(label);
      this.controlsLayer!.appendChild(el);
    });
  }

  private createTextLabelElement(label: LayoutTextLabel): HTMLElement {
    const isSelected = this.selectedElement?.type === "label" && this.selectedElement.id === label.id;

    const el = document.createElement("div");
    el.className = `layout-text-label${isSelected ? " selected" : ""}`;
    el.dataset.labelId = label.id;
    el.style.left = `${label.position.x}px`;
    el.style.top = `${label.position.y}px`;
    el.style.fontSize = `${label.fontSize}px`;
    el.style.fontWeight = label.fontWeight || "normal";
    if (label.fontFamily) {
      el.style.fontFamily = label.fontFamily;
    }
    el.style.color = label.color || "var(--text-dark-primary)";
    el.style.textAlign = label.textAlign || "left";
    el.textContent = label.text;

    // Click to select
    el.addEventListener("click", (e) => {
      e.stopPropagation();
      if (!this.previewMode) {
        this.selectElement({ type: "label", id: label.id });
      }
    });

    // Double-click to edit
    el.addEventListener("dblclick", (e) => {
      e.stopPropagation();
      if (!this.previewMode) {
        this.editTextLabel(label, el);
      }
    });

    return el;
  }

  private editTextLabel(label: LayoutTextLabel, el: HTMLElement): void {
    el.classList.add("editing");

    const input = document.createElement("input");
    input.type = "text";
    input.value = label.text;
    input.style.fontSize = `${label.fontSize}px`;
    input.style.color = label.color || "inherit";

    el.textContent = "";
    el.appendChild(input);
    input.focus();
    input.select();

    const finishEdit = () => {
      const newText = input.value.trim();
      if (newText) {
        label.text = newText;
      }
      el.classList.remove("editing");
      el.textContent = label.text;
      this.renderSidebar();
    };

    input.addEventListener("blur", finishEdit);
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        finishEdit();
      } else if (e.key === "Escape") {
        el.classList.remove("editing");
        el.textContent = label.text;
      }
    });
  }

  private selectElement(element: SelectedElement): void {
    this.selectedElement = element;
    this.renderCanvas();
    this.renderSidebar();
  }

  private renderSidebar(): void {
    if (!this.sidebarContent) return;
    this.sidebarUndoPushed = false;

    if (!this.selectedElement) {
      this.canvasProperties.renderCanvasProperties();
      return;
    }

    if (this.selectedElement.type === "control") {
      this.elementProperties.renderControlProperties(this.selectedElement.paramKey);
    } else if (this.selectedElement.type === "label") {
      this.elementProperties.renderLabelProperties(this.selectedElement.id);
    } else if (this.selectedElement.type === "overlay") {
      this.elementProperties.renderOverlayProperties(this.selectedElement.id);
    } else if (this.selectedElement.type === "background") {
      this.canvasProperties.renderBackgroundProperties(this.selectedElement.layerIndex);
    }
  }








  private addTextLabel(): void {
    if (!this.layout) return;

    this.pushUndoState();
    const newLabel: LayoutTextLabel = {
      id: generateLabelId(),
      text: "New Label",
      position: { x: snapToGrid(this.layout.dimensions.width / 2 - 40), y: snapToGrid(20) },
      fontSize: 14,
      fontWeight: "normal",
      color: "#ffffff",
      textAlign: "center",
    };

    this.layout.textLabels.push(newLabel);
    this.selectElement({ type: "label", id: newLabel.id });
    this.renderCanvas();
  }

  private addRectangleOverlay(): void {
    if (!this.layout) return;

    this.pushUndoState();
    const newOverlay: LayoutRectangleOverlay = {
      id: generateOverlayId(),
      position: {
        x: snapToGrid(this.layout.dimensions.width / 2 - 80),
        y: snapToGrid(this.layout.dimensions.height / 2 - 50),
      },
      size: {
        width: 160,
        height: 100,
      },
      style: {
        visibilityMode: "always",
        toggleBypassOnClick: false,
        backgroundColor: "#000000",
        backgroundOpacity: 0.25,
        borderColor: "#ffffff",
        borderWidth: 1,
        borderRadius: 0,
      },
    };

    if (!this.layout.overlays) {
      this.layout.overlays = [];
    }
    this.layout.overlays.push(newOverlay);
    this.selectElement({ type: "overlay", id: newOverlay.id });
    this.renderCanvas();
  }



  private showAddBackgroundMenu(): void {
    if (!this.layout || !this.sidebarContent) return;

    if (this.layout.backgrounds.length >= 2) {
      showNotification("Maximum 2 background layers");
      return;
    }

    const targetLayerIndex = this.layout.backgrounds.length;
    const images = this.getReusableLayoutImages("background");

    this.selectedElement = null;
    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Add Background Layer ${targetLayerIndex + 1}</div>
        <div style="font-size: 11px; color: var(--text-dark-muted); margin-bottom: 8px;">Choose an existing image resource or browse for a new file</div>
        <div class="layout-property-row">
          <span class="layout-property-label">Use Existing</span>
          <div class="layout-property-input">
            <select id="layout-add-bg-image-select">
              ${this.renderImageOptionsHtml("background")}
            </select>
          </div>
        </div>
        ${images.length > 0 ? `
        <div style="font-size: 10px; color: var(--text-dark-muted); margin-top: 6px;">
          Found ${images.length} reusable image${images.length === 1 ? "" : "s"}
        </div>
        ` : ""}
        <div class="layout-image-actions" style="margin-top: 8px;">
          <button id="layout-add-bg-browse">Browse...</button>
        </div>
      </div>
    `;

    const existingSelect = document.getElementById("layout-add-bg-image-select") as HTMLSelectElement;
    const browseBtn = document.getElementById("layout-add-bg-browse") as HTMLButtonElement;

    existingSelect?.addEventListener("change", () => {
      const imageId = existingSelect.value;
      if (!imageId) return;
      this.applyImageBackground(targetLayerIndex, imageId);
    });

    browseBtn?.addEventListener("click", () => {
      this.browseBackgroundImage(targetLayerIndex);
    });
  }

  private applyImageBackground(layerIndex: number, imageId: string): void {
    if (!this.layout) return;

    this.pushUndoState();
    const existingIndex = this.layout.backgrounds.findIndex((bg) => bg.layerIndex === layerIndex);
    const newBg: LayoutBackground = {
      layerIndex,
      type: "image",
      value: imageId,
      opacity: 1,
      size: "contain",
    };

    if (existingIndex >= 0) {
      this.layout.backgrounds[existingIndex] = newBg;
    } else {
      this.layout.backgrounds.push(newBg);
    }

    this.selectElement({ type: "background", layerIndex });
    this.renderCanvas();
    this.renderSidebar();
  }

  private browseBackgroundImage(layerIndex?: number): void {
    console.log("[LayoutDesigner] browseBackgroundImage called");
    postMessage({
      type: "browseLayoutImage",
      purpose: "background",
      layerIndex: layerIndex ?? (this.layout?.backgrounds.length || 0),
      layoutId: this.layout?.layoutId ?? "",
    });
  }

  private addColorBackground(): void {
    if (!this.layout) return;

    if (this.layout.backgrounds.length >= 2) {
      showNotification("Maximum 2 background layers");
      return;
    }

    const layerIndex = this.layout.backgrounds.length;
    this.pushUndoState();
    const newBg: LayoutBackground = {
      layerIndex,
      type: "color",
      value: "#1a1a2e",
      opacity: 1,
    };

    this.layout.backgrounds.push(newBg);
    this.selectElement({ type: "background", layerIndex });
    this.renderCanvas();
    this.renderSidebar();
  }

  private showAddControlMenu(): void {
    if (!this.layout || !this.sidebarContent) return;

    // Find params not already in layout
    const usedKeys = new Set(this.layout.controls.map((c) => c.paramKey));
    const availableParams = this.paramDefs.filter((p) => !usedKeys.has(p.key));
    const availableResources = this.resourceCandidates.filter((resource) => !usedKeys.has(resource.controlKey));

    if (availableParams.length === 0 && availableResources.length === 0) {
      showNotification("All controls are already in the layout");
      return;
    }

    // Show available params in sidebar
    this.selectedElement = null;
    this.sidebarContent.innerHTML = `
      <div class="layout-property-group">
        <div class="layout-property-group-title">Add Parameter Control</div>
        <div style="font-size: 11px; color: var(--text-dark-muted); margin-bottom: 8px;">Click a parameter or resource selector to add it to the layout</div>
        ${availableParams
          .map(
            (p) => `
          <button
            class="layout-add-control-btn"
            data-param-key="${p.key}"
            style="
              display: block;
              width: 100%;
              padding: 6px 10px;
              margin-bottom: 4px;
              background: rgba(255, 255, 255, 0.05);
              border: 1px solid rgba(255, 255, 255, 0.1);
              border-radius: 4px;
              color: var(--text-dark-primary);
              font-size: 12px;
              cursor: pointer;
              text-align: left;
            "
          >
            ${p.name || p.key} <span style="opacity: 0.5; font-size: 10px;">${p.unit || ""}</span>
          </button>
        `
          )
          .join("")}
        ${availableResources.length > 0 ? `
          <div style="margin: 10px 0 6px; font-size: 11px; color: var(--text-dark-muted);">Resource Selectors</div>
          ${availableResources
            .map(
              (resource) => `
            <button
              class="layout-add-control-btn"
              data-resource-key="${resource.controlKey}"
              style="
                display: block;
                width: 100%;
                padding: 6px 10px;
                margin-bottom: 4px;
                background: rgba(255, 255, 255, 0.05);
                border: 1px solid rgba(255, 255, 255, 0.1);
                border-radius: 4px;
                color: var(--text-dark-primary);
                font-size: 12px;
                cursor: pointer;
                text-align: left;
              "
            >
              ${resource.displayName} <span style="opacity: 0.5; font-size: 10px;">${resource.resourceType}</span>
            </button>
          `,
            )
            .join("")}
        ` : ""}
      </div>
    `;

    // Bind click handlers
    this.sidebarContent.querySelectorAll(".layout-add-control-btn").forEach((btn) => {
      btn.addEventListener("click", () => {
        const paramKey = (btn as HTMLElement).dataset.paramKey;
        const resourceKey = (btn as HTMLElement).dataset.resourceKey;
        if (paramKey) {
          this.addControlForParam(paramKey);
        } else if (resourceKey) {
          this.addControlForResource(resourceKey);
        }
      });
    });
  }

  private addControlForParam(paramKey: string): void {
    if (!this.layout) return;

    const paramDef = this.paramDefs.find((p) => p.key === paramKey);
    if (!paramDef) return;

    this.pushUndoState();
    // Place in center of canvas
    const newControl: LayoutControl = {
      paramKey,
      bindingType: "parameter",
      type: paramDef.unit === "toggle" ? "toggle" : "knob",
      position: {
        x: snapToGrid(this.layout.dimensions.width / 2 - 24),
        y: snapToGrid(this.layout.dimensions.height / 2 - 24),
      },
      style: {
        labelPosition: "top",
        showValue: true,
        valuePosition: "bottom",
        knobStyle: "default",
      },
    };

    this.layout.controls.push(newControl);
    this.selectElement({ type: "control", paramKey });
    this.renderCanvas();
  }

  private addControlForResource(resourceKey: string): void {
    if (!this.layout) return;

    const resource = this.resourceCandidates.find((candidate) => candidate.controlKey === resourceKey);
    if (!resource) return;

    this.pushUndoState();

    const newControl: LayoutControl = {
      paramKey: resource.controlKey,
      bindingType: "resource",
      type: "dropdown",
      resourceType: resource.resourceType,
      resourceIndex: resource.resourceIndex,
      exposedResourceId: resource.exposedResourceId,
      allowBrowseFile: resource.allowBrowseFile,
      position: {
        x: snapToGrid(this.layout.dimensions.width / 2 - 90),
        y: snapToGrid(this.layout.dimensions.height / 2 - 18),
      },
      size: {
        width: 180,
        height: 36,
      },
      style: {
        labelPosition: "top",
        showValue: false,
        valuePosition: "bottom",
      },
      labelOverride: resource.displayName,
    };

    this.layout.controls.push(newControl);
    this.selectElement({ type: "control", paramKey: resource.controlKey });
    this.renderCanvas();
  }

  private isResourceControl(control: LayoutControl): boolean {
    return control.bindingType === "resource" || control.paramKey.startsWith("__resource__:");
  }

  private buildResourceCandidates(typeInfo?: { requiresResource?: boolean; resourceType?: string; exposedResources?: Array<{
    resourceId: string;
    displayName: string;
    resourceType: string;
    resourceIndex?: number;
    allowBrowseFile?: boolean;
  }> }): LayoutResourceCandidate[] {
    const candidates: LayoutResourceCandidate[] = [];

    const exposed = typeInfo?.exposedResources ?? [];
    if (exposed.length > 0) {
      exposed.forEach((resource, index) => {
        const resourceIndex = typeof resource.resourceIndex === "number" ? resource.resourceIndex : index;
        candidates.push({
          controlKey: `__resource__:${resource.resourceId}:${resourceIndex}`,
          displayName: resource.displayName || resource.resourceId,
          resourceType: resource.resourceType,
          resourceIndex,
          exposedResourceId: resource.resourceId,
          allowBrowseFile: resource.allowBrowseFile ?? true,
        });
      });
      return candidates;
    }

    if (typeInfo?.requiresResource && typeInfo.resourceType) {
      candidates.push({
        controlKey: `__resource__:primary:0`,
        displayName: typeInfo.resourceType === "nam" ? "Model" : typeInfo.resourceType === "ir" ? "IR" : "Resource",
        resourceType: typeInfo.resourceType,
        resourceIndex: 0,
        allowBrowseFile: true,
      });
    }

    return candidates;
  }

  private browseKnobImage(control: LayoutControl): void {
    postMessage({
      type: "browseLayoutImage",
      purpose: "knob",
      paramKey: control.paramKey,
      layoutId: this.layout?.layoutId ?? "",
    });
  }

  /** Called when the plugin responds with a selected image */
  handleImageSelected(purpose: string, imageId: string, layerIndex?: number, paramKey?: string): void {
    console.log("[LayoutDesigner] handleImageSelected:", { purpose, imageId, layerIndex, paramKey });
    if (!this.layout) {
      console.warn("[LayoutDesigner] handleImageSelected: no layout!");
      return;
    }

    if (purpose === "background" && layerIndex !== undefined) {
      this.applyImageBackground(layerIndex, imageId);
    } else if (purpose === "knob" && paramKey) {
      this.pushUndoState();
      const control = this.layout.controls.find((c) => c.paramKey === paramKey);
      if (control) {
        if (!control.style) control.style = {};
        control.style.knobImageId = imageId;
      }

      this.renderCanvas();
      this.renderSidebar();
      return;
    }

    this.renderCanvas();
    this.renderSidebar();
  }

  // === Drag and Drop ===

  private onCanvasMouseDown(e: MouseEvent): void {
    if (this.previewMode) return;

    const target = e.target as HTMLElement;
    const resizeHandle = target.closest(".layout-resize-handle") as HTMLElement;
    const rectangle = target.closest(".layout-rectangle-overlay") as HTMLElement;
    const placeholder = target.closest(".layout-control-placeholder") as HTMLElement;
    const textLabel = target.closest(".layout-text-label") as HTMLElement;

    if (resizeHandle && rectangle) {
      const overlayId = rectangle.dataset.overlayId;
      const handle = resizeHandle.dataset.overlayHandle as "top-left" | "top-right" | "bottom-left" | "bottom-right" | undefined;
      if (overlayId && handle) {
        this.startDrag(e, rectangle, "overlay", overlayId, "resize", handle);
      }
      return;
    }

    if (rectangle) {
      const overlayId = rectangle.dataset.overlayId;
      if (overlayId) {
        this.startDrag(e, rectangle, "overlay", overlayId, "move");
      }
      return;
    }

    if (placeholder) {
      const paramKey = placeholder.dataset.paramKey;
      if (paramKey) {
        this.startDrag(e, placeholder, "control", paramKey, "move");
      }
    } else if (textLabel) {
      const labelId = textLabel.dataset.labelId;
      if (labelId) {
        this.startDrag(e, textLabel, "label", labelId, "move");
      }
    }
  }

  private startDrag(
    e: MouseEvent,
    element: HTMLElement,
    type: "control" | "label" | "overlay",
    id: string,
    mode: "move" | "resize",
    resizeHandle: "top-left" | "top-right" | "bottom-left" | "bottom-right" | null = null,
  ): void {
    e.preventDefault();

    this.pushUndoState();
    this.dragState = {
      active: true,
      element,
      startX: e.clientX,
      startY: e.clientY,
      elementStartX: parseInt(element.style.left) || 0,
      elementStartY: parseInt(element.style.top) || 0,
      elementStartWidth: parseInt(element.style.width) || 0,
      elementStartHeight: parseInt(element.style.height) || 0,
      type,
      mode,
      resizeHandle,
      id,
    };

    element.classList.add("dragging");
  }

  private onDocumentMouseMove(e: MouseEvent): void {
    if (!this.dragState.active || !this.dragState.element || !this.layout) return;

    const dx = e.clientX - this.dragState.startX;
    const dy = e.clientY - this.dragState.startY;

    if (this.dragState.type === "overlay" && this.dragState.mode === "resize") {
      const minSize = 16;
      let newX = this.dragState.elementStartX;
      let newY = this.dragState.elementStartY;
      let newWidth = this.dragState.elementStartWidth;
      let newHeight = this.dragState.elementStartHeight;

      switch (this.dragState.resizeHandle) {
        case "top-left":
          newX = snapToGrid(this.dragState.elementStartX + dx / this.zoom);
          newY = snapToGrid(this.dragState.elementStartY + dy / this.zoom);
          newWidth = snapToGrid(this.dragState.elementStartWidth - dx / this.zoom);
          newHeight = snapToGrid(this.dragState.elementStartHeight - dy / this.zoom);
          break;
        case "top-right":
          newY = snapToGrid(this.dragState.elementStartY + dy / this.zoom);
          newWidth = snapToGrid(this.dragState.elementStartWidth + dx / this.zoom);
          newHeight = snapToGrid(this.dragState.elementStartHeight - dy / this.zoom);
          break;
        case "bottom-left":
          newX = snapToGrid(this.dragState.elementStartX + dx / this.zoom);
          newWidth = snapToGrid(this.dragState.elementStartWidth - dx / this.zoom);
          newHeight = snapToGrid(this.dragState.elementStartHeight + dy / this.zoom);
          break;
        case "bottom-right":
        default:
          newWidth = snapToGrid(this.dragState.elementStartWidth + dx / this.zoom);
          newHeight = snapToGrid(this.dragState.elementStartHeight + dy / this.zoom);
          break;
      }

      if (newWidth < minSize) {
        if (this.dragState.resizeHandle === "top-left" || this.dragState.resizeHandle === "bottom-left") {
          newX += newWidth - minSize;
        }
        newWidth = minSize;
      }
      if (newHeight < minSize) {
        if (this.dragState.resizeHandle === "top-left" || this.dragState.resizeHandle === "top-right") {
          newY += newHeight - minSize;
        }
        newHeight = minSize;
      }

      newX = Math.max(0, Math.min(this.layout.dimensions.width - minSize, newX));
      newY = Math.max(0, Math.min(this.layout.dimensions.height - minSize, newY));
      newWidth = Math.min(newWidth, this.layout.dimensions.width - newX);
      newHeight = Math.min(newHeight, this.layout.dimensions.height - newY);

      this.dragState.element.style.left = `${newX}px`;
      this.dragState.element.style.top = `${newY}px`;
      this.dragState.element.style.width = `${Math.max(minSize, newWidth)}px`;
      this.dragState.element.style.height = `${Math.max(minSize, newHeight)}px`;
      return;
    }

    const newX = snapToGrid(this.dragState.elementStartX + dx / this.zoom);
    const newY = snapToGrid(this.dragState.elementStartY + dy / this.zoom);
    const dragWidth = this.dragState.type === "overlay"
      ? (parseInt(this.dragState.element.style.width) || this.dragState.elementStartWidth || 60)
      : 60;
    const dragHeight = this.dragState.type === "overlay"
      ? (parseInt(this.dragState.element.style.height) || this.dragState.elementStartHeight || 60)
      : 60;

    // Clamp to canvas bounds
    const clampedX = Math.max(0, Math.min(this.layout.dimensions.width - dragWidth, newX));
    const clampedY = Math.max(0, Math.min(this.layout.dimensions.height - dragHeight, newY));

    this.dragState.element.style.left = `${clampedX}px`;
    this.dragState.element.style.top = `${clampedY}px`;
  }

  private onDocumentMouseUp(): void {
    if (!this.dragState.active || !this.layout) return;

    const { element, type, id } = this.dragState;

    if (element) {
      element.classList.remove("dragging");

      const newX = parseInt(element.style.left) || 0;
      const newY = parseInt(element.style.top) || 0;

      // Update model
      if (type === "control") {
        const control = this.layout.controls.find((c) => c.paramKey === id);
        if (control) {
          control.position = { x: newX, y: newY };
        }
      } else if (type === "label") {
        const label = this.layout.textLabels.find((l) => l.id === id);
        if (label) {
          label.position = { x: newX, y: newY };
        }
      } else if (type === "overlay") {
        const overlay = (this.layout.overlays ?? []).find((item) => item.id === id);
        if (overlay) {
          overlay.position = { x: newX, y: newY };
          if (this.dragState.mode === "resize") {
            overlay.size = {
              width: Math.max(16, parseInt(element.style.width) || overlay.size.width),
              height: Math.max(16, parseInt(element.style.height) || overlay.size.height),
            };
          }
        }
      }

      // Update sidebar if selected
      if (
        (this.selectedElement?.type === "control" && this.selectedElement.paramKey === id) ||
        (this.selectedElement?.type === "label" && this.selectedElement.id === id) ||
        (this.selectedElement?.type === "overlay" && this.selectedElement.id === id)
      ) {
        this.renderSidebar();
      }
    }

    this.dragState = {
      active: false,
      element: null,
      startX: 0,
      startY: 0,
      elementStartX: 0,
      elementStartY: 0,
      elementStartWidth: 0,
      elementStartHeight: 0,
      type: null,
      mode: "move",
      resizeHandle: null,
      id: "",
    };
  }

  // === Keyboard Shortcuts ===

  private onKeyDown(e: KeyboardEvent): void {
    if (!this.modal || this.modal.style.display === "none") return;
    if (this.isEditableKeyboardTarget(e.target)) return;

    // Undo/Redo: Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z
    if ((e.ctrlKey || e.metaKey) && !e.altKey) {
      if (e.key === "c" || e.key === "C") {
        if (this.copySelectedTextLabel()) {
          e.preventDefault();
        }
        return;
      }
      if (e.key === "v" || e.key === "V") {
        if (this.pasteCopiedTextLabel()) {
          e.preventDefault();
        }
        return;
      }
      if (e.key === "z" || e.key === "Z") {
        e.preventDefault();
        if (e.shiftKey) {
          this.redo();
        } else {
          this.undo();
        }
        return;
      }
      if (e.key === "y" || e.key === "Y") {
        e.preventDefault();
        this.redo();
        return;
      }
    }

    if (e.key === "Escape") {
      if (this.selectedElement) {
        this.selectElement(null);
      } else {
        this.close();
      }
      return;
    }

    if (e.key === "Delete" || e.key === "Backspace") {
      this.deleteSelectedElement();
      return;
    }

    // Arrow keys for nudging
    if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(e.key)) {
      e.preventDefault();
      const nudge = e.shiftKey ? LAYOUT_GRID_SIZE : 1;
      this.nudgeSelectedElement(e.key, nudge);
    }

    // Grid toggle: G
    if (e.key === "g" || e.key === "G") {
      this.toggleGrid();
    }

    // Preview toggle: P
    if (e.key === "p" || e.key === "P") {
      this.togglePreview();
    }

    // Zoom: + / - / 0
    if (e.key === "+" || e.key === "=") {
      this.setZoom(this.zoom + 0.25);
    }
    if (e.key === "-" || e.key === "_") {
      this.setZoom(this.zoom - 0.25);
    }
    if (e.key === "0") {
      this.setZoom(1);
    }
  }

  private isEditableKeyboardTarget(target: EventTarget | null): boolean {
    if (!(target instanceof HTMLElement)) return false;
    if (target.isContentEditable) return true;

    const tagName = target.tagName;
    return tagName === "INPUT" || tagName === "TEXTAREA" || tagName === "SELECT";
  }

  private copySelectedTextLabel(): boolean {
    if (!this.layout || this.selectedElement?.type !== "label") {
      return false;
    }

    const selectedLabelId = this.selectedElement.id;
    const label = this.layout.textLabels.find((item) => item.id === selectedLabelId);
    if (!label) {
      return false;
    }

    this.copiedTextLabel = {
      text: label.text,
      position: { ...label.position },
      fontSize: label.fontSize,
      fontWeight: label.fontWeight,
      fontFamily: label.fontFamily,
      color: label.color,
      textAlign: label.textAlign,
    };
    return true;
  }

  private pasteCopiedTextLabel(): boolean {
    if (!this.layout || !this.copiedTextLabel) {
      return false;
    }

    const selectedLabel = this.selectedElement?.type === "label" ? this.selectedElement : null;
    const sourcePosition = selectedLabel
      ? this.layout.textLabels.find((item) => item.id === selectedLabel.id)?.position
      : undefined;
    const pasteOffset = LAYOUT_GRID_SIZE * 2;
    const basePosition = sourcePosition ?? this.copiedTextLabel.position;
    const nextX = Math.max(0, Math.min(
      this.layout.dimensions.width,
      snapToGrid(basePosition.x + pasteOffset),
    ));
    const nextY = Math.max(0, Math.min(
      this.layout.dimensions.height,
      snapToGrid(basePosition.y + pasteOffset),
    ));

    const newLabel: LayoutTextLabel = {
      id: generateLabelId(),
      text: this.copiedTextLabel.text,
      position: { x: nextX, y: nextY },
      fontSize: this.copiedTextLabel.fontSize,
      fontWeight: this.copiedTextLabel.fontWeight,
      fontFamily: this.copiedTextLabel.fontFamily,
      color: this.copiedTextLabel.color,
      textAlign: this.copiedTextLabel.textAlign,
    };

    this.pushUndoState();
    this.layout.textLabels.push(newLabel);
    this.selectElement({ type: "label", id: newLabel.id });
    this.renderCanvas();
    return true;
  }

  private deleteSelectedElement(): void {
    if (!this.layout || !this.selectedElement) return;

    this.pushUndoState();
    if (this.selectedElement.type === "control") {
      const selectedParamKey = this.selectedElement.paramKey;
      this.layout.controls = this.layout.controls.filter(
        (c) => c.paramKey !== selectedParamKey
      );
    } else if (this.selectedElement.type === "label") {
      this.layout.textLabels = this.layout.textLabels.filter(
        (l) => l.id !== (this.selectedElement as { type: "label"; id: string }).id
      );
    } else if (this.selectedElement.type === "overlay") {
      const selectedOverlayId = this.selectedElement.id;
      this.layout.overlays = (this.layout.overlays ?? []).filter(
        (item) => item.id !== selectedOverlayId
      );
    } else if (this.selectedElement.type === "background") {
      const selectedLayerIndex = this.selectedElement.layerIndex;
      this.layout.backgrounds = this.layout.backgrounds.filter(
        (b) => b.layerIndex !== selectedLayerIndex
      );
    }

    this.selectElement(null);
    this.renderCanvas();
  }

  private nudgeSelectedElement(key: string, amount: number): void {
    if (!this.layout || !this.selectedElement) return;

    // Debounced undo: push once at start of a nudge sequence, not on every arrow press
    if (!this.nudgeUndoTimer) {
      this.pushUndoState();
    } else {
      clearTimeout(this.nudgeUndoTimer);
    }
    this.nudgeUndoTimer = setTimeout(() => { this.nudgeUndoTimer = null; }, 500);

    let position: { x: number; y: number } | undefined;

    if (this.selectedElement.type === "control") {
      const selectedParamKey = this.selectedElement.paramKey;
      const control = this.layout.controls.find((c) => c.paramKey === selectedParamKey);
      position = control?.position;
    } else if (this.selectedElement.type === "label") {
      const label = this.layout.textLabels.find(
        (l) => l.id === (this.selectedElement as { type: "label"; id: string }).id
      );
      position = label?.position;
    } else if (this.selectedElement.type === "overlay") {
      const selectedOverlayId = this.selectedElement.id;
      const overlay = (this.layout.overlays ?? []).find((item) => item.id === selectedOverlayId);
      position = overlay?.position;
    }

    if (!position) return;

    switch (key) {
      case "ArrowUp":
        position.y = Math.max(0, position.y - amount);
        break;
      case "ArrowDown":
        position.y = Math.min(this.layout.dimensions.height - 20, position.y + amount);
        break;
      case "ArrowLeft":
        position.x = Math.max(0, position.x - amount);
        break;
      case "ArrowRight":
        position.x = Math.min(this.layout.dimensions.width - 20, position.x + amount);
        break;
    }

    this.renderCanvas();
    this.renderSidebar();
  }
}

// Singleton instance
export const layoutDesigner = new LayoutDesignerModal();
