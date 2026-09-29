import { uiState } from "./state.js";
import { requestAppInfo } from "./bridge.js";
import { updateAppSetting } from "./appSettingsStore.js";
import { appendLog } from "./logging.js";
import { showNotification } from "./notifications.js";
import { getApiBaseUrl } from "./apiConfig.js";
import { refreshSettingsUpdateBanner } from "./settings.js";
import { escapeHtml } from "./utils.js";

const UPDATE_CHECK_ENABLED_SETTING = "app.updateCheckEnabled";
const INSTANCE_ID_SETTING = "app.instanceId";

let hasCheckedForUpdates = false;

export function triggerUpdateCheck(): void {
  if (hasCheckedForUpdates) return;
  hasCheckedForUpdates = true;

  // Request app info from backend first
  requestAppInfo();

  // Ensure instance ID exists
  let instanceId = uiState.appSettings[INSTANCE_ID_SETTING] as string | undefined;
  if (!instanceId) {
    instanceId = crypto.randomUUID();
    updateAppSetting(INSTANCE_ID_SETTING, instanceId);
  }

  const rawEnabled = uiState.appSettings[UPDATE_CHECK_ENABLED_SETTING];
  const updateCheckEnabled = rawEnabled === undefined ? true : (rawEnabled === true || rawEnabled === "true");
  
  if (updateCheckEnabled) {
    setTimeout(() => {
      void performUpdateCheck(instanceId!);
    }, 5000); // Delay check by 5 seconds to not block startup
  } else {
    console.log("[UpdateCheck] Update check is disabled in settings");
  }
}

async function performUpdateCheck(instanceId: string): Promise<void> {
  try {
    const os = uiState.environment?.os ?? "Unknown";
    const cpu = uiState.environment?.cpu ?? "x64";
    const isStandalone = uiState.environment?.standalone ?? false;
    const currentVersion = uiState.environment?.version?.trim();

    if (!currentVersion) {
      console.warn("[UpdateCheck] Skipping update check because the current version is unavailable");
      return;
    }

    const apiBase = getApiBaseUrl();
    const response = await fetch(`${apiBase}/app/updatecheck`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json"
      },
      body: JSON.stringify({
        current_version: currentVersion,
        os,
        cpu,
        is_standalone: isStandalone,
        instance_id: instanceId
      })
    });

    if (!response.ok) {
      throw new Error(`HTTP error! status: ${response.status}`);
    }

    const result = await response.json();
    
    if (result.ok && result.data) {
      if (isVersionNewer(result.data.latest_version, currentVersion)) {
        showUpdateAvailable(result.data);
      } else if (result.data.is_update_available) {
        console.info(
          `[UpdateCheck] Ignoring update notification because ${result.data.latest_version} is not newer than ${currentVersion}`,
        );
      }
    }
  } catch (error) {
    console.error("Failed to check for updates:", error);
    appendLog("Update check failed");
  }
}

interface UpdateCheckResult {
  is_update_available: boolean;
  latest_version: string;
  download_url: string;
  release_notes: string;
}

function toNumericSemver(version: string | null | undefined): [number, number, number] | null {
  if (typeof version !== "string") {
    return null;
  }

  const match = version.trim().match(/^[^\d]*(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:[-+].*)?$/);
  if (!match) {
    return null;
  }

  const major = Number.parseInt(match[1] ?? "0", 10);
  const minor = Number.parseInt(match[2] ?? "0", 10);
  const patch = Number.parseInt(match[3] ?? "0", 10);

  if (![major, minor, patch].every((value) => Number.isSafeInteger(value) && value >= 0)) {
    return null;
  }

  return [major, minor, patch];
}

function isVersionNewer(candidateVersion: string, currentVersion: string): boolean {
  const candidate = toNumericSemver(candidateVersion);
  const current = toNumericSemver(currentVersion);

  if (candidate === null || current === null) {
    console.warn("[UpdateCheck] Unable to compare app versions", {
      candidateVersion,
      currentVersion,
    });
    return false;
  }

  for (let index = 0; index < candidate.length; index += 1) {
    if (candidate[index] !== current[index]) {
      return candidate[index] > current[index];
    }
  }

  return false;
}

