import { StateEffect, StateField } from "@codemirror/state";
import { Decoration, type DecorationSet, EditorView } from "@codemirror/view";
import { api } from "./api";
import { inDocument } from "./document";
import { getSource } from "./editor";
import { showNotice } from "./notice";

// Alt+click a symbol to highlight every reference to it in the buffer.
// References in other files of the program are counted in a notice.
// The highlights clear on the next edit.

const setReferences = StateEffect.define<DecorationSet>();

const useMark = Decoration.mark({ class: "dao-reference" });
const definitionMark = Decoration.mark({ class: "dao-reference-def" });

export const referenceHighlights = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(value, tr) {
    if (tr.docChanged) return Decoration.none;
    for (const effect of tr.effects) {
      if (effect.is(setReferences)) return effect.value;
    }
    return value;
  },
  provide: (field) => EditorView.decorations.from(field),
});

export function initReferences(view: EditorView): void {
  view.dom.addEventListener("click", async (e: MouseEvent) => {
    if (!e.altKey) return;
    e.preventDefault();

    const pos = view.posAtCoords({ x: e.clientX, y: e.clientY });
    if (pos === null) return;

    try {
      const refs = await api("references", { source: getSource(), offset: pos });
      const docLength = view.state.doc.length;
      const here = refs.filter(
        (r) => inDocument(r.file) && r.length > 0 && r.offset + r.length <= docLength,
      );
      const elsewhere = refs.filter((r) => !inDocument(r.file));

      const marks = here.map((r) =>
        (r.isDefinition ? definitionMark : useMark).range(r.offset, r.offset + r.length),
      );
      view.dispatch({ effects: setReferences.of(Decoration.set(marks, true)) });

      if (refs.length === 0) {
        showNotice("no references");
      } else if (elsewhere.length > 0) {
        const files = [...new Set(elsewhere.map((r) => r.file))].join(", ");
        showNotice(`${here.length} here, ${elsewhere.length} in ${files}`);
      }
    } catch (err) {
      console.error("references failed:", err);
    }
  });
}
