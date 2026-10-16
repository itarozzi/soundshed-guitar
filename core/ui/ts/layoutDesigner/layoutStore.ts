/**
 * Getting a layout in and out of the app: saving it, deleting it, exporting it
 * as a file, importing one back, and the thumbnail that represents it in the
 * library.
 *
 * A layout is addressed by the effect type it belongs to, plus the blend id
 * when it is a blend's own layout — so every path through here has to carry
 * both, and the host is asked for them rather than being assumed.
 */

import { postMessage } from "../bridge.js";
import type { EffectLayout, LayoutLibraryEntry } from "../layoutTypes.js";
import type { ParameterDef } from "../presetV2.js";
import type { SelectedElement } from "./types.js";
import { DEFAULT_LAYOUT_DIMENSIONS, createEmptyLayout, generateLayoutId, layoutLookupKey, snapToGrid } from "../layoutTypes.js";
import { showNotification } from "../notifications.js";
import { uiState } from "../state.js";
import { arrayBufferToBase64 } from "../utils.js";
import { LAYOUT_LIBRARY_CHANGED_EVENT } from "./events.js";
import { loadThumbnailImage } from "./images.js";
import { prepareLayoutImport } from "./layoutImport.js";

/**
 * What the store needs from the designer around it: which layout is open, what
 * it belongs to, and the few pieces of designer state a save changes.
 */
export interface LayoutStoreHost {
  getLayout(): EffectLayout | null;
  setLayout(layout: EffectLayout | null): void;
  /** The effect type this layout is for. */
  getEffectType(): string;
  /** Set when the layout belongs to a blend rather than a plain effect type. */
  getBlendId(): string;
  /** The name to fall back on when the layout has none of its own. */
  getFallbackLayoutName(): string;
  /** Factory layouts are saved as a user copy rather than overwritten. */
  isFactoryLayout(): boolean;
  setFactoryLayout(value: boolean): void;
  /** True until the layout has been persisted for the first time. */
  isNewLayout(): boolean;
  setNewLayout(value: boolean): void;
  /** The parameters a default layout should lay out controls for. */
  getParamDefs(): ParameterDef[];
  /** Parameter and resource keys the open effect has, so an import can say what does not match. */
  getKnownControlKeys(): ReadonlySet<string>;
  /** Read the name field back into the layout before saving. */
  applyLayoutName(pushUndo?: boolean): void;
  updateDeleteButtonVisibility(): void;
  /** Told after a successful save, for a caller that asked to be. */
  notifySaved(layout: EffectLayout): void;
  /** Snapshot for undo, before an import replaces what is open. */
  pushUndoState(): void;
  /** Redraw the toolbar fields (name, size, default-controls toggle) after an import changed them. */
  refreshEditorChrome(): void;
  /** Disable Save while the engine is writing the layout. */
  setSaving(saving: boolean): void;
  renderCanvas(): void;
  selectElement(element: SelectedElement): void;
  /** The file input behind Import — read it, then clear it for the next pick. */
  takeImportFile(): File | null;
  closeModal(didSave?: boolean): void;
}

/** How long a save waits for the engine to answer before Save is offered again. */
const SAVE_ANSWER_TIMEOUT_MS = 15000;

interface PendingSave {
  layoutId: string;
  layout: EffectLayout;
  timer: ReturnType<typeof setTimeout>;
}

export class LayoutStore {
  /** The save sent to the engine and not yet answered. */
  private pendingSave: PendingSave | null = null;
  /** Set from the click until the save is sent, so a double click cannot send two. */
  private preparingSave = false;

  constructor(private readonly host: LayoutStoreHost) {}

