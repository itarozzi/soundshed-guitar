/**
 * How the looper's message handlers ask for the strip to be redrawn without importing it: the
 * facade (looper.ts) registers the renderer once at start-up. `full` asks for the markup again;
 * otherwise only what moves is updated, unless the strip finds its shape has changed.
 */

let renderer: ((full: boolean) => void) | null = null;

export function registerLooperRenderer(render: (full: boolean) => void): void {
  renderer = render;
}

export function requestLooperRender(full = false): void {
  renderer?.(full);
}
