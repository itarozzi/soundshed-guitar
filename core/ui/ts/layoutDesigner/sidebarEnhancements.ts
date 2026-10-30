/**
 * What the designer adds to its sidebar however the sidebar is drawn: thumbnail image pickers
 * (imagePicker.ts) and property groups that fold (collapsibleGroups.ts). The sidebar is rendered
 * from many places, and some re-render only part of it, so one observer enhances whatever is new.
 */

import { postMessage } from "../bridge.js";
import { enhanceCollapsibleGroups } from "./collapsibleGroups.js";
import { enhanceImageSelects, setImagePickerBrowse } from "./imagePicker.js";

/** `layoutId` names the layout open in the designer, the folder a browsed image goes into. */
export function watchSidebar(root: HTMLElement, layoutId?: () => string): void {
  if (layoutId) {
    setImagePickerBrowse((purpose, pickerId) =>
      postMessage({ type: "browseLayoutImage", purpose, pickerId, layoutId: layoutId() }));
  }
  const enhance = () => {
    enhanceImageSelects(root);
    enhanceCollapsibleGroups(root);
  };
  enhance();
  new MutationObserver(enhance).observe(root, { childList: true, subtree: true });
}
