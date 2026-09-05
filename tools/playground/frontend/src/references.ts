import { StateEffect, StateField } from "@codemirror/state";
import { Decoration, type DecorationSet, EditorView } from "@codemirror/view";
import { api } from "./api";
import { getSource } from "./editor";

// Alt+click a symbol to highlight every reference to it in the buffer.
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
      const marks = refs
        .filter((r) => r.length > 0 && r.offset + r.length <= docLength)
        .map((r) =>
          (r.isDefinition ? definitionMark : useMark).range(
            r.offset,
            r.offset + r.length,
          ),
        );
      view.dispatch({ effects: setReferences.of(Decoration.set(marks, true)) });
    } catch (err) {
      console.error("references failed:", err);
    }
  });
}
