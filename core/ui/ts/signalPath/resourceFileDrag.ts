/**
 * Lights up the model and IR slots while a file is dragged over the window, so a
 * user learns a slot takes a dropped file at the moment it matters.
 *
 * File names are hidden until the drop, so the kind is guessed from the MIME type
 * the browser does expose: audio is an IR, and an unregistered type (a .nam file
 * has none) is a model. Anything else, such as a preset pack zip, lights nothing.
 */

const DRAG_ATTRIBUTE = "resourceFileDrag";
/** dragover keeps firing while a drag sits over the page, unmoved too, but a browser
 *  may space those repeats out to ~550 ms; leaving the window is caught sooner below. */
const DRAG_IDLE_CLEAR_MS = 1000;

export function classifyDraggedResourceTypes(mimeTypes: readonly string[]): Array<"nam" | "ir"> {
  const kinds = new Set<"nam" | "ir">();
  for (const type of mimeTypes) {
    const lower = type.trim().toLowerCase();
    if (lower.startsWith("audio/")) {
      kinds.add("ir");
    } else if (lower === "" || lower === "application/octet-stream") {
      kinds.add("nam");
    }
  }
  return Array.from(kinds).sort();
}

function draggedFileTypes(event: DragEvent): string[] | null {
  const transfer = event.dataTransfer;
  if (!transfer || !Array.from(transfer.types).includes("Files")) {
    return null;
  }
  return Array.from(transfer.items ?? [])
    .filter((item) => item.kind === "file")
    .map((item) => item.type);
}

let initialized = false;

export function initResourceSlotDragHighlight(): void {
  if (initialized) {
    return;
  }
  initialized = true;

  const root = document.documentElement;
  let clearTimer: ReturnType<typeof setTimeout> | null = null;

  const clear = (): void => {
    if (clearTimer !== null) {
      clearTimeout(clearTimer);
      clearTimer = null;
    }
    delete root.dataset[DRAG_ATTRIBUTE];
  };

  // Capture phase: the slots stop their own dragover from bubbling.
  document.addEventListener("dragover", (event) => {
    const types = draggedFileTypes(event);
    if (!types) {
      return;
    }
    const kinds = classifyDraggedResourceTypes(types);
    if (kinds.length) {
      root.dataset[DRAG_ATTRIBUTE] = kinds.join(" ");
    } else {
      delete root.dataset[DRAG_ATTRIBUTE];
    }
    if (clearTimer !== null) {
      clearTimeout(clearTimer);
    }
    clearTimer = setTimeout(clear, DRAG_IDLE_CLEAR_MS);
  }, true);

  // relatedTarget is not reliable on a dragleave, so leaving the window is told by
  // position; the idle timer catches whatever this misses.
  document.addEventListener("dragleave", (event) => {
    if (event.clientX <= 0 || event.clientY <= 0 || event.clientX >= window.innerWidth || event.clientY >= window.innerHeight) {
      clear();
    }
  }, true);
  document.addEventListener("drop", clear, true);
  document.addEventListener("dragend", clear, true);
}
