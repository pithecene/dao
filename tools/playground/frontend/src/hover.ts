import { EditorView } from "@codemirror/view";
import { api } from "./api";
import { getSource } from "./editor";
import type { Hover } from "./generated/tooling_surface";
import { escapeHtml } from "./util";

let hoverTooltip: HTMLElement | null = null;
let hoverSeq = 0;
let hoverTimer: ReturnType<typeof setTimeout> | null = null;

const HOVER_DEBOUNCE_MS = 100;

export function initHover(view: EditorView): void {
  const container = view.dom;

  container.addEventListener("mousemove", (e: MouseEvent) => {
    if (hoverTimer) clearTimeout(hoverTimer);
    hoverTimer = setTimeout(() => doHover(view, e), HOVER_DEBOUNCE_MS);
  });

  container.addEventListener("mouseleave", hideTooltip);
}

async function doHover(view: EditorView, e: MouseEvent): Promise<void> {
  const pos = view.posAtCoords({ x: e.clientX, y: e.clientY });
  if (pos === null) {
    hideTooltip();
    return;
  }

  const seq = ++hoverSeq;

  try {
    const data = await api("hover", { source: getSource(), offset: pos });
    if (seq !== hoverSeq) return; // a newer mouse position superseded this
    if (!data) {
      hideTooltip();
      return;
    }
    showTooltip(e.clientX, e.clientY, data);
  } catch {
    if (seq === hoverSeq) hideTooltip();
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
  hoverTooltip.style.left = `${x + 12}px`;
  hoverTooltip.style.top = `${y + 12}px`;
  hoverTooltip.style.display = "block";
}

function hideTooltip(): void {
  if (hoverTooltip) {
    hoverTooltip.style.display = "none";
  }
}
