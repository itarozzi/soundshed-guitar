# UI Components

This directory contains the split HTML fragments for the Soundshed Guitar UI.

## Usage
- Fragments are inlined at build time by `scripts/assemble-html.js` using `<!--#include:ui-components/xxx.html-->` markers in `index.html` (or `index.template.html`).
- Run `npm run build` (or `npm run build:html`) after edits.
- CMake tracks these files for rebuilds.

## Markup only, no inline script
- The fragments are static markup. Behaviour, and any markup built from data, lives in TypeScript.
- `index.template.html` sets a Content-Security-Policy with no `'unsafe-inline'` or `'unsafe-eval'`
  for scripts, so an `on*=` attribute, a `javascript:` URL or a template directive that evaluates
  expressions (Alpine's `x-*`, which the UI used to load) will not run. Alpine was removed for
  that reason: it evaluates attribute text as code, so any markup injected from a shared preset
  would have run too.
- An inline `<script>` in the template is allowed, and hashed into the policy at build time.
- Values interpolated into markup go through `escapeHtml` (`ts/utils.ts`); ESLint's
  `soundshed/no-unescaped-html-attribute` rule enforces it for attribute values.

## Adding a new component
1. Create `some-component.html` here with the markup.
2. Add `<!--#include:ui-components/some-component.html-->` in the shell.
3. Render its dynamic parts, and bind its events, from the matching `.ts` module.
4. Rebuild and test.

## Current extractions (full port in progress - index.template.html is the thin editable source with markers; run build to produce runtime index.html)
Top-level:
- splash-screen.html
- header-icon-bar.html
- control-bar.html (thin shell)
  - input-control-group.html
  - preset-group.html (further includes below)
  - output-control-group.html
  - jam-player-dock.html
- jam-floating-player-root.html
- preset-toolbar-row.html
- preset-selector-row.html
- preset-library-popover.html
- signal-path-bar.html
- fx-selector-panel.html
- main-content.html (thin)
  - panels/advanced-panel.html
  - panels/jam-panel.html
  - panels/sharing-panel.html
- footer-bar.html
- notification-area.html

Modals (all extracted to modals/):
- custom-effect-designer-modal.html
- riff-save-modal.html
- riff-capture-modal.html
- tone3000-details-modal.html
- metronome-modal.html
- user-input-calibration-modal.html
- tuner-modal.html
- eq-modal.html
- blend-editor-modal.html
- resource-browser-modal.html
- blend-model-browser-modal.html
- save-preset-modal.html
- tone-sharing-pack-view-modal.html
- tone-sharing-publish-modal.html
- tone-sharing-consent-modal.html
- tone-sharing-signin-modal.html
- tone-sharing-pack-modal.html
- layout-designer-modal.html
- dialog-modal.html
- tone3000-required-modal.html
(and any additional discovered)

See plan.md for phases and goals. All logic remains in TypeScript. Edit index.template.html + fragments.
