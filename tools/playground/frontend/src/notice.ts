const NOTICE_MS = 4000;
let hideTimer: ReturnType<typeof setTimeout> | null = null;

/** Show a short transient message in the header (navigation results). */
export function showNotice(text: string): void {
  const el = document.getElementById("nav-notice")!;
  el.textContent = text;
  el.hidden = false;
  if (hideTimer) clearTimeout(hideTimer);
  hideTimer = setTimeout(() => {
    el.hidden = true;
  }, NOTICE_MS);
}
