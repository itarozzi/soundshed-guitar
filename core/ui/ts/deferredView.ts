/**
 * Views that render only while they can be seen.
 *
 * Every engine `state` asks the whole UI to redraw, and some of what it redraws is
 * hidden most of the time: the Settings resource library, the preset list popover.
 * Building those while nobody can see them cost tens of milliseconds per state. A
 * deferred view renders straight away when it is on screen; otherwise it only
 * remembers that it is out of date, and renders the next time it is shown.
 *
 * "Shown" is announced, not observed: whatever reveals a view (a panel switch, a tab,
 * a popover opening) calls `flushShownViews()` afterwards. This module imports
 * nothing, so any module can call it without forming a cycle.
 */

export interface DeferredView {
  /** Renders now if the view is shown, else marks it out of date. */
  request(): void;
  /** Renders if the view is out of date and now shown. */
  flush(): void;
}

const views = new Set<DeferredView>();

export function createDeferredView(isShown: () => boolean, render: () => void): DeferredView {
  let stale = true;
  const view: DeferredView = {
    request() {
      if (isShown()) {
        stale = false;
        render();
      } else {
        stale = true;
      }
    },
    flush() {
      if (stale && isShown()) {
        stale = false;
        render();
      }
    },
  };
  views.add(view);
  return view;
}

/** Call after revealing part of the UI: every out-of-date view that is now shown renders. */
export function flushShownViews(): void {
  views.forEach((view) => view.flush());
}
