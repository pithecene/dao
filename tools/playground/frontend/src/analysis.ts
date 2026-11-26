import type { EditorView } from "@codemirror/view";
import { api } from "./api";
import { setTokens } from "./highlighting";
import { renderDiagnostics } from "./diagnostics";
import { renderOutline } from "./outline";
import {
  getActivePath,
  getActivePreludeTab,
  getWorkspaceDiagnostics,
  isActivePrelude,
  programRequest,
  setDocument,
  setWorkspaceDiagnostics,
} from "./document";
import { getSource, setEditorDiagnostics } from "./editor";

let analyzeTimer: ReturnType<typeof setTimeout> | null = null;
let analyzeSeq = 0;
let editorView: EditorView | null = null;

const DEBOUNCE_MS = 260;

/** Bind the analysis module to the editor view. */
export function initAnalysis(view: EditorView): void {
  editorView = view;
  document
    .getElementById("include-prelude")!
    .addEventListener("change", () => void doAnalyze());
}

/** Schedule a debounced analysis request. */
export function scheduleAnalyze(): void {
  if (analyzeTimer) clearTimeout(analyzeTimer);
  analyzeTimer = setTimeout(() => void doAnalyze(), DEBOUNCE_MS);
}

function includePrelude(): boolean {
  const box = document.getElementById("include-prelude") as HTMLInputElement;
  return box.checked;
}

/** Run analysis immediately: tokens, diagnostics, IR panels, outline. */
export async function doAnalyze(): Promise<void> {
  if (!editorView) return;

  // Read-only prelude tabs already carry their classified semantic tokens and symbols.
  if (isActivePrelude()) {
    const tab = getActivePreludeTab();
    if (tab) {
      setDocument(tab.path, tab.module);
      setTokens(tab.semanticTokens, editorView);
      setEditorDiagnostics(getWorkspaceDiagnostics(), editorView);
      renderOutline(tab.symbols, editorView);
    }
    return;
  }

  const requestedPath = getActivePath();
  const source = getSource();
  const seq = ++analyzeSeq;

  try {
    const req = programRequest(source);
    const [data, symbols] = await Promise.all([
      api("analyze", { ...req, includePrelude: includePrelude() }),
      api("documentSymbols", req),
    ]);
    if (seq !== analyzeSeq) return;

    setDocument(data.file, data.module);
    setWorkspaceDiagnostics(data.diagnostics);

    // Only apply buffer-local tokens/outline if the user hasn't switched tabs mid-flight.
    if (getActivePath() === requestedPath) {
      setTokens(data.semanticTokens, editorView);
      setEditorDiagnostics(data.diagnostics, editorView);
      renderOutline(symbols, editorView);
    }

    setText("ast-output", data.ast);
    setText("hir-output", data.hir);
    setText("mir-output", data.mir);
    setText("llvm-ir-output", data.llvm_ir);

    renderDiagnostics(data.diagnostics);
  } catch (err) {
    console.error("analyze failed:", err);
  }
}

function setText(id: string, value: string): void {
  const el = document.getElementById(id);
  if (el) el.textContent = value || "";
}
