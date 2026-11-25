import {
  getAnalysisDocumentPath,
  getCurrentExampleName,
  getUserFiles,
  getVersionInfo,
  isActivePrelude,
  setWorkspaceProgram,
} from "./document";
import { getSource } from "./editor";
import type { SourceInput } from "./generated/tooling_surface";
import { showNotice } from "./notice";

const DRAFT_STORAGE_KEY = "dao.workspace.draft.v1";

export interface WorkspaceSnapshotV1 {
  schemaVersion: 1;
  compiler: {
    commit: string;
    llvmVersion: string;
    targetTriple: string;
  };
  exampleName?: string;
  document: string;
  files: SourceInput[];
}

interface CompactShareV1 {
  v: 1;
  c: string;
  l: string;
  t: string;
  d: string;
  f: [string, string][];
}

let draftSaveTimer: ReturnType<typeof setTimeout> | null = null;

function setDraftStatus(text: string): void {
  const el = document.getElementById("draft-status");
  if (el) el.textContent = text;
}

export function scheduleDraftSave(): void {
  setDraftStatus("Unsaved edits…");
  if (draftSaveTimer) clearTimeout(draftSaveTimer);
  draftSaveTimer = setTimeout(saveDraft, 400);
}

export function buildCurrentSnapshot(): WorkspaceSnapshotV1 {
  const liveSource = !isActivePrelude() ? getSource() : undefined;
  const activeDoc = getAnalysisDocumentPath();
  const files = getUserFiles().map((f) => ({
    path: f.path,
    source:
      liveSource !== undefined && f.path === activeDoc ? liveSource : f.source,
  }));
  const info = getVersionInfo();
  return {
    schemaVersion: 1,
    compiler: {
      commit: info?.commit ?? "unknown",
      llvmVersion: info?.llvmVersion ?? "unknown",
      targetTriple: info?.targetTriple ?? "unknown",
    },
    exampleName: getCurrentExampleName() || undefined,
    document: activeDoc,
    files,
  };
}

export function saveDraft(): void {
  try {
    const snap = buildCurrentSnapshot();
    localStorage.setItem(DRAFT_STORAGE_KEY, JSON.stringify(snap));
    setDraftStatus("Draft saved");
  } catch {
    setDraftStatus("");
  }
}

export function clearDraft(): void {
  try {
    localStorage.removeItem(DRAFT_STORAGE_KEY);
    setDraftStatus("");
  } catch {
    // Ignore.
  }
}

export function loadSavedDraft(): WorkspaceSnapshotV1 | null {
  try {
    const raw = localStorage.getItem(DRAFT_STORAGE_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw) as WorkspaceSnapshotV1;
    if (!isValidSnapshot(parsed)) return null;
    return parsed;
  } catch {
    return null;
  }
}

function isValidSnapshot(value: unknown): value is WorkspaceSnapshotV1 {
  if (!value || typeof value !== "object") return false;
  const obj = value as Partial<WorkspaceSnapshotV1>;
  if (obj.schemaVersion !== 1 || !Array.isArray(obj.files) || obj.files.length === 0) {
    return false;
  }
  return obj.files.every(
    (f) => f && typeof f.path === "string" && typeof f.source === "string",
  );
}

// ---------------------------------------------------------------------------
// Shareable URL Encoding (CompressionStream deflate-raw + URL-safe base64)
// ---------------------------------------------------------------------------

