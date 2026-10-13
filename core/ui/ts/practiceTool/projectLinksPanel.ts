/**
 * The Practice Tool's Links section: web pages kept with the selected saved
 * project — the tablature, a lesson video, the lyrics.
 *
 * Links belong to the saved project rather than to the loaded track, so they
 * are edited on the stored project directly and need no Save. The project bar
 * owns which project is selected and calls `renderPracticeToolProjectLinks`
 * with it on every render; this module only remembers the id it last drew.
 *
 * The rows are plain `<a href>`s: main.ts's document-level click handler sends
 * any http(s) anchor to the system browser instead of navigating the WebView.
 */

import { showConfirm } from "../dialogs.js";
import { appendLog } from "../logging.js";
import { showNotification } from "../notifications.js";
import { escapeHtml } from "../utils.js";
import {
  addPracticeToolProjectLink,
  describePracticeToolLink,
  findPracticeToolProject,
  MAX_PRACTICE_TOOL_PROJECT_LINKS,
  removePracticeToolProjectLink,
} from "./projects.js";

let shownProjectId: string | null = null;

function getTitleInput(): HTMLInputElement | null {
  return document.getElementById("practice-tool-link-title") as HTMLInputElement | null;
}

function getUrlInput(): HTMLInputElement | null {
  return document.getElementById("practice-tool-link-url") as HTMLInputElement | null;
}

export function renderPracticeToolProjectLinks(projectId: string | null): void {
  const list = document.getElementById("practice-tool-links-list");
  const hint = document.getElementById("practice-tool-links-hint");
  const addBtn = document.getElementById("practice-tool-link-add") as HTMLButtonElement | null;
  if (!list) {
    return;
  }

  const project = projectId ? findPracticeToolProject(projectId) : null;
  if ((project?.id ?? null) !== shownProjectId) {
    // A half-typed link was meant for the project that was showing.
    const titleInput = getTitleInput();
    const urlInput = getUrlInput();
    if (titleInput) titleInput.value = "";
    if (urlInput) urlInput.value = "";
  }
  shownProjectId = project?.id ?? null;
  const links = project?.links ?? [];

  const titleInput = getTitleInput();
  const urlInput = getUrlInput();
  if (titleInput) titleInput.disabled = !project;
  if (urlInput) urlInput.disabled = !project;
  if (addBtn) addBtn.disabled = !project || !(urlInput?.value.trim()) || links.length >= MAX_PRACTICE_TOOL_PROJECT_LINKS;

  if (hint) {
    hint.textContent = !project
      ? "Save the track as a project, or pick a saved one, to keep links with it — the tab, a lesson, the lyrics."
      : `Links are kept with "${project.name}" as soon as they're added, and open in your web browser.`;
  }

  if (!links.length) {
    list.innerHTML = project ? `<div class="equipment-library-empty">No links for this project yet.</div>` : "";
    return;
  }

  list.innerHTML = links
    .map((link) => `
      <div class="practice-tool-link-row" data-link-id="${escapeHtml(link.id)}">
        <a class="practice-tool-link" href="${escapeHtml(link.url)}" title="${escapeHtml(link.url)}">${escapeHtml(describePracticeToolLink(link))}</a>
        <span class="practice-tool-link-url">${escapeHtml(link.url)}</span>
        <button type="button" class="practice-tool-link-delete-btn" data-link-id="${escapeHtml(link.id)}" title="Remove link" aria-label="Remove link">✕</button>
      </div>
    `)
    .join("");
}

function addLink(): void {
  if (!shownProjectId) {
    return;
  }
  const titleInput = getTitleInput();
  const urlInput = getUrlInput();
  const url = urlInput?.value ?? "";
  if (!url.trim()) {
    urlInput?.focus();
    return;
  }
  const result = addPracticeToolProjectLink(shownProjectId, titleInput?.value ?? "", url);
  if (result === "invalid-url") {
    showNotification("Not a web address", "Paste a link that starts with https://, or a site address like songsterr.com/…");
    urlInput?.focus();
    return;
  }
  if (result === "too-many") {
    showNotification("Too many links", `A project can keep ${MAX_PRACTICE_TOOL_PROJECT_LINKS} links — remove one first`);
    return;
  }
  if (result === "no-project") {
    renderPracticeToolProjectLinks(null);
    return;
  }
  if (titleInput) titleInput.value = "";
  if (urlInput) urlInput.value = "";
  renderPracticeToolProjectLinks(shownProjectId);
  appendLog(`practice tool project link added → ${shownProjectId}`);
}

async function removeLink(linkId: string): Promise<void> {
  const projectId = shownProjectId;
  const link = projectId ? findPracticeToolProject(projectId)?.links?.find((entry) => entry.id === linkId) : null;
  if (!projectId || !link) {
    return;
  }
  if (!(await showConfirm(`Remove the link "${describePracticeToolLink(link)}" from this project?`, "Remove link"))) {
    return;
  }
  removePracticeToolProjectLink(projectId, linkId);
  renderPracticeToolProjectLinks(shownProjectId);
}

export function bindPracticeToolProjectLinkActions(): void {
  const list = document.getElementById("practice-tool-links-list");
  if (list && list.dataset.bound !== "true") {
    list.dataset.bound = "true";
    list.addEventListener("click", (event) => {
      const button = (event.target as Element | null)?.closest<HTMLButtonElement>(".practice-tool-link-delete-btn");
      if (button?.dataset.linkId) {
        void removeLink(button.dataset.linkId);
      }
    });
  }

  for (const input of [getTitleInput(), getUrlInput()]) {
    if (input && input.dataset.bound !== "true") {
      input.dataset.bound = "true";
      input.addEventListener("input", () => renderPracticeToolProjectLinks(shownProjectId));
      input.addEventListener("keydown", (event) => {
        if (event.key === "Enter") {
          addLink();
        }
      });
    }
  }

  const addBtn = document.getElementById("practice-tool-link-add") as HTMLButtonElement | null;
  if (addBtn && addBtn.dataset.bound !== "true") {
    addBtn.dataset.bound = "true";
    addBtn.addEventListener("click", () => addLink());
  }
}
