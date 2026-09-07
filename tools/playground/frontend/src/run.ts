import { api } from "./api";
import { getSource } from "./editor";
import { renderDiagnostics } from "./diagnostics";

let runInFlight = false;

/** Compile and execute the current editor source. */
export async function doRun(): Promise<void> {
  if (runInFlight) return;
  runInFlight = true;

  const btn = document.getElementById("run-btn") as HTMLButtonElement;
  const output = document.getElementById("console-output")!;
  btn.disabled = true;
  btn.textContent = "⏳ Running…";
  output.className = "";
  output.textContent = "Compiling…";

  try {
    const data = await api("run", { source: getSource() });

    // Always update diagnostics (clears stale errors on success).
    renderDiagnostics(data.diagnostics);

    if (data.exit_code === -1) {
      output.className = "console-error";
      output.textContent =
        data.diagnostics.length > 0
          ? "Compilation failed. See diagnostics."
          : "Compilation failed (internal error).";
      return;
    }

    let text = data.stdout;
    if (data.stderr) {
      if (text) text += "\n";
      text += data.stderr;
    }

    if (data.exit_code !== 0) {
      if (text) text += "\n";
      text += `\nProcess exited with code ${data.exit_code}`;
      output.className = "console-error";
    } else {
      output.className = "console-ok";
    }

    output.textContent = text || "(no output)";
  } catch (err) {
    output.className = "console-error";
    output.textContent = `Run failed: ${(err as Error).message}`;
  } finally {
    btn.disabled = false;
    btn.textContent = "▶ Run";
    runInFlight = false;
  }
}
