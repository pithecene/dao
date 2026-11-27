import { basicSetup } from "codemirror";
import { indentWithTab } from "@codemirror/commands";
import {
  Annotation,
  Compartment,
  EditorState,
  StateEffect,
  StateField,
  Transaction,
} from "@codemirror/state";
import { Decoration, type DecorationSet, EditorView, keymap } from "@codemirror/view";
import { referenceHighlights, tokenHighlighter } from "./highlighting";
import { daoCompletions } from "./completions";
import type { Diagnostic } from "./generated/tooling_surface";
import { inDocument, onWorkspaceLifecycle } from "./document";

let editorInstance: EditorView | null = null;
let onUserEditCallback: ((source: string) => void) | null = null;

const readOnlyCompartment = new Compartment();
const programmaticChange = Annotation.define<boolean>();
const statesByPath = new Map<string, EditorState>();
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

function createTabState(doc: string, readOnly: boolean): EditorState {
  return EditorState.create({
    doc,
    extensions: [
      basicSetup,
      // Tab indents, so it no longer moves focus on its own.  Escape and
      // then Tab does, which the editor provides; the description tells a
      // screen reader so, and the hint bar tells a sighted reader.
      keymap.of([indentWithTab]),
      EditorView.contentAttributes.of({
        "aria-description": "Press Escape, then Tab, to leave the editor.",
      }),
      pitheceneEditorTheme,
      readOnlyCompartment.of(EditorState.readOnly.of(readOnly)),
      tokenHighlighter,
      daoCompletions,
      referenceHighlights,
      diagnosticHighlights,
      EditorView.updateListener.of((update) => {
        if (trackedPath) {
          statesByPath.set(trackedPath, update.state);
        }
        if (
          update.docChanged &&
          !update.transactions.some((tr) => tr.annotation(programmaticChange))
        ) {
          onUserEditCallback?.(update.state.doc.toString());
        }
      }),
    ],
  });
}

/**
 * Create and mount the CodeMirror editor with isolated per-file EditorState
 * instances (preserving independent undo/redo history and selection per tab).
 */
export function createEditor(
  onUserDocChange: (source: string) => void,
): EditorView {
  onUserEditCallback = onUserDocChange;

  onWorkspaceLifecycle({
    onReset: () => {
      statesByPath.clear();
      trackedPath = "";
    },
    onDelete: (path) => {
      statesByPath.delete(path);
      if (trackedPath === path) {
        trackedPath = "";
      }
    },
    onRename: (oldPath, newPath) => {
      const existing = statesByPath.get(oldPath);
      if (existing) {
        statesByPath.delete(oldPath);
        statesByPath.set(newPath, existing);
      }
      if (trackedPath === oldPath) {
        trackedPath = newPath;
      }
    },
  });

  const view = new EditorView({
    state: createTabState("", false),
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

/**
 * Activate the isolated EditorState for `path` (creating one when first opened
 * or if `source` changed externally), preserving per-file undo history.
 */
export function loadEditorBuffer(
  path: string,
  source: string,
  readOnly: boolean,
  selection?: { offset: number; length?: number },
): void {
  if (!editorInstance) return;

  // The completion extension outlives `setState`: a query still running for
  // the outgoing buffer would deliver its options, or its failure, into the
  // incoming one.  It aborts its queries and closes its popup on any
  // transaction that sets the selection, so the outgoing buffer gets one
  // that sets the selection it already has.
  editorInstance.dispatch({
    selection: editorInstance.state.selection,
    annotations: [programmaticChange.of(true), Transaction.addToHistory.of(false)],
  });

  if (trackedPath && statesByPath.has(trackedPath)) {
    statesByPath.set(trackedPath, editorInstance.state);
  }

  let nextState = statesByPath.get(path);
  if (!nextState || nextState.doc.toString() !== source) {
    nextState = createTabState(source, readOnly);
  }

  trackedPath = path;
  statesByPath.set(path, nextState);
  editorInstance.setState(nextState);

  editorInstance.dispatch({
    effects: readOnlyCompartment.reconfigure(EditorState.readOnly.of(readOnly)),
    annotations: [programmaticChange.of(true), Transaction.addToHistory.of(false)],
  });

  if (selection) {
    const anchor = Math.max(0, Math.min(selection.offset, source.length));
    const head = Math.max(
      anchor,
      Math.min(
        selection.length ? anchor + selection.length : anchor,
        source.length,
      ),
    );
    editorInstance.dispatch({
      selection: { anchor, head },
      effects: EditorView.scrollIntoView(anchor, { y: "center" }),
      annotations: [programmaticChange.of(true), Transaction.addToHistory.of(false)],
    });
  }
  statesByPath.set(path, editorInstance.state);
}

/**
 * Build an undoable whole-buffer replacement transaction on `state` when
 * writable and still matching `expectedSource`, clamping selection bounds.
 */
export function buildUndoableReplaceTransaction(
  state: EditorState,
  expectedSource: string,
  nextSource: string,
): Transaction | null {
  if (
    state.readOnly ||
    state.doc.toString() !== expectedSource ||
    expectedSource === nextSource
  ) {
    return null;
  }
  const sel = state.selection.main;
  const anchor = Math.min(sel.anchor, nextSource.length);
  const head = Math.min(sel.head, nextSource.length);
  return state.update({
    changes: { from: 0, to: expectedSource.length, insert: nextSource },
    selection: { anchor, head },
  });
}

/**
 * Apply an undoable replacement (such as formatting) to the currently tracked
 * writable file's EditorState when `path` is active and its buffer still equals
 * `expectedSource`. Preserves undo history and triggers user-edit listeners.
 */
export function applyUndoableEdit(
  path: string,
  expectedSource: string,
  nextSource: string,
): boolean {
  if (!editorInstance || trackedPath !== path) return false;
  const tr = buildUndoableReplaceTransaction(
    editorInstance.state,
    expectedSource,
    nextSource,
  );
  if (!tr) return false;
  editorInstance.dispatch(tr);
  return true;
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
    annotations: [programmaticChange.of(true), Transaction.addToHistory.of(false)],
  });
}
