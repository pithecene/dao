import { basicSetup } from "codemirror";
import { EditorState } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { oneDark } from "@codemirror/theme-one-dark";
import { tokenHighlighter } from "./highlighting";
import { daoCompletions } from "./completions";
import { referenceHighlights } from "./references";

let editorInstance: EditorView | null = null;

/**
 * Create and mount the CodeMirror editor.
 * @param onDocChange Called whenever the document content changes.
 */
export function createEditor(onDocChange: () => void): EditorView {
  const view = new EditorView({
    state: EditorState.create({
      doc: "",
      extensions: [
        basicSetup,
        oneDark,
        tokenHighlighter,
        daoCompletions,
        referenceHighlights,
        EditorView.updateListener.of((update) => {
          if (update.docChanged) {
            onDocChange();
          }
        }),
      ],
    }),
    parent: document.getElementById("editor-container")!,
  });
  editorInstance = view;
  return view;
}

/** Get the current document source. */
export function getSource(): string {
  return editorInstance?.state.doc.toString() ?? "";
}

/** Replace the entire editor content. */
export function setSource(source: string): void {
  if (!editorInstance) return;
  editorInstance.dispatch({
    changes: {
      from: 0,
      to: editorInstance.state.doc.length,
      insert: source,
    },
  });
}
