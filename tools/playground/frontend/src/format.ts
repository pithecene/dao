import { api } from "./api";
import { scheduleAnalyze } from "./analysis";
import { renderDiagnostics } from "./diagnostics";
import {
  getActivePath,
  getActiveSource,
  getWorkspaceRevision,
  hasUserFile,
  isActivePrelude,
  onWorkspaceChange,
  onWorkspaceLifecycle,
  programRequest,
  setWorkspaceDiagnostics,
  updateActiveUserSource,
} from "./document";
import {
  applyUndoableEdit,
  getEditorView,
  getSource,
  setEditorDiagnostics,
} from "./editor";
import { scheduleDraftSave } from "./persistence";

export type FormatOutcome =
  | "formatted"
  | "unchanged"
  | "diagnostics"
  | "superseded"
  | "readonly";

interface InFlightFormatTarget {
  readonly reqId: number;
  readonly path: string;
  readonly revision: number;
  readonly source: string;
}

let latestFormatRequestId = 0;
let inFlightTarget: InFlightFormatTarget | null = null;
let listenersBound = false;

function ensureFormatListeners(): void {
  if (listenersBound) return;
  listenersBound = true;
  onWorkspaceChange(syncFormatButtonState);
  onWorkspaceLifecycle({
    onReset: () => {
      ++latestFormatRequestId;
      inFlightTarget = null;
      syncFormatButtonState();
    },
    onDelete: () => {
      ++latestFormatRequestId;
      inFlightTarget = null;
      syncFormatButtonState();
    },
    onRename: () => {
      ++latestFormatRequestId;
      inFlightTarget = null;
      syncFormatButtonState();
    },
  });
}

function isSameTargetInFlight(
  path: string,
  revision: number,
  source: string,
): boolean {
  return (
    inFlightTarget !== null &&
    inFlightTarget.reqId === latestFormatRequestId &&
    inFlightTarget.path === path &&
    inFlightTarget.revision === revision &&
    inFlightTarget.source === source
  );
}

function syncFormatButtonState(): void {
  if (typeof document === "undefined") return;
  const btn = document.getElementById("format-btn") as HTMLButtonElement | null;
  if (!btn) return;
  if (isActivePrelude()) {
    btn.disabled = true;
    return;
  }
  const activePath = getActivePath();
  const activeSource = getEditorView() ? getSource() : getActiveSource();
  btn.disabled = isSameTargetInFlight(
    activePath,
    getWorkspaceRevision(),
    activeSource,
  );
}

export function initFormat(): void {
  ensureFormatListeners();
  syncFormatButtonState();
}

/**
 * Format the currently active writable user file via `/api/format`.
 * Guards against read-only prelude tabs, out-of-order requests, concurrent
 * edits, tab switches, file renames/deletions, and workspace resets, and
 * applies changes as an undoable editor transaction.
 */
export async function doFormat(): Promise<FormatOutcome> {
  ensureFormatListeners();
  if (isActivePrelude()) return "readonly";

  const capturedPath = getActivePath();
  if (!hasUserFile(capturedPath)) return "readonly";

  const viewAtStart = getEditorView();
  const capturedSource = viewAtStart ? getSource() : getActiveSource();
  const capturedRevision = getWorkspaceRevision();
  if (isSameTargetInFlight(capturedPath, capturedRevision, capturedSource)) {
    return "superseded";
  }

  const request = programRequest(capturedSource);
  if (request.document !== capturedPath) {
    return "superseded";
  }

  const reqId = ++latestFormatRequestId;
  inFlightTarget = {
    reqId,
    path: capturedPath,
    revision: capturedRevision,
    source: capturedSource,
  };
  syncFormatButtonState();

  try {
    const data = await api("format", request);
    const liveView = getEditorView();
    const liveSource = liveView ? getSource() : getActiveSource();
    if (
      reqId !== latestFormatRequestId ||
      isActivePrelude() ||
      !hasUserFile(capturedPath) ||
      data.file !== capturedPath ||
      getActivePath() !== capturedPath ||
      getWorkspaceRevision() !== capturedRevision ||
      liveSource !== capturedSource
    ) {
      return "superseded";
    }

    if (data.diagnostics.length > 0) {
      setWorkspaceDiagnostics(data.diagnostics);
      renderDiagnostics(data.diagnostics);
      if (liveView) {
        setEditorDiagnostics(data.diagnostics, liveView);
      }
      return "diagnostics";
    }

    if (!data.changed || data.formatted === capturedSource) {
      return "unchanged";
    }

    if (liveView) {
      return applyUndoableEdit(capturedPath, capturedSource, data.formatted)
        ? "formatted"
        : "superseded";
    }

    updateActiveUserSource(data.formatted);
    scheduleDraftSave();
    if (getEditorView()) {
      scheduleAnalyze();
    }
    return "formatted";
  } catch (err) {
    console.error("format failed:", err);
    return "unchanged";
  } finally {
    if (inFlightTarget?.reqId === reqId) {
      inFlightTarget = null;
    }
    syncFormatButtonState();
  }
}