function bytesToBase64Url(bytes: Uint8Array): string {
  let binary = "";
  for (let i = 0; i < bytes.length; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function base64UrlToBytes(encoded: string): Uint8Array {
  const padded = encoded.replace(/-/g, "+").replace(/_/g, "/");
  const mod = padded.length % 4;
  const base64 = mod === 0 ? padded : padded + "=".repeat(4 - mod);
  const binary = atob(base64);
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    out[i] = binary.charCodeAt(i);
  }
  return out;
}

export async function encodeShareToken(snap: WorkspaceSnapshotV1): Promise<string> {
  const compact: CompactShareV1 = {
    v: 1,
    c: snap.compiler.commit,
    l: snap.compiler.llvmVersion,
    t: snap.compiler.targetTriple,
    d: snap.document,
    f: snap.files.map((file) => [file.path, file.source]),
  };
  const jsonBytes = new TextEncoder().encode(JSON.stringify(compact));

  if (typeof CompressionStream !== "undefined") {
    try {
      const cs = new CompressionStream("deflate-raw");
      const writer = cs.writable.getWriter();
      void writer.write(jsonBytes);
      void writer.close();
      const compressed = new Uint8Array(await new Response(cs.readable).arrayBuffer());
      return `z.${bytesToBase64Url(compressed)}`;
    } catch {
      // Fall through to uncompressed encoding.
    }
  }
  return `u.${bytesToBase64Url(jsonBytes)}`;
}

export async function decodeShareToken(
  token: string,
): Promise<WorkspaceSnapshotV1 | null> {
  try {
    let jsonBytes: Uint8Array;
    if (token.startsWith("z.") && typeof DecompressionStream !== "undefined") {
      const raw = base64UrlToBytes(token.slice(2));
      const ds = new DecompressionStream("deflate-raw");
      const writer = ds.writable.getWriter();
      void writer.write(new Uint8Array(raw));
      void writer.close();
      jsonBytes = new Uint8Array(await new Response(ds.readable).arrayBuffer());
    } else if (token.startsWith("u.")) {
      jsonBytes = base64UrlToBytes(token.slice(2));
    } else {
      return null;
    }

    const compact = JSON.parse(
      new TextDecoder().decode(jsonBytes),
    ) as CompactShareV1;
    if (compact.v !== 1 || !Array.isArray(compact.f) || compact.f.length === 0) {
      return null;
    }
    return {
      schemaVersion: 1,
      compiler: {
        commit: compact.c || "unknown",
        llvmVersion: compact.l || "unknown",
        targetTriple: compact.t || "unknown",
      },
      document: compact.d,
      files: compact.f.map(([path, source]) => ({ path, source })),
    };
  } catch {
    return null;
  }
}

// ---------------------------------------------------------------------------
// Snapshot Modal UI
// ---------------------------------------------------------------------------

export function initSnapshotModal(onSnapshotImported: () => void): void {
  const modal = document.getElementById("snapshot-modal")!;
  const shareBtn = document.getElementById("share-btn")!;
  const closeBtn = document.getElementById("close-snapshot-btn")!;
  const urlInput = document.getElementById("share-url-input") as HTMLInputElement;
  const copyBtn = document.getElementById("copy-url-btn")!;
  const exportBtn = document.getElementById("export-json-btn")!;
  const importInput = document.getElementById("import-json-input") as HTMLInputElement;
  const statusEl = document.getElementById("snapshot-status")!;
  const versionNote = document.getElementById("snapshot-version-note")!;

  shareBtn.addEventListener("click", async () => {
    const snap = buildCurrentSnapshot();
    const token = await encodeShareToken(snap);
    const shareUrl = `${window.location.origin}${window.location.pathname}#playground?s=${token}`;
    urlInput.value = shareUrl;
    statusEl.textContent = `${snap.files.length} ${snap.files.length === 1 ? "file" : "files"}`;
    versionNote.textContent =
      snap.compiler.commit !== "unknown"
        ? `Pinned to daoc ${snap.compiler.commit} · LLVM ${snap.compiler.llvmVersion} (${snap.compiler.targetTriple}).`
        : "Share this multi-file program by URL or export a versioned JSON snapshot.";
    modal.hidden = false;
    urlInput.select();
  });

  closeBtn.addEventListener("click", () => {
    modal.hidden = true;
  });

  modal.addEventListener("click", (e) => {
    if (e.target === modal) modal.hidden = true;
  });

  copyBtn.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(urlInput.value);
      statusEl.textContent = "Copied link to clipboard";
    } catch {
      urlInput.select();
      statusEl.textContent = "Press Cmd/Ctrl+C to copy";
    }
  });

  exportBtn.addEventListener("click", () => {
    const snap = buildCurrentSnapshot();
    const blob = new Blob([JSON.stringify(snap, null, 2) + "\n"], {
      type: "application/json",
    });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    const stamp = snap.compiler.commit !== "unknown" ? `-${snap.compiler.commit}` : "";
    link.href = url;
    link.download = `dao-workspace${stamp}.dao.json`;
    link.click();
    URL.revokeObjectURL(url);
    statusEl.textContent = "Downloaded snapshot";
  });

  importInput.addEventListener("change", async () => {
    const file = importInput.files?.[0];
    importInput.value = "";
    if (!file) return;
    try {
      const text = await file.text();
      const parsed = JSON.parse(text) as unknown;
      if (!isValidSnapshot(parsed)) {
        showNotice("Invalid .dao.json workspace snapshot");
        return;
      }
      applySnapshot(parsed);
      saveDraft();
      modal.hidden = true;
      onSnapshotImported();
    } catch {
      showNotice("Failed to parse snapshot JSON");
    }
  });
}

export function closeSnapshotModal(): void {
  const modal = document.getElementById("snapshot-modal");
  if (modal) modal.hidden = true;
}

export function applySnapshot(snap: WorkspaceSnapshotV1): void {
  setWorkspaceProgram(snap.files, snap.document, {
    exampleName: snap.exampleName ?? "",
  });
  const live = getVersionInfo();
  if (
    live &&
    snap.compiler.commit &&
    snap.compiler.commit !== "unknown" &&
    snap.compiler.commit !== live.commit
  ) {
    showNotice(
      `Snapshot authored on daoc ${snap.compiler.commit} (running ${live.commit})`,
    );
  } else {
    setDraftStatus(
      snap.compiler.commit !== "unknown"
        ? `Loaded · daoc ${snap.compiler.commit}`
        : "Loaded snapshot",
    );
  }
}
