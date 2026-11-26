import { api } from "./api";
import { renderDiagnostics } from "./diagnostics";
import { documentFile, programRequest } from "./document";
import { getSource, setSource } from "./editor";

let formatInFlight = false;

/** Format the current editor buffer using the compiler service. */
export async function doFormat(): Promise<void> {
  if (formatInFlight) return;
  formatInFlight = true;

  const btn = document.getElementById("format-btn") as HTMLButtonElement | null;
  if (btn) btn.disabled = true;

  const capturedSource = getSource();
  const request = programRequest(capturedSource);
  const capturedDocument = request.document;
  const capturedFile = documentFile();

  try {
    const data = await api("format", request);
    if (
      data.file !== capturedDocument ||
      documentFile() !== capturedFile ||
      getSource() !== capturedSource
    ) {
      return;
    }
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
