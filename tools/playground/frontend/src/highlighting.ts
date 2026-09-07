import {
  Decoration,
  EditorView,
  ViewPlugin,
  type ViewUpdate,
} from "@codemirror/view";
import type { SemanticToken } from "./types";

// Module-private token state. Updated by setTokens(), read by the plugin.
let currentTokens: SemanticToken[] = [];

/**
 * Replace the current token set and force a decoration rebuild.
 * Call this after receiving new semantic tokens from /api/analyze.
 */
export function setTokens(tokens: SemanticToken[], view: EditorView): void {
  currentTokens = tokens;
  // Force decoration rebuild by dispatching an empty transaction.
  view.dispatch({});
}

/**
 * Map semantic token kinds from CONTRACT_LANGUAGE_TOOLING.md to CSS classes.
 * Prefix groups share a class so highlighting degrades gracefully.
 */
function semanticKindToClass(kind: string): string | null {
  if (kind.startsWith("keyword.")) return "dao-keyword";
  if (kind.startsWith("decl.variable.")) return "dao-variable";
  if (kind === "decl.module" || kind === "use.module") return "dao-module";
  if (kind === "decl.field" || kind === "use.field" || kind === "use.variant") return "dao-field";
  if (kind.startsWith("decl.")) return "dao-decl";
  if (kind.startsWith("type.") || kind === "use.type") return "dao-type";
  if (kind.startsWith("use.variable.")) return "dao-variable";
  if (kind === "use.function") return "dao-decl";
  if (kind.startsWith("mode.")) return "dao-mode";
  if (kind.startsWith("resource.")) return "dao-resource";
  if (kind === "lambda.param") return "dao-lambda-param";
  if (kind === "literal.number" || kind === "literal.bool") return "dao-literal-number";
  if (kind === "literal.string") return "dao-literal-string";
  if (kind.startsWith("operator.")) return "dao-operator";
  if (kind === "punctuation") return "dao-punctuation";
  return null;
}

function buildDecorations(view: EditorView) {
  const decorations = [];
  const docLength = view.state.doc.length;

  for (const tok of currentTokens) {
    const from = tok.offset;
    const to = tok.offset + tok.length;
    if (from >= docLength || to > docLength || from >= to) continue;

    const cls = semanticKindToClass(tok.kind);
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
