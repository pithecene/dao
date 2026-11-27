import type { EditorView } from "@codemirror/view";
import { api } from "./api";
import { isActivePrelude, programRequest } from "./document";
import { getSource } from "./editor";
import { showNotice } from "./notice";
import { navigateToPosition } from "./workspace_ui";

let boundView: EditorView | null = null;

/**
 * Ctrl+click (Cmd+click on Mac) or F12 jumps to a symbol's declaration,
 * opening sibling workspace files or read-only stdlib prelude files.
 */
export function initGotoDef(view: EditorView): void {
  boundView = view;
  // macOS turns Ctrl+click into the context menu, so the click that reaches
  // this handler there is Cmd+click.
  if (/Mac|iPhone|iPad/.test(navigator.platform)) {
    const hint = document.getElementById("goto-def-click-hint");
    if (hint) hint.textContent = "⌘+Click";
  }
  view.dom.addEventListener("click", (e: MouseEvent) => {
    if (!e.ctrlKey && !e.metaKey) return;
    e.preventDefault();

    const pos = view.posAtCoords({ x: e.clientX, y: e.clientY });
    if (pos === null) return;
    void gotoDefAtOffset(pos);
  });
}

export async function gotoDefAtCursor(): Promise<void> {
  if (!boundView) return;
  await gotoDefAtOffset(boundView.state.selection.main.head);
}

async function gotoDefAtOffset(offset: number): Promise<void> {
  if (isActivePrelude()) {
    showNotice("Viewing read-only prelude file");
    return;
  }
  try {
    const target = await api("gotoDef", {
      ...programRequest(getSource()),
      offset,
    });
    if (!target) {
      showNotice("No definition at cursor");
      return;
    }
    await navigateToPosition(target.file, target.offset, target.length);
  } catch (err) {
    console.error("go to definition failed:", err);
  }
}
