import type { EditorView } from "@codemirror/view";
import { api } from "./api";
import { getSource } from "./editor";

/** Ctrl+click (Cmd+click on Mac) jumps to the symbol's declaration. */
export function initGotoDef(view: EditorView): void {
  view.dom.addEventListener("click", async (e: MouseEvent) => {
    if (!e.ctrlKey && !e.metaKey) return;
    e.preventDefault();

    const pos = view.posAtCoords({ x: e.clientX, y: e.clientY });
    if (pos === null) return;

    try {
      const target = await api("gotoDef", { source: getSource(), offset: pos });
      if (!target) return; // no declaration in the buffer (prelude symbol or none)
      view.dispatch({ selection: { anchor: target.offset }, scrollIntoView: true });
    } catch (err) {
      console.error("go to definition failed:", err);
    }
  });
}
