import type { EditorView } from "@codemirror/view";
import { api } from "./api";
import { inDocument, programRequest } from "./document";
import { getSource } from "./editor";
import { showNotice } from "./notice";

/**
 * Ctrl+click (Cmd+click on Mac) jumps to the symbol's declaration.  A
 * declaration in another file of the program (the prelude) is named
 * rather than opened: the UI shows one document.
 */
export function initGotoDef(view: EditorView): void {
  view.dom.addEventListener("click", async (e: MouseEvent) => {
    if (!e.ctrlKey && !e.metaKey) return;
    e.preventDefault();

    const pos = view.posAtCoords({ x: e.clientX, y: e.clientY });
    if (pos === null) return;

    try {
      const target = await api("gotoDef", { ...programRequest(getSource()), offset: pos });
      if (!target) return;
      if (!inDocument(target.file)) {
        showNotice(`defined in ${target.file}:${target.line}:${target.col}`);
        return;
      }
      view.dispatch({ selection: { anchor: target.offset }, scrollIntoView: true });
    } catch (err) {
      console.error("go to definition failed:", err);
    }
  });
}
