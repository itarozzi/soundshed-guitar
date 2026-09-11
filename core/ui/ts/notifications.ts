const notificationElement = document.getElementById("notification-area");
let _dismissTimer: ReturnType<typeof setTimeout> | null = null;
const SEVERITY_WORDS = new Set(["success", "warning", "error", "info"]);

export function clearNotification(): void {
  if (!notificationElement) return;
  if (_dismissTimer !== null) {
    clearTimeout(_dismissTimer);
    _dismissTimer = null;
  }
  notificationElement.textContent = "";
  notificationElement.classList.remove("visible");
}

/**
 * Show the toast for a few seconds. `detail` is appended to the message after a
 * colon ("Failed to apply preset: disk full") — it is extra information for the
 * user, not a severity. The toast has no severity styling; a message should read
 * as a success or a failure on its own ("Layout saved", "Import failed: …").
 *
 * A call with nothing to say — both arguments empty, which is what an absent
 * payload field amounts to — shows no toast at all rather than an empty one,
 * and leaves whatever is already on screen alone. Use `clearNotification()` to
 * take a toast down early.
 */
export function showNotification(message: string, detail = ""): void {
  if (!notificationElement) return;
  if (SEVERITY_WORDS.has(detail.trim().toLowerCase())) {
    // Would render as "Layout saved: success". Treat it as the mistake it is.
    console.warn(`[notification] showNotification("${message}", "${detail}"): the second argument is a detail string, not a severity`);
    detail = "";
  }
  const trimmedMessage = (message ?? "").trim();
  const trimmedDetail = (detail ?? "").trim();
  // A detail on its own still reads fine; a bare colon in front of it does not.
  const resolvedMessage = trimmedMessage && trimmedDetail
    ? `${trimmedMessage}: ${trimmedDetail}`
    : trimmedMessage || trimmedDetail;
  if (!resolvedMessage) {
    return;
  }
  if (_dismissTimer !== null) {
    clearTimeout(_dismissTimer);
    _dismissTimer = null;
  }
  notificationElement.textContent = resolvedMessage;
  notificationElement.classList.add("visible");
  _dismissTimer = setTimeout(() => {
    clearNotification();
  }, 4000);
}
