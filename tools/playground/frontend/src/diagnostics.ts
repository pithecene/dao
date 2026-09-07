import type { Diagnostic } from "./generated/tooling_surface";
import { escapeHtml } from "./util";

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
        `<span class="location">${d.line}:${d.col}</span>` +
        `${escapeHtml(d.message)}</div>`,
    )
    .join("");
}