function showUpdateAvailable(data: UpdateCheckResult): void {
  // Store in UI state so settings panel can show the banner
  uiState.availableUpdate = {
    version: data.latest_version,
    downloadUrl: data.download_url,
    releaseNotes: data.release_notes ?? "",
  };
  refreshSettingsUpdateBanner();

  // Create badge in UI
  const settingsBtn = document.getElementById("footer-settings-btn");
  if (settingsBtn) {
    let badge = settingsBtn.querySelector(".update-badge") as HTMLElement | null;
    if (!badge) {
      badge = document.createElement("span");
      badge.className = "update-badge";
      badge.textContent = "1";
      badge.style.cssText = "position: absolute; top: -5px; right: -5px; background: var(--accent-color, #ff4444); color: white; border-radius: 50%; padding: 2px 6px; font-size: 10px; font-weight: bold; pointer-events: none;";
      settingsBtn.style.position = "relative";
      settingsBtn.appendChild(badge);
    }
    
    // Add click handler to show modal
    settingsBtn.addEventListener("click", () => {
      createUpdateModal(data);
    });
  }

  // Show notification
  showNotification("Update Available", `Version ${data.latest_version} is available.`);
}

function createUpdateModal(data: UpdateCheckResult): void {
  let modal = document.getElementById("update-modal");
  if (!modal) {
    modal = document.createElement("div");
    modal.id = "update-modal";
    modal.className = "modal-overlay";
    modal.style.display = "none";
    
    modal.innerHTML = `
      <div class="modal-content" style="max-width: 500px;">
        <div class="modal-header">
          <h2>Update Available</h2>
          <button class="icon-btn" id="update-modal-close">&times;</button>
        </div>
        <div class="modal-body">
          <p>A new version of Soundshed Guitar is available: <strong>${escapeHtml(data.latest_version)}</strong></p>
          <div class="release-notes" style="margin-top: 15px; max-height: 200px; overflow-y: auto; background: var(--bg-color-dark, rgba(0,0,0,0.1)); padding: 10px; border-radius: 4px; font-size: 0.9em;">
            ${renderMarkdown(data.release_notes || "No release notes provided.")}
          </div>
        </div>
        <div class="modal-footer">
          <button class="btn" id="update-modal-later">Later</button>
          <a href="${escapeHtml(toHttpUrl(data.download_url) ?? "#")}" target="_blank" class="btn primary" id="update-modal-download">Download Update</a>
        </div>
      </div>
    `;
    document.body.appendChild(modal);

    document.getElementById("update-modal-close")?.addEventListener("click", () => {
      modal!.style.display = "none";
    });
    document.getElementById("update-modal-later")?.addEventListener("click", () => {
      modal!.style.display = "none";
    });
    document.getElementById("update-modal-download")?.addEventListener("click", () => {
      modal!.style.display = "none";
    });
  }

  // Show the modal
  modal.style.display = "flex";
}

/** The URL when it is an absolute http(s) one, else null: a link can never be `javascript:`. */
export function toHttpUrl(value: unknown): string | null {
  if (typeof value !== "string") {
    return null;
  }
  try {
    const url = new URL(value.trim());
    return url.protocol === "https:" || url.protocol === "http:" ? url.toString() : null;
  } catch {
    return null;
  }
}

function renderEmphasis(escaped: string): string {
  return escaped
    .replace(/\*\*(.*)\*\*/g, "<strong>$1</strong>")
    .replace(/\*(.*)\*/g, "<em>$1</em>");
}

/**
 * One line of release notes: text is escaped before any tag is added, and a link
 * or image keeps its markup only when its target is an http(s) URL.
 */
function renderMarkdownInline(line: string): string {
  let html = "";
  let last = 0;
  const pattern = /(!?)\[([^\]]*)\]\(([^)\s]*)\)/g;
  for (let match = pattern.exec(line); match; match = pattern.exec(line)) {
    html += renderEmphasis(escapeHtml(line.slice(last, match.index)));
    const [, bang, label, target] = match;
    const url = toHttpUrl(target);
    if (!url) {
      html += renderEmphasis(escapeHtml(label));
    } else if (bang) {
      html += `<img alt="${escapeHtml(label)}" src="${escapeHtml(url)}" />`;
    } else {
      html += `<a href="${escapeHtml(url)}" target="_blank" rel="noopener noreferrer">${renderEmphasis(escapeHtml(label))}</a>`;
    }
    last = match.index + match[0].length;
  }
  return html + renderEmphasis(escapeHtml(line.slice(last)));
}

/** Very basic markdown for release notes, which arrive from the update server. */
export function renderMarkdown(text: string): string {
  return text
    .split(/\r?\n/)
    .map((line) => {
      const heading = /^(#{1,3}) (.*)$/.exec(line);
      if (heading) {
        const level = heading[1].length;
        return `<h${level}>${renderMarkdownInline(heading[2])}</h${level}>`;
      }
      const quote = /^> (.*)$/.exec(line);
      return quote ? `<blockquote>${renderMarkdownInline(quote[1])}</blockquote>` : renderMarkdownInline(line);
    })
    .join("\n")
    .replace(/\n$/gm, "<br />");
}
