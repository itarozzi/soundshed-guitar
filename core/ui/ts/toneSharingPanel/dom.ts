/**
 * The three DOM helpers every part of the panel reaches for.
 */

export function element<T extends HTMLElement>(id: string): T | null {
  return document.getElementById(id) as T | null;
}

export function setText(id: string, value: string): void {
  const target = element<HTMLElement>(id);
  if (target) {
    target.textContent = value;
  }
}

export function setUploadStatus(value: string): void {
  setText("tone-sharing-upload-status", value);
  setText("tone-sharing-publish-modal-status", value);
}
