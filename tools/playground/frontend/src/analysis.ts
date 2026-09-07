import type { EditorView } from "@codemirror/view";
import { api } from "./api";
import { setTokens } from "./highlighting";
import { renderDiagnostics } from "./diagnostics";
import { renderOutline } from "./outline";
import { getSource } from "./editor";

let analyzeTimer: ReturnType<typeof setTimeout> | null = null;
let analyzeSeq = 0;
let editorView: EditorView | null = null;

const DEBOUNCE_MS = 300;

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
  analyzeTimer = setTimeout(doAnalyze, DEBOUNCE_MS);
}

function includePrelude(): boolean {
  const box = document.getElementById("include-prelude") as HTMLInputElement;
  return box.checked;
}

/** Run analysis immediately: tokens, diagnostics, IR panels, outline. */
export async function doAnalyze(): Promise<void> {
  if (!editorView) return;

  const source = getSource();
  const seq = ++analyzeSeq;

  try {
    const [data, symbols] = await Promise.all([
      api("analyze", { source, includePrelude: includePrelude() }),
      api("documentSymbols", { source }),
    ]);
    if (seq !== analyzeSeq) return; // a newer edit superseded this request

    setTokens(data.semanticTokens, editorView);

    setText("ast-output", data.ast);
    setText("hir-output", data.hir);
    setText("mir-output", data.mir);
    setText("llvm-ir-output", data.llvm_ir);

    renderDiagnostics(data.diagnostics);
    renderOutline(symbols, editorView);
  } catch (err) {
    console.error("analyze failed:", err);
  }
}

function setText(id: string, value: string): void {
  const el = document.getElementById(id);
  if (el) el.textContent = value || "";
}
