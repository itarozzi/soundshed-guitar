/**
 * Indirection for "reload what the feed is showing".
 *
 * browse.ts decides what each mode loads, and to do that it imports the feed
 * renderer, the AI search view and the pack detail view. Some of those need to
 * ask for a reload in turn — the community search box once typing settles, an
 * install that changed which packs count as installed — and importing browse.ts
 * back would tie them into an import cycle with it.
 *
 * So they request the reload and browse.ts registers the implementation once, at
 * module load. Same pattern as presets/refresh.ts and signalPath/render.ts.
 */

let reload: (() => Promise<void>) | null = null;
let refreshAfterInstall: (() => Promise<void>) | null = null;

/** Called once by browse.ts to supply the real loaders. */
export function setBrowseLoaders(loaders: { reload: () => Promise<void>; refreshAfterInstall: () => Promise<void> }): void {
  reload = loaders.reload;
  refreshAfterInstall = loaders.refreshAfterInstall;
}

/** Reloads the feed for the current browse mode. */
export function requestBrowseReload(): Promise<void> {
  return reload ? reload() : Promise.resolve();
}

/** Redraws whichever view an install can have changed: the feed, the installed list, an open pack. */
export function requestBrowseRefreshAfterInstall(): Promise<void> {
  return refreshAfterInstall ? refreshAfterInstall() : Promise.resolve();
}
