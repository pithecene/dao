import { StateEffect, StateField } from "@codemirror/state";
import {
  Decoration,
  type DecorationSet,
  EditorView,
  ViewPlugin,
  type ViewUpdate,
} from "@codemirror/view";
import {
  TOKEN_GROUP,
  type SemanticToken,
  type TokenKind,
} from "./generated/tooling_surface";

let currentTokens: SemanticToken[] = [];

const setReferencesEffect = StateEffect.define<DecorationSet>();
const useMark = Decoration.mark({ class: "dao-reference" });
const definitionMark = Decoration.mark({ class: "dao-reference-def" });

/** CodeMirror state field for symbol reference highlights (Alt+Click / Shift+F12). */
export const referenceHighlights = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(value, tr) {
    if (tr.docChanged) return Decoration.none;
    for (const effect of tr.effects) {
      if (effect.is(setReferencesEffect)) return effect.value;
    }
    return value;
  },
  provide: (field) => EditorView.decorations.from(field),
});

export function setReferenceMarks(
  view: EditorView,
  spans: { from: number; to: number; isDefinition: boolean }[],
): void {
  const marks = spans
    .map((s) => (s.isDefinition ? definitionMark : useMark).range(s.from, s.to))
    .sort((a, b) => a.from - b.from || a.to - b.to);
  view.dispatch({ effects: setReferencesEffect.of(Decoration.set(marks, true)) });
}

/**
 * Replace the current token set and force a decoration rebuild.
 */
export function setTokens(tokens: SemanticToken[], view: EditorView): void {
  currentTokens = tokens;
  view.dispatch({});
}

function cssClass(kind: string): string | null {
  const group = TOKEN_GROUP[kind as TokenKind];
  return group ? `dao-${group}` : null;
}

function buildDecorations(view: EditorView) {
  const decorations = [];
  const docLength = view.state.doc.length;

  for (const tok of currentTokens) {
    const from = tok.offset;
    const to = tok.offset + tok.length;
    if (from >= docLength || to > docLength || from >= to) continue;

    const cls = cssClass(tok.kind);
    if (!cls) continue;

    decorations.push(Decoration.mark({ class: cls }).range(from, to));
  }

  return Decoration.set(decorations, true);
}

/** CodeMirror plugin that applies semantic token decorations. */
export const tokenHighlighter = ViewPlugin.fromClass(
  class {
    decorations = Decoration.none;

    constructor(view: EditorView) {
      this.decorations = buildDecorations(view);
    }

    update(vu: ViewUpdate) {
      this.decorations = buildDecorations(vu.view);
    }
  },
  {
    decorations: (v) => v.decorations,
  },
);
