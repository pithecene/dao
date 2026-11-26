import {
  addUserFile,
  deleteUserFile,
  getActivePath,
  getActiveSource,
  getCurrentExampleName,
  getUserFiles,
  getWorkspaceRevision,
  renameUserFile,
  setWorkspaceProgram,
  updateActiveUserSource,
} from "./document";
import { loadExampleByName } from "./examples";
import {
  applySnapshot,
  decodeShareToken,
  encodeShareToken,
  importSnapshotJson,
  loadSavedDraft,
  normalizeSnapshot,
  saveDraft,
} from "./persistence";

// ---------------------------------------------------------------------------
// Minimal deterministic test harness (pure ES2022 + Web globals, 0 npm deps)
// ---------------------------------------------------------------------------

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) {
    throw new Error(`Assertion failed: ${message}`);
  }
}

function assertEqual<T>(actual: T, expected: T, label: string): void {
  if (actual !== expected) {
    throw new Error(
      `${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`,
    );
  }
}

class MemoryStorage implements Storage {
  private readonly store = new Map<string, string>();
  get length(): number {
    return this.store.size;
  }
  clear(): void {
    this.store.clear();
  }
  getItem(key: string): string | null {
    return this.store.get(key) ?? null;
  }
  key(index: number): string | null {
    return Array.from(this.store.keys())[index] ?? null;
  }
  removeItem(key: string): void {
    this.store.delete(key);
  }
  setItem(key: string, value: string): void {
    this.store.set(key, String(value));
  }
}

interface Deferred<T> {
  promise: Promise<T>;
  resolve: (value: T) => void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((res) => {
    resolve = res;
  });
  return { promise, resolve };
}

// Install in-memory localStorage for Node execution.
const memoryStorage = new MemoryStorage();
Object.defineProperty(globalThis, "localStorage", {
  value: memoryStorage,
  configurable: true,
  writable: true,
});

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

