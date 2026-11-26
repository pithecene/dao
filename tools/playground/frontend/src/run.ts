import {
  isActivePrelude,
  programRequest,
  setWorkspaceDiagnostics,
} from "./document";
import { api } from "./api";
import { getEditorView, getSource, setEditorDiagnostics } from "./editor";
import { renderDiagnostics } from "./diagnostics";

let runInFlight = false;

/** Compile and execute the entire multi-file workspace program. */
export async function doRun(): Promise<void> {
  if (runInFlight) return;
  runInFlight = true;

  const btn = document.getElementById("run-btn") as HTMLButtonElement;
  const output = document.getElementById("console-output")!;
  const exitBadge = document.getElementById("exit-badge");

  btn.disabled = true;
  btn.textContent = "Running…";
  output.className = "";
  output.textContent = "Compiling program…";
  if (exitBadge) exitBadge.hidden = true;

  try {
    const req = isActivePrelude()
      ? programRequest()
      : programRequest(getSource());
    const data = await api("run", req);

    setWorkspaceDiagnostics(data.diagnostics);
    renderDiagnostics(data.diagnostics);
    const view = getEditorView();
    if (view) {
      setEditorDiagnostics(data.diagnostics, view);
    }

    if (data.exit_code === -1) {
      output.className = "console-error";
      output.textContent =
        data.diagnostics.length > 0
          ? "Compilation failed. Click any diagnostic below to jump to its location."
          : "Compilation failed (internal error).";
      if (exitBadge) {
        exitBadge.textContent = "compile error";
        exitBadge.className = "badge badge-warn";
        exitBadge.hidden = false;
      }
      return;
    }

    let text = data.stdout;
    if (data.stderr) {
      if (text) text += "\n";
      text += data.stderr;
    }

    if (exitBadge) {
      exitBadge.textContent = `exit ${data.exit_code}`;
      exitBadge.className =
        data.exit_code === 0 ? "badge badge-accent" : "badge badge-warn";
      exitBadge.hidden = false;
    }

    if (data.exit_code !== 0) {
      if (text) text += "\n";
      text += `\nProcess exited with code ${data.exit_code}`;
      output.className = "console-error";
    } else {
      output.className = "console-ok";
    }

    output.textContent = text || "(no stdout output — exit 0)";
  } catch (err) {
    output.className = "console-error";
    output.textContent = `Run failed: ${(err as Error).message}`;
  } finally {
    btn.disabled = false;
    btn.textContent = "▶ Run";
    runInFlight = false;
  }
}
