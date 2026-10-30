/**
 * What the designer adds to its sidebar however the sidebar is drawn: thumbnail image pickers
 * (imagePicker.ts) and property groups that fold (collapsibleGroups.ts). The sidebar is rendered
 * from many places, and some re-render only part of it, so one observer enhances whatever is new.
 */

import { enhanceCollapsibleGroups } from "./collapsibleGroups.js";
import { enhanceImageSelects } from "./imagePicker.js";

export function watchSidebar(root: HTMLElement): void {
  const enhance = () => {
    enhanceImageSelects(root);
    enhanceCollapsibleGroups(root);
  };
  enhance();
  new MutationObserver(enhance).observe(root, { childList: true, subtree: true });
}
