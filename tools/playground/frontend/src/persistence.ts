import {
  getAnalysisDocumentPath,
  getCurrentExampleName,
  getUserFiles,
  getVersionInfo,
  setWorkspaceProgram,
} from "./document";
import type { SourceInput } from "./generated/tooling_surface";
import { showNotice } from "./notice";
import { normalizeDaoPath, validateUserPath } from "./util";

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
  if (typeof document === "undefined") return;
  const el = document.getElementById("draft-status");
  if (el) el.textContent = text;
}

export function scheduleDraftSave(): void {
  setDraftStatus("Unsaved edits…");
  if (draftSaveTimer) clearTimeout(draftSaveTimer);
  draftSaveTimer = setTimeout(saveDraft, 400);
}

export function buildCurrentSnapshot(): WorkspaceSnapshotV1 {
  const activeDoc = getAnalysisDocumentPath();
  const files = getUserFiles().map((f) => ({
    path: f.path,
    source: f.source,
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
  if (draftSaveTimer) {
    clearTimeout(draftSaveTimer);
    draftSaveTimer = null;
  }
  try {
    const snap = buildCurrentSnapshot();
    localStorage.setItem(DRAFT_STORAGE_KEY, JSON.stringify(snap));
    setDraftStatus("Draft saved");
  } catch {
    setDraftStatus("");
  }
}

export function clearDraft(): void {
  if (draftSaveTimer) {
    clearTimeout(draftSaveTimer);
    draftSaveTimer = null;
  }
  try {
    localStorage.removeItem(DRAFT_STORAGE_KEY);
    setDraftStatus("");
  } catch {
    // Ignore.
  }
}

/**
 * Validate and normalize an untrusted snapshot object up front without
 * mutating any workspace state. Returns `null` if any required structure,
 * `compiler` metadata, `files` entry, path invariant, or `document` target
 * is malformed. Missing or partial `compiler` subfields normalize to `"unknown"`.
 */
export function normalizeSnapshot(value: unknown): WorkspaceSnapshotV1 | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return null;
  }
  const obj = value as Record<string, unknown>;
  if (obj.schemaVersion !== 1) {
    return null;
  }

  let commit = "unknown";
  let llvmVersion = "unknown";
  let targetTriple = "unknown";
  if (obj.compiler !== undefined) {
    if (
      !obj.compiler ||
      typeof obj.compiler !== "object" ||
      Array.isArray(obj.compiler)
    ) {
      return null;
    }
    const comp = obj.compiler as Record<string, unknown>;
    if (comp.commit !== undefined && typeof comp.commit !== "string") {
      return null;
    }
    if (comp.llvmVersion !== undefined && typeof comp.llvmVersion !== "string") {
      return null;
    }
    if (comp.targetTriple !== undefined && typeof comp.targetTriple !== "string") {
      return null;
    }
    commit = (comp.commit as string | undefined)?.trim() || "unknown";
    llvmVersion = (comp.llvmVersion as string | undefined)?.trim() || "unknown";
    targetTriple = (comp.targetTriple as string | undefined)?.trim() || "unknown";
  }

  if (obj.exampleName !== undefined && typeof obj.exampleName !== "string") {
    return null;
  }
  const exampleName = (obj.exampleName as string | undefined)?.trim() || undefined;

  if (!Array.isArray(obj.files) || obj.files.length === 0) {
    return null;
  }
  const seenPaths = new Set<string>();
  const normalizedFiles: SourceInput[] = [];
  for (const rawFile of obj.files) {
    if (!rawFile || typeof rawFile !== "object" || Array.isArray(rawFile)) {
      return null;
    }
    const entry = rawFile as Record<string, unknown>;
    if (typeof entry.path !== "string" || typeof entry.source !== "string") {
      return null;
    }
    const cleanPath = normalizeDaoPath(entry.path);
    if (validateUserPath(cleanPath) !== null || seenPaths.has(cleanPath)) {
      return null;
    }
    seenPaths.add(cleanPath);
    normalizedFiles.push({ path: cleanPath, source: entry.source });
  }

  let documentPath = normalizedFiles[0].path;
  if (obj.document !== undefined) {
    if (typeof obj.document !== "string") {
      return null;
    }
    const trimmedDoc = obj.document.trim();
    if (trimmedDoc) {
      const cleanDoc = normalizeDaoPath(trimmedDoc);
      if (!seenPaths.has(cleanDoc)) {
        return null;
      }
      documentPath = cleanDoc;
    }
  }

  return {
    schemaVersion: 1,
    compiler: {
      commit,
      llvmVersion,
      targetTriple,
    },
    exampleName,
    document: documentPath,
    files: normalizedFiles,
  };
}

/**
 * What storage holds for the draft, from one read.  Only a read that succeeds
 * and finds no entry is `absent`: a read that fails, and an entry that does
 * not decode, are `unreadable`, since either may hide someone's work.
 */
export type StoredDraft =
  | { kind: "absent" }
  | { kind: "unreadable" }
  | { kind: "draft"; snapshot: WorkspaceSnapshotV1 };

export function readStoredDraft(): StoredDraft {
  try {
    const raw = localStorage.getItem(DRAFT_STORAGE_KEY);
    if (raw === null) return { kind: "absent" };
    const snapshot = normalizeSnapshot(JSON.parse(raw) as unknown);
    return snapshot ? { kind: "draft", snapshot } : { kind: "unreadable" };
  } catch {
    return { kind: "unreadable" };
  }
}

export function loadSavedDraft(): WorkspaceSnapshotV1 | null {
  const stored = readStoredDraft();
  return stored.kind === "draft" ? stored.snapshot : null;
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
    ) as Partial<CompactShareV1>;
    if (compact.v !== 1 || !Array.isArray(compact.f) || compact.f.length === 0) {
      return null;
    }
    const files: SourceInput[] = [];
    for (const pair of compact.f) {
      if (
        !Array.isArray(pair) ||
        pair.length !== 2 ||
        typeof pair[0] !== "string" ||
        typeof pair[1] !== "string"
      ) {
        return null;
      }
      files.push({ path: pair[0], source: pair[1] });
    }
    return normalizeSnapshot({
      schemaVersion: 1,
      compiler: {
        commit: typeof compact.c === "string" ? compact.c : "unknown",
        llvmVersion: typeof compact.l === "string" ? compact.l : "unknown",
        targetTriple: typeof compact.t === "string" ? compact.t : "unknown",
      },
      document: compact.d,
      files,
    });
  } catch {
    return null;
  }
}

