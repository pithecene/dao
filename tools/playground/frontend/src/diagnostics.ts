import { inDocument } from "./document";
import type { Diagnostic } from "./generated/tooling_surface";
import { escapeHtml } from "./util";

/** `line:col` for the document, `file:line:col` for anywhere else. */
function location(d: Diagnostic): string {
  const prefix = d.file && !inDocument(d.file) ? `${d.file}:` : "";
  return `${prefix}${d.line}:${d.col}`;
}

/** Render diagnostics into the diagnostics panel. */
export function renderDiagnostics(diagnostics: Diagnostic[]): void {
  const container = document.getElementById("diagnostics-output")!;

  if (diagnostics.length === 0) {
    container.innerHTML = '<div class="no-diagnostics">No errors</div>';
    return;
  }

  container.innerHTML = diagnostics
    .map(
      (d) =>
        `<div class="diagnostic diagnostic-${d.severity}">` +
        `<span class="location">${escapeHtml(location(d))}</span>` +
        `${escapeHtml(d.message)}</div>`,
    )
    .join("");
}