  private persistLayoutLocally(layout: EffectLayout): void {
    if (!uiState.layoutLibrary) {
      uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
    }

    const blendId = this.host.getBlendId() || layout.blendId || "";
    const lookupKey = layoutLookupKey(this.host.getEffectType() || layout.effectType, blendId || undefined);
    if (!uiState.layoutLibrary.byEffectType[lookupKey]) {
      uiState.layoutLibrary.byEffectType[lookupKey] = [];
    }

    const list = uiState.layoutLibrary.byEffectType[lookupKey];
    const layoutId = layout.layoutId || generateLayoutId();
    layout.layoutId = layoutId;

    const clonedLayout = JSON.parse(JSON.stringify(layout)) as EffectLayout;
    const existingIndex = list.findIndex((entry) => entry.layoutId === layoutId);
    const newEntry: LayoutLibraryEntry = {
      layout: clonedLayout,
      isDefault: true,
      layoutId,
      filePath: existingIndex >= 0 ? list[existingIndex].filePath : undefined,
    };

    if (existingIndex >= 0) {
      list[existingIndex] = newEntry;
    } else {
      list.push(newEntry);
    }

    uiState.layoutLibrary.defaults[lookupKey] = layoutId;
    uiState.layoutLibrary.byEffectType[lookupKey] = list.map((entry) => ({
      ...entry,
      isDefault: entry.layoutId === layoutId,
    }));
    window.dispatchEvent(new CustomEvent(LAYOUT_LIBRARY_CHANGED_EVENT));
  }

  deleteCurrentLayout(key: string, entries: LayoutLibraryEntry[], entry: LayoutLibraryEntry): void {
    const library = uiState.layoutLibrary;
    if (!library) {
      return;
    }
    const layoutName = entry.layout.name || entry.layout.effectType;

    postMessage({
      type: "deleteLayout",
      effectType: entry.layout.effectType,
      blendId: entry.layout.blendId ?? "",
      layoutId: entry.layoutId,
    });

    library.byEffectType[key] = entries.filter((candidate) => candidate.layoutId !== entry.layoutId);
    if (library.defaults[key] === entry.layoutId) {
      const nextDefault =
        library.byEffectType[key]?.find((candidate) => !candidate.isFactory)?.layoutId ??
        library.byEffectType[key]?.[0]?.layoutId;
      if (nextDefault) {
        library.defaults[key] = nextDefault;
        library.byEffectType[key] = library.byEffectType[key].map((candidate) => ({
          ...candidate,
          isDefault: candidate.layoutId === nextDefault,
        }));
      } else {
        delete library.defaults[key];
      }
    }

    if ((library.byEffectType[key] ?? []).length === 0) {
      delete library.byEffectType[key];
    }

    window.dispatchEvent(new CustomEvent(LAYOUT_LIBRARY_CHANGED_EVENT));
    showNotification(`Layout "${layoutName}" deleted`);
    this.host.closeModal();
  }

  /**
   * Send the layout to the engine. The designer stays open until the engine answers
   * (`layoutSaved` or `layoutSaveFailed`, through handleSaveResult), so a save that fails
   * keeps the work on screen instead of closing over it with "Layout saved".
   */
  async save(): Promise<void> {
    const layout = this.host.getLayout();
    if (!layout || this.pendingSave || this.preparingSave) return;
    this.preparingSave = true;
    try {
      await this.sendSave(layout);
    } finally {
      this.preparingSave = false;
    }
  }

  private async sendSave(layout: EffectLayout): Promise<void> {
    // Pick up any name edit that has not been committed by a blur yet
    this.host.applyLayoutName();
    if (!layout.name) {
      layout.name = this.host.getFallbackLayoutName();
    }

    // The engine mints an id for a layout without one, and the library cache here would
    // then hold the layout under a different one until the next reload.
    if (!layout.layoutId) {
      layout.layoutId = generateLayoutId();
    }

    layout.modifiedAt = new Date().toISOString();

    // Ensure blendId is stored in the layout itself
    if (this.host.getBlendId()) {
      layout.blendId = this.host.getBlendId();
    }

    // Capture a thumbnail of the current design state before persisting
    try {
      const thumbnail = await this.captureLayoutThumbnail();
      if (thumbnail) layout.thumbnailDataUrl = thumbnail;
    } catch { /* thumbnail failure must not block save */ }

    // The designer may have closed while the thumbnail rendered.
    if (this.host.getLayout() !== layout) return;

    const layoutId = layout.layoutId;
    this.pendingSave = {
      layoutId,
      layout: JSON.parse(JSON.stringify(layout)) as EffectLayout,
      timer: setTimeout(() => this.handleSaveTimeout(layoutId), SAVE_ANSWER_TIMEOUT_MS),
    };
    this.host.setSaving(true);

    // The engine copies every image the layout draws on into the layout's own folder.
    postMessage({
      type: "saveEffectLayout",
      effectType: this.host.getEffectType(),
      blendId: this.host.getBlendId() || undefined,
      layoutId,
      layout,
    });
  }

