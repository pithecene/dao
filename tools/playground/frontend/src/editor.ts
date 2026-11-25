import { basicSetup } from "codemirror";
import {
  Annotation,
  Compartment,
  EditorState,
  StateEffect,
  StateField,
} from "@codemirror/state";
import { Decoration, type DecorationSet, EditorView } from "@codemirror/view";
import { referenceHighlights, tokenHighlighter } from "./highlighting";
import { daoCompletions } from "./completions";
import type { Diagnostic } from "./generated/tooling_surface";
import { inDocument } from "./document";

let editorInstance: EditorView | null = null;
const readOnlyCompartment = new Compartment();
const programmaticChange = Annotation.define<boolean>();
const cursorByPath = new Map<string, number>();
let trackedPath = "";

const setDiagDecorations = StateEffect.define<DecorationSet>();
const errorDiagMark = Decoration.mark({ class: "dao-diag-mark-error" });
const warnDiagMark = Decoration.mark({ class: "dao-diag-mark-warning" });

const diagnosticHighlights = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(value, tr) {
    if (tr.docChanged) return Decoration.none;
    for (const effect of tr.effects) {
      if (effect.is(setDiagDecorations)) return effect.value;
    }
    return value;
  },
  provide: (field) => EditorView.decorations.from(field),
});

/** Shared Pithecene token-driven CodeMirror theme (adapts to dark & light modes). */
const pitheceneEditorTheme = EditorView.theme({
  "&": {
    backgroundColor: "var(--editor-bg)",
    color: "var(--fg-primary)",
    height: "100%",
  },
  ".cm-content": {
    caretColor: "var(--editor-cursor)",
    padding: "12px 0",
    fontFamily: "var(--font-mono)",
  },
  ".cm-line": {
    padding: "0 14px",
  },
  ".cm-cursor, .cm-dropCursor": {
    borderLeftColor: "var(--editor-cursor)",
  },
  "&.cm-focused > .cm-scroller > .cm-selectionLayer .cm-selectionBackground, .cm-selectionBackground, .cm-content ::selection":
    {
      backgroundColor: "var(--editor-selection)",
    },
  ".cm-activeLine": {
    backgroundColor: "var(--editor-active-line)",
  },
  ".cm-gutters": {
    backgroundColor: "var(--editor-gutter-bg)",
    color: "var(--editor-gutter-fg)",
    borderRight: "1px solid var(--border-hairline)",
    fontFamily: "var(--font-mono)",
    fontSize: "12px",
  },
  ".cm-activeLineGutter": {
    backgroundColor: "var(--editor-active-line)",
    color: "var(--editor-gutter-active-fg)",
  },
  ".cm-foldPlaceholder": {
    backgroundColor: "var(--surface-elevated)",
    border: "1px solid var(--border-strong)",
    color: "var(--fg-secondary)",
  },
  ".cm-tooltip": {
    backgroundColor: "var(--surface-elevated)",
    border: "1px solid var(--border-strong)",
    color: "var(--fg-primary)",
    borderRadius: "var(--radius-md)",
    boxShadow: "var(--shadow-popover)",
  },
  ".cm-tooltip-autocomplete > ul": {
    fontFamily: "var(--font-mono)",
    fontSize: "12.5px",
  },
  ".cm-tooltip-autocomplete ul li[aria-selected]": {
    backgroundColor: "var(--accent-subtle)",
    color: "var(--fg-primary)",
  },
  ".cm-completionDetail": {
    color: "var(--fg-muted)",
    fontStyle: "normal",
    marginLeft: "10px",
  },
  ".cm-panels": {
    backgroundColor: "var(--surface-panel)",
    color: "var(--fg-primary)",
  },
  ".cm-searchMatch": {
    backgroundColor: "var(--status-warn-subtle)",
    outline: "1px solid var(--syntax-resource-border)",
  },
});

/**
 * Create and mount the CodeMirror editor.
 * @param onUserDocChange Called only when the user edits an editable document.
 */
export function createEditor(onUserDocChange: (source: string) => void): EditorView {
  const view = new EditorView({
    state: EditorState.create({
      doc: "",
      extensions: [
        basicSetup,
        pitheceneEditorTheme,
        readOnlyCompartment.of(EditorState.readOnly.of(false)),
        tokenHighlighter,
        daoCompletions,
        referenceHighlights,
        diagnosticHighlights,
        EditorView.updateListener.of((update) => {
          if (
            update.docChanged &&
            !update.transactions.some((tr) => tr.annotation(programmaticChange))
          ) {
            onUserDocChange(update.state.doc.toString());
          }
        }),
      ],
    }),
    parent: document.getElementById("editor-container")!,
  });
  editorInstance = view;
  return view;
}

export function getEditorView(): EditorView | null {
  return editorInstance;
}

/** Get the current editor buffer source. */
export function getSource(): string {
  return editorInstance?.state.doc.toString() ?? "";
}

/** Replace the editor buffer programmatically (switching tabs, loading examples). */
export function loadEditorBuffer(
  path: string,
  source: string,
  readOnly: boolean,
  selection?: { offset: number; length?: number },
): void {
  if (!editorInstance) return;

  if (trackedPath) {
    cursorByPath.set(trackedPath, editorInstance.state.selection.main.head);
  }
  trackedPath = path;

  const savedPos = cursorByPath.get(path) ?? 0;
  const anchor = Math.max(
    0,
    Math.min(selection ? selection.offset : savedPos, source.length),
  );
  const head = Math.max(
    anchor,
    Math.min(
      selection && selection.length ? anchor + selection.length : anchor,
      source.length,
    ),
  );

  editorInstance.dispatch({
    changes: {
      from: 0,
      to: editorInstance.state.doc.length,
      insert: source,
    },
    selection: { anchor, head },
    effects: [
      readOnlyCompartment.reconfigure(EditorState.readOnly.of(readOnly)),
      EditorView.scrollIntoView(anchor, { y: "center" }),
    ],
    annotations: programmaticChange.of(true),
  });
}

/** Jump cursor/selection inside the currently displayed buffer. */
export function jumpInEditor(offset: number, length = 0): void {
  if (!editorInstance) return;
  const docLen = editorInstance.state.doc.length;
  const anchor = Math.max(0, Math.min(offset, docLen));
  const head = Math.max(anchor, Math.min(anchor + Math.max(0, length), docLen));
  editorInstance.dispatch({
    selection: { anchor, head },
    effects: EditorView.scrollIntoView(anchor, { y: "center" }),
  });
  editorInstance.focus();
}

/** Underline located diagnostics belonging to the currently displayed buffer. */
export function setEditorDiagnostics(
  diagnostics: readonly Diagnostic[],
  view: EditorView,
): void {
  const docLength = view.state.doc.length;
  const ranges = [];
  for (const d of diagnostics) {
    if (!d.file || !inDocument(d.file)) continue;
    const from = Math.max(0, Math.min(d.offset, docLength));
    const to = Math.max(
      from,
      Math.min(d.offset + Math.max(1, d.length), docLength),
    );
    if (from < to) {
      const mark = d.severity === "warning" ? warnDiagMark : errorDiagMark;
      ranges.push(mark.range(from, to));
    }
  }
  ranges.sort((a, b) => a.from - b.from || a.to - b.to);
  view.dispatch({
    effects: setDiagDecorations.of(Decoration.set(ranges, true)),
    annotations: programmaticChange.of(true),
  });
}
