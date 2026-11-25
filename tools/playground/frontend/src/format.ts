import { api } from "./api";
import { renderDiagnostics } from "./diagnostics";
import { programRequest } from "./document";
import { getSource, setSource } from "./editor";

let formatInFlight = false;

/** Format the current editor buffer using the compiler service. */
export async function doFormat(): Promise<void> {
  if (formatInFlight) return;
  formatInFlight = true;

  const btn = document.getElementById("format-btn") as HTMLButtonElement | null;
  if (btn) btn.disabled = true;

  try {
    const data = await api("format", programRequest(getSource()));
    if (data.diagnostics.length > 0) {
      renderDiagnostics(data.diagnostics);
      return;
    }
    if (data.changed) {
      setSource(data.formatted);
    }
  } finally {
    if (btn) btn.disabled = false;
    formatInFlight = false;
  }
}