  /**
   * The engine's answer to a save. Returns false when it is not for the save this
   * designer is waiting on, so the caller can report it some other way.
   */
  handleSaveResult(result: { layoutId?: string; ok: boolean; message?: string }): boolean {
    const pending = this.pendingSave;
    if (!pending || result.layoutId !== pending.layoutId) return false;

    clearTimeout(pending.timer);
    this.pendingSave = null;
    this.host.setSaving(false);

    if (!result.ok) {
      showNotification(`Layout not saved: ${result.message || "the engine could not write it"}`);
      return true;
    }

    // After first save the layout is no longer new/factory
    this.host.setNewLayout(false);
    this.host.setFactoryLayout(false);
    this.host.updateDeleteButtonVisibility();
    this.persistLayoutLocally(pending.layout);
    this.host.notifySaved(pending.layout);
    showNotification("Layout saved");
    this.host.closeModal(true);
    return true;
  }

  /** Forget a save still in flight: the designer is closing, so nothing waits on the answer. */
  cancelPendingSave(): void {
    if (this.pendingSave) {
      clearTimeout(this.pendingSave.timer);
      this.pendingSave = null;
    }
    this.host.setSaving(false);
  }

  private handleSaveTimeout(layoutId: string): void {
    if (this.pendingSave?.layoutId !== layoutId) return;
    this.pendingSave = null;
    this.host.setSaving(false);
    showNotification("The layout may not have saved: the engine did not answer. Save again to be sure.");
  }

