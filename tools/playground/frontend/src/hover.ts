import { isActivePrelude, programRequest } from "./document";
import { EditorView } from "@codemirror/view";
import { api } from "./api";
import { getSource } from "./editor";
import type { Hover } from "./generated/tooling_surface";
import { escapeHtml } from "./util";

let hoverTooltip: HTMLElement | null = null;
let hoverSeq = 0;
let hoverTimer: ReturnType<typeof setTimeout> | null = null;

const HOVER_DEBOUNCE_MS = 110;

export function initHover(view: EditorView): void {
  const container = view.dom;

  container.addEventListener("mousemove", (e: MouseEvent) => {
    if (hoverTimer) clearTimeout(hoverTimer);
    hoverTimer = setTimeout(() => void doHover(view, e), HOVER_DEBOUNCE_MS);
  });

  container.addEventListener("mouseleave", hideHoverTooltip);
}

async function doHover(view: EditorView, e: MouseEvent): Promise<void> {
  if (isActivePrelude()) {
    hideHoverTooltip();
    return;
  }

  const pos = view.posAtCoords({ x: e.clientX, y: e.clientY });
  if (pos === null) {
    hideHoverTooltip();
    return;
  }

  const seq = ++hoverSeq;

  try {
    const data = await api("hover", {
      ...programRequest(getSource()),
      offset: pos,
    });
    if (seq !== hoverSeq) return;
    if (!data) {
      hideHoverTooltip();
      return;
    }
    showTooltip(e.clientX, e.clientY, data);
  } catch {
    if (seq === hoverSeq) hideHoverTooltip();
  }
}

function showTooltip(x: number, y: number, data: Hover): void {
  if (!hoverTooltip) {
    hoverTooltip = document.createElement("div");
    hoverTooltip.className = "dao-hover-tooltip";
    document.body.appendChild(hoverTooltip);
  }

  let content = `<span class="hover-kind">${escapeHtml(data.kind)}</span> <strong>${escapeHtml(data.name)}</strong>`;
  if (data.type) {
    content += `<br><span class="hover-type">${escapeHtml(data.type)}</span>`;
  }

  hoverTooltip.innerHTML = content;
  const maxLeft = Math.max(8, window.innerWidth - 430);
  hoverTooltip.style.left = `${Math.min(x + 12, maxLeft)}px`;
  hoverTooltip.style.top = `${y + 14}px`;
  hoverTooltip.style.display = "block";
}

export function hideHoverTooltip(): void {
  if (hoverTooltip) {
    hoverTooltip.style.display = "none";
  }
}
