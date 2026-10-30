/**
 * Property groups that fold away: clicking a group's title (or Enter or Space on it) hides the
 * rest of the group. Which groups are folded is remembered by title, numbers dropped ("Background
 * Layer 2" folds with "Background Layer 1"), in this browser only, so a sidebar drawn again, or the
 * designer opened again, keeps them folded.
 */

const STORAGE_KEY = "layoutDesigner.collapsedGroups";
const BOUND = "collapsible";

function loadCollapsed(): Set<string> {
  try {
    const stored: unknown = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? "[]");
    return new Set(Array.isArray(stored) ? stored.filter((key): key is string => typeof key === "string") : []);
  } catch {
    return new Set();
  }
}

function saveCollapsed(collapsed: Set<string>): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify([...collapsed]));
  } catch {
    // Storage may be unavailable; the group still folds until the sidebar is drawn again.
  }
}

/** The key a group's fold is remembered by: its title in lower case, a trailing number dropped. */
export function groupKey(title: string): string {
  return title.trim().replace(/\s*\d+$/, "").toLowerCase();
}

function setCollapsed(group: HTMLElement, title: HTMLElement, collapsed: boolean): void {
  group.classList.toggle("is-collapsed", collapsed);
  title.setAttribute("aria-expanded", String(!collapsed));
}

/** Make every titled property group under `root` that is not yet foldable fold on its title. */
export function enhanceCollapsibleGroups(root: ParentNode): void {
  let collapsed: Set<string> | null = null;
  root.querySelectorAll<HTMLElement>(".layout-property-group").forEach((group) => {
    const title = group.querySelector<HTMLElement>(":scope > .layout-property-group-title");
    if (!title || title.dataset[BOUND]) return;
    title.dataset[BOUND] = "true";
    title.setAttribute("role", "button");
    title.tabIndex = 0;
    const key = groupKey(title.textContent ?? "");
    collapsed ??= loadCollapsed();
    setCollapsed(group, title, collapsed.has(key));

    const toggle = () => {
      // Read again: another group may have folded since this one was drawn.
      const current = loadCollapsed();
      const fold = !group.classList.contains("is-collapsed");
      if (fold) current.add(key);
      else current.delete(key);
      saveCollapsed(current);
      setCollapsed(group, title, fold);
    };
    title.addEventListener("click", toggle);
    title.addEventListener("keydown", (event) => {
      if (event.key !== "Enter" && event.key !== " ") return;
      event.preventDefault();
      toggle();
    });
  });
}