  /** Render a compact thumbnail of the current layout onto an offscreen canvas and return a JPEG data URL. */
  private async captureLayoutThumbnail(): Promise<string | null> {
    const layout = this.host.getLayout();
    if (!layout) return null;

    const THUMB_W = 280;
    const layoutW = layout.dimensions.width;
    const layoutH = layout.dimensions.height;
    const scale = Math.min(THUMB_W / layoutW, 1);
    const scaledW = Math.ceil(layoutW * scale);
    const scaledH = Math.ceil(layoutH * scale);

    const canvasEl = document.createElement("canvas");
    canvasEl.width = scaledW;
    canvasEl.height = scaledH;
    const ctx = canvasEl.getContext("2d");
    if (!ctx) return null;

    // Base fill for transparent/unset areas
    ctx.fillStyle = "#1c1c1c";
    ctx.fillRect(0, 0, scaledW, scaledH);

    // Backgrounds
    const sortedBgs = [...layout.backgrounds].sort((a, b) => a.layerIndex - b.layerIndex);
    for (const bg of sortedBgs) {
      ctx.globalAlpha = bg.opacity ?? 1;
      if (bg.type === "color") {
        ctx.fillStyle = bg.value;
        ctx.fillRect(0, 0, scaledW, scaledH);
      } else if (bg.type === "image") {
        const imageRef = uiState.layoutLibrary?.images.find((img) => img.imageId === bg.value);
        if (imageRef?.dataUrl) {
          try {
            const imgEl = await loadThumbnailImage(imageRef.dataUrl);
            ctx.drawImage(imgEl, 0, 0, scaledW, scaledH);
          } catch { /* ignore failed image */ }
        }
      }
      ctx.globalAlpha = 1;
    }

    // Rectangle overlays
    for (const overlay of layout.overlays ?? []) {
      const ox = overlay.position.x * scale;
      const oy = overlay.position.y * scale;
      const ow = overlay.size.width * scale;
      const oh = overlay.size.height * scale;
      const style = overlay.style ?? {};
      if (style.backgroundColor) {
        ctx.globalAlpha = style.backgroundOpacity ?? 1;
        ctx.fillStyle = style.backgroundColor;
        ctx.fillRect(ox, oy, ow, oh);
        ctx.globalAlpha = 1;
      }
      if (style.borderColor && (style.borderWidth ?? 0) > 0) {
        ctx.strokeStyle = style.borderColor;
        ctx.lineWidth = Math.max(0.5, (style.borderWidth ?? 1) * scale);
        ctx.strokeRect(ox, oy, ow, oh);
      }
    }

    // Control indicator dots
    if (!layout.useDefaultControls) {
      ctx.fillStyle = "rgba(90, 159, 212, 0.55)";
      const r = Math.max(4, 7 * scale);
      for (const control of layout.controls) {
        const cx = control.position.x * scale + r;
        const cy = control.position.y * scale + r;
        ctx.beginPath();
        ctx.arc(cx, cy, r, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // Text labels
    for (const label of layout.textLabels) {
      const fontSize = Math.max(5, Math.round((label.fontSize || 11) * scale));
      ctx.font = `${label.fontWeight ?? "normal"} ${fontSize}px ${label.fontFamily ?? "sans-serif"}`;
      ctx.fillStyle = label.color ?? "#dddddd";
      ctx.globalAlpha = 0.9;
      ctx.textAlign = (label.textAlign as CanvasTextAlign) ?? "left";
      ctx.fillText(label.text, label.position.x * scale, label.position.y * scale + fontSize);
      ctx.globalAlpha = 1;
    }

    return canvasEl.toDataURL("image/jpeg", 0.82);
  }

  async exportLayout(): Promise<void> {
    const layout = this.host.getLayout();
    if (!layout) return;

    const zipLib = window.JSZip;
    if (!zipLib) {
      showNotification("Export failed: archive library not available");
      return;
    }

    const zip = new zipLib();

    // Collect all image IDs referenced by this layout
    const referencedImageIds = new Set<string>();
    for (const bg of layout.backgrounds) {
      if (bg.type === "image" && bg.value) {
        referencedImageIds.add(bg.value);
      }
    }
    for (const control of layout.controls) {
      if (control.style?.knobImageId) {
        referencedImageIds.add(control.style.knobImageId);
      }
    }

    // Add referenced images to zip
    const imagesFolder = zip.folder("images");
    const imageManifest: Array<{ imageId: string; fileName: string; type?: string }> = [];

    if (imagesFolder) {
      const images = uiState.layoutLibrary?.images ?? [];
      for (const imageId of referencedImageIds) {
        const img = images.find((i) => i.imageId === imageId);
        if (!img?.dataUrl) continue;

        // Extract base64 data from data URL (data:image/png;base64,...)
        const match = img.dataUrl.match(/^data:image\/([^;]+);base64,(.+)$/);
        if (!match) continue;

        const ext = match[1] === "jpeg" ? "jpg" : match[1];
        const base64Data = match[2];
        const fileName = img.fileName || `${imageId}.${ext}`;

        imagesFolder.file(fileName, base64Data, { base64: true });
        imageManifest.push({
          imageId: img.imageId,
          fileName,
          type: img.type,
        });
      }
    }

    // Build layout JSON for export (includes image manifest for reimport).
    // When useDefaultControls is true the controls array is redundant — strip it
    // to keep the exported file clean.
    const exportLayout = layout.useDefaultControls
      ? { ...layout, controls: [] }
      : layout;

    const exportData = {
      formatVersion: 1,
      createdAt: new Date().toISOString(),
      layout: exportLayout,
      images: imageManifest,
    };

    zip.file("layout.json", JSON.stringify(exportData, null, 2));

    const blob = await zip.generateAsync({ type: "blob" });
    const buffer = await blob.arrayBuffer();
    const data = arrayBufferToBase64(buffer);

    const safeName = this.host.getEffectType().replace(/[^a-zA-Z0-9_-]/g, "_");
    const blendSuffix = this.host.getBlendId() ? `--${this.host.getBlendId().replace(/[^a-zA-Z0-9_-]/g, "_")}` : "";
    postMessage({
      type: "exportEffectLayout",
      fileName: `${safeName}${blendSuffix}.sgfxlayout.zip`,
      data,
    });
  }

  async handleImportFileSelected(): Promise<void> {
    const file = this.host.takeImportFile();
    if (!file) return;

    const zipLib = window.JSZip;
    if (!zipLib) {
      showNotification("Import failed: archive library not available");
      return;
    }

    const current = this.host.getLayout();
    if (!current?.layoutId) return;

    try {
      const buffer = await file.arrayBuffer();
      const zip = await zipLib.loadAsync(buffer);

      const layoutEntry = zip.file("layout.json");
      if (!layoutEntry) {
        showNotification("Import failed: archive is missing layout.json");
        return;
      }

      const archive = JSON.parse(await layoutEntry.async("text")) as unknown;
      const prepared = await prepareLayoutImport(
        archive,
        async (archivePath) => {
          const entry = zip.file(archivePath);
          return entry ? entry.async("arraybuffer") : null;
        },
        {
          layoutId: current.layoutId,
          effectType: this.host.getEffectType() || current.effectType,
          blendId: this.host.getBlendId() || undefined,
          name: current.name,
          createdAt: current.createdAt,
        },
        this.host.getKnownControlKeys(),
      );

      if ("error" in prepared) {
        showNotification(`Import failed: ${prepared.error}`);
        return;
      }

      // The images go into the open layout's own folder, under names chosen here.
      if (!uiState.layoutLibrary) {
        uiState.layoutLibrary = { byEffectType: {}, defaults: {}, images: [] };
      }
      for (const image of prepared.images) {
        uiState.layoutLibrary.images.push({
          imageId: image.imageId,
          fileName: image.fileName,
          dataUrl: image.dataUrl,
          type: image.type,
        });
        postMessage({
          type: "saveLayoutImage",
          imageId: image.imageId,
          fileName: image.fileName,
          data: image.base64,
          layoutId: current.layoutId,
        });
      }

      this.host.pushUndoState();
      this.host.setLayout(prepared.layout);
      this.host.refreshEditorChrome();
      this.host.renderCanvas();
      this.host.selectElement(null);

      const notes: string[] = [];
      if (prepared.skippedImages.length) {
        notes.push(`${prepared.skippedImages.length} image${prepared.skippedImages.length === 1 ? "" : "s"} could not be used`);
      }
      if (prepared.unmatchedControls) {
        notes.push(`${prepared.unmatchedControls} control${prepared.unmatchedControls === 1 ? " doesn't" : "s don't"} match this effect`);
      }
      showNotification(notes.length ? `Layout imported; ${notes.join(", ")}` : "Layout imported");
    } catch (err) {
      console.error("[LayoutDesigner] Import failed:", err);
      showNotification(`Import failed: ${err instanceof Error ? err.message : "unknown error"}`);
    }
  }

  createDefaultLayout(effectType: string): EffectLayout {
    const paramDefs = this.host.getParamDefs();
    const layout = createEmptyLayout(effectType);

    // Auto-populate controls from param definitions
    const controlsPerRow = 4;
    const controlSpacing = 80;
    const startX = 40;
    const startY = 60;

    paramDefs.forEach((param, index) => {
      const row = Math.floor(index / controlsPerRow);
      const col = index % controlsPerRow;

      layout.controls.push({
        paramKey: param.key,
        type: param.unit === "toggle" ? "toggle" : "knob",
        position: {
          x: snapToGrid(startX + col * controlSpacing),
          y: snapToGrid(startY + row * controlSpacing),
        },
        style: {
          labelPosition: "top",
          showValue: true,
          valuePosition: "bottom",
          knobStyle: "default",
        },
      });
    });

    // Adjust dimensions to fit controls
    const rows = Math.ceil(paramDefs.length / controlsPerRow);
    layout.dimensions.height = Math.max(
      DEFAULT_LAYOUT_DIMENSIONS.height,
      snapToGrid(startY + rows * controlSpacing + 40)
    );

    return layout;
  }
}