async function testOutOfOrderExampleRequestsAndEdits(): Promise<void> {
  memoryStorage.clear();
  setWorkspaceProgram(
    [{ path: "initial.dao", source: "module initial\n" }],
    "initial.dao",
  );

  const pendingByUrl = new Map<string, Deferred<Response>>();
  const originalFetch = globalThis.fetch;
  globalThis.fetch = ((input: RequestInfo | URL) => {
    const url = String(input);
    const gate = deferred<Response>();
    pendingByUrl.set(url, gate);
    return gate.promise;
  }) as typeof fetch;

  try {
    // 1. Start loading slow example A, then fast example B.
    const reqA = loadExampleByName("astar.dao", { persistDraft: true });
    const reqB = loadExampleByName("hello.dao", { persistDraft: true });

    // 2. Example B arrives first and populates the workspace + draft.
    pendingByUrl.get("/api/examples/hello.dao")!.resolve(
      new Response(
        JSON.stringify({
          name: "hello.dao",
          files: [{ path: "hello.dao", source: "module hello\nfn main(): i32 -> 0\n" }],
          document: "hello.dao",
          source: "module hello\nfn main(): i32 -> 0\n",
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    );
    assertEqual(await reqB, true, "Example B should apply");
    assertEqual(getActivePath(), "hello.dao", "Active path is hello.dao");

    // 3. User edits example B and saves draft before stale request A completes.
    const editedSource = "module hello\nfn main(): i32 -> 42\n";
    updateActiveUserSource(editedSource);
    saveDraft();
    const revAfterEdit = getWorkspaceRevision();

    // 4. Stale request A resolves late — must NOT overwrite workspace or draft.
    pendingByUrl.get("/api/examples/astar.dao")!.resolve(
      new Response(
        JSON.stringify({
          name: "astar.dao",
          files: [{ path: "astar.dao", source: "module astar\nfn main(): i32 -> 99\n" }],
          document: "astar.dao",
          source: "module astar\nfn main(): i32 -> 99\n",
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    );
    assertEqual(await reqA, false, "Stale request A must be discarded");
    assertEqual(getActivePath(), "hello.dao", "Active path stays hello.dao");
    assertEqual(getCurrentExampleName(), "hello.dao", "Example name stays hello.dao");
    assertEqual(getActiveSource(), editedSource, "User edit in B must survive");
    assertEqual(getWorkspaceRevision(), revAfterEdit, "Revision unchanged by discarded response");

    const saved = loadSavedDraft();
    assert(saved !== null, "Saved draft exists");
    assertEqual(saved.document, "hello.dao", "Saved draft document stays hello.dao");
    assertEqual(saved.files[0].source, editedSource, "Saved draft keeps user edit");

    // 5. Single in-flight request superseded by a direct user edit before arrival.
    const reqC = loadExampleByName("math.dao", { persistDraft: true });
    const secondEdit = "module hello\nfn main(): i32 -> 77\n";
    updateActiveUserSource(secondEdit);
    saveDraft();

    pendingByUrl.get("/api/examples/math.dao")!.resolve(
      new Response(
        JSON.stringify({
          name: "math.dao",
          files: [{ path: "math.dao", source: "module math\nfn main(): i32 -> 1\n" }],
          document: "math.dao",
          source: "module math\nfn main(): i32 -> 1\n",
        }),
        { status: 200, headers: { "Content-Type": "application/json" } },
      ),
    );
    assertEqual(await reqC, false, "Request C superseded by user edit must be discarded");
    assertEqual(getActiveSource(), secondEdit, "Second user edit survives single-flight race");
    assertEqual(loadSavedDraft()?.files[0].source, secondEdit, "Draft keeps second edit");
  } finally {
    globalThis.fetch = originalFetch;
  }
}

function testWorkspaceRevisionIncrementsOnEveryMutation(): void {
  setWorkspaceProgram([{ path: "a.dao", source: "module a\n" }], "a.dao");
  let prev = getWorkspaceRevision();

  updateActiveUserSource("module a\nfn f(): i32 -> 1\n");
  assert(getWorkspaceRevision() > prev, "Edit increments workspaceRevision");
  prev = getWorkspaceRevision();

  const added = addUserFile("b.dao");
  assert(added.ok, "addUserFile succeeds");
  assert(getWorkspaceRevision() > prev, "addUserFile increments workspaceRevision");
  prev = getWorkspaceRevision();

  const renamed = renameUserFile("b.dao", "c.dao");
  assert(renamed.ok, "renameUserFile succeeds");
  assert(getWorkspaceRevision() > prev, "renameUserFile increments workspaceRevision");
  prev = getWorkspaceRevision();

  assert(deleteUserFile("c.dao"), "deleteUserFile succeeds");
  assert(getWorkspaceRevision() > prev, "deleteUserFile increments workspaceRevision");
}

async function testMalformedAndPartialSnapshotsAreAtomic(): Promise<void> {
  memoryStorage.clear();
  const baselineSource = "module keep\nfn main(): i32 -> 7\n";
  setWorkspaceProgram([{ path: "keep.dao", source: baselineSource }], "keep.dao", {
    exampleName: "keep.dao",
  });
  saveDraft();
  const baselineRev = getWorkspaceRevision();

  // 1. Malformed snapshots must be rejected BEFORE any mutation (zero throw, zero partial state).
  const malformedPayloads: unknown[] = [
    null,
    "not-an-object",
    { schemaVersion: 2, document: "x.dao", files: [{ path: "x.dao", source: "" }] },
    { schemaVersion: 1, compiler: null, document: "x.dao", files: [{ path: "x.dao", source: "" }] },
    { schemaVersion: 1, compiler: "bad", document: "x.dao", files: [{ path: "x.dao", source: "" }] },
    { schemaVersion: 1, compiler: { commit: 123 }, document: "x.dao", files: [{ path: "x.dao", source: "" }] },
    { schemaVersion: 1, document: "missing.dao", files: [{ path: "x.dao", source: "" }] },
    { schemaVersion: 1, document: "x.dao", files: [{ path: "x.dao", source: "" }, { path: "./x.dao", source: "" }] },
    { schemaVersion: 1, document: "stdlib/core/option.dao", files: [{ path: "stdlib/core/option.dao", source: "" }] },
    { schemaVersion: 1, document: "../escape.dao", files: [{ path: "../escape.dao", source: "" }] },
    { schemaVersion: 1, document: "x.dao", files: [{ path: "x.dao", source: 999 }] },
  ];

  let onAppliedCalls = 0;
  for (const bad of malformedPayloads) {
    assertEqual(normalizeSnapshot(bad), null, "normalizeSnapshot rejects malformed input");
    assertEqual(applySnapshot(bad), false, "applySnapshot rejects malformed input without throwing");
    assertEqual(
      importSnapshotJson(JSON.stringify(bad), () => {
        ++onAppliedCalls;
      }),
      false,
      "importSnapshotJson rejects malformed JSON payload",
    );
  }

  assertEqual(onAppliedCalls, 0, "onApplied never called on rejected imports");
  assertEqual(getWorkspaceRevision(), baselineRev, "Workspace revision untouched after rejected imports");
  assertEqual(getActivePath(), "keep.dao", "Active path untouched after rejected imports");
  assertEqual(getUserFiles().length, 1, "User file count untouched");
  assertEqual(getUserFiles()[0].source, baselineSource, "User source untouched");
  assertEqual(loadSavedDraft()?.files[0].source, baselineSource, "Saved draft untouched");

  // 2. Snapshot omitting `compiler` (or with partial `compiler`) normalizes cleanly and applies atomically.
  const withoutCompiler = {
    schemaVersion: 1,
    document: "app",
    files: [
      { path: "lib.dao", source: "module lib\nfn val(): i32 -> 5\n" },
      { path: "app", source: "module app\nimport lib\nfn main(): i32 -> lib::val()\n" },
    ],
  };
  let synced = false;
  assertEqual(
    importSnapshotJson(JSON.stringify(withoutCompiler), () => {
      synced = true;
    }),
    true,
    "Snapshot missing compiler normalizes and imports cleanly",
  );
  assert(synced, "Editor sync callback ran after atomic snapshot apply");
  assertEqual(getActivePath(), "app.dao", "Normalized document path selected");
  assertEqual(getUserFiles().length, 2, "Both files imported");

  const persisted = loadSavedDraft();
  assert(persisted !== null, "Imported snapshot persisted to localStorage");
  assertEqual(persisted.compiler.commit, "unknown", "Missing commit normalized to unknown");
  assertEqual(persisted.compiler.llvmVersion, "unknown", "Missing llvmVersion normalized to unknown");
  assertEqual(persisted.compiler.targetTriple, "unknown", "Missing targetTriple normalized to unknown");

  // 3. Round-trip share token encoding & decoding validates via normalizeSnapshot.
  const token = await encodeShareToken(persisted);
  const decoded = await decodeShareToken(token);
  assert(decoded !== null, "Encoded share token decodes cleanly");
  assertEqual(decoded.document, "app.dao", "Share token preserves active document");
  assertEqual(decoded.files.length, 2, "Share token preserves file count");
}

async function runAll(): Promise<void> {
  await testOutOfOrderExampleRequestsAndEdits();
  testWorkspaceRevisionIncrementsOnEveryMutation();
  await testMalformedAndPartialSnapshotsAreAtomic();
  console.log("workspace_state.test: all assertions passed");
}

void runAll().catch((err: unknown) => {
  console.error(err);
  throw err;
});
