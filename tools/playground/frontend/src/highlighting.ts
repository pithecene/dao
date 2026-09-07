import {
  Decoration,
  EditorView,
  ViewPlugin,
  type ViewUpdate,
} from "@codemirror/view";
import {
  TOKEN_GROUP,
  type SemanticToken,
  type TokenKind,
} from "./generated/tooling_surface";

// Module-private token state. Updated by setTokens(), read by the plugin.
let currentTokens: SemanticToken[] = [];

/**
 * Replace the current token set and force a decoration rebuild.
 * Call this after receiving new semantic tokens from the analyze route.
 */
export function setTokens(tokens: SemanticToken[], view: EditorView): void {
  currentTokens = tokens;
  // Force decoration rebuild by dispatching an empty transaction.
  view.dispatch({});
}

/**
 * CSS class of a token kind: `dao-<group>` from the generated surface.
 * A kind this build of the frontend does not know (a newer service)
 * is left unstyled rather than mis-styled.
 */
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
