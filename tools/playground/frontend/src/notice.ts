const NOTICE_MS = 4000;
let hideTimer: ReturnType<typeof setTimeout> | null = null;

/** Show a short message for a few seconds, in the screen's bottom corner. */
export function showNotice(text: string): void {
  if (typeof document === "undefined") return;
  const el = document.getElementById("nav-notice");
  if (!el) return;
  el.textContent = text;
  el.hidden = false;
  if (hideTimer) clearTimeout(hideTimer);
  hideTimer = setTimeout(() => {
    el.hidden = true;
  }, NOTICE_MS);
}