/**
 * Parse, validate, normalize, and atomically import a `.dao.json` snapshot
 * string. Never mutates workspace, editor, or draft storage on invalid input.
 */
export function importSnapshotJson(
  rawText: string,
  onApplied?: () => void,
): boolean {
  let parsed: unknown;
  try {
    parsed = JSON.parse(rawText) as unknown;
  } catch {
    showNotice("Failed to parse snapshot JSON");
    return false;
  }
  const normalized = normalizeSnapshot(parsed);
  if (!normalized) {
    showNotice("Invalid .dao.json workspace snapshot");
    return false;
  }
  applySnapshot(normalized);
  onApplied?.();
  saveDraft();
  return true;
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
        ? `Authored with daoc ${snap.compiler.commit} · LLVM ${snap.compiler.llvmVersion} (${snap.compiler.targetTriple}).`
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
      if (importSnapshotJson(text, onSnapshotImported)) {
        modal.hidden = true;
      }
    } catch {
      showNotice("Failed to read snapshot file");
    }
  });
}

export function closeSnapshotModal(): void {
  if (typeof document === "undefined") return;
  const modal = document.getElementById("snapshot-modal");
  if (modal) modal.hidden = true;
}

export function applySnapshot(input: WorkspaceSnapshotV1 | unknown): boolean {
  const snap = normalizeSnapshot(input);
  if (!snap) {
    return false;
  }
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
  return true;
}
