import type { Diagnostic } from "./generated/tooling_surface";

type DiagnosticNavigator = (
  file: string,
  offset: number,
  length: number,
) => void;

let onSelectDiagnostic: DiagnosticNavigator | null = null;

export function initDiagnostics(navigator: DiagnosticNavigator): void {
  onSelectDiagnostic = navigator;
}

function locationLabel(d: Diagnostic): string {
  if (!d.file) return "program";
  return `${d.file}:${d.line}:${d.col}`;
}

/** Render clickable program-wide diagnostics into the diagnostics panel. */
export function renderDiagnostics(diagnostics: Diagnostic[]): void {
  const container = document.getElementById("diagnostics-output")!;
  const countBadge = document.getElementById("diagnostics-count");

  const errors = diagnostics.filter((d) => d.severity === "error").length;
  const warnings = diagnostics.filter((d) => d.severity === "warning").length;

  if (countBadge) {
    countBadge.textContent = String(diagnostics.length);
    countBadge.className =
      errors > 0 || warnings > 0 ? "badge badge-warn" : "badge";
  }

  if (diagnostics.length === 0) {
    const ok = document.createElement("div");
    ok.className = "no-diagnostics";
    ok.textContent = "No diagnostics";
    container.replaceChildren(ok);
    return;
  }

  const rows = diagnostics.map((d) => {
    const row = document.createElement("button");
    row.type = "button";
    const clickable = Boolean(d.file);
    row.className = `diagnostic-row diagnostic-${d.severity}${clickable ? " clickable" : ""}`;
    if (clickable) {
      row.title = `Jump to ${d.file}:${d.line}:${d.col}`;
      row.addEventListener("click", () => {
        onSelectDiagnostic?.(d.file, d.offset, d.length);
      });
    }

    const sev = document.createElement("span");
    sev.className = "diag-severity";
    sev.textContent = d.severity === "warning" ? "warn" : "err";

    const loc = document.createElement("span");
    loc.className = "location";
    loc.textContent = locationLabel(d);

    const msg = document.createElement("span");
    msg.textContent = d.message;

    row.append(sev, loc, msg);
    return row;
  });

  container.replaceChildren(...rows);
}
