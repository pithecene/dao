import { CompletionContext } from "@codemirror/autocomplete";
import { EditorState, Transaction } from "@codemirror/state";
import { daoCompletionSource } from "./completions";
import {
  addUserFile,
  closePreludeTab,
  deleteUserFile,
  getActivePath,
  getActiveSource,
  getCurrentExampleName,
  getUserFiles,
  getWorkspaceDiagnostics,
  getWorkspaceRevision,
  openPreludeTab,
  renameUserFile,
  selectTab,
  setWorkspaceProgram,
  updateActiveUserSource,
} from "./document";
import { buildUndoableReplaceTransaction } from "./editor";
import { FALLBACK_FILES, loadExampleByName, resetWorkspace, resetWouldDiscardWork } from "./examples";
import { doFormat } from "./format";
import {
  applySnapshot,
  clearDraft,
  decodeShareToken,
  encodeShareToken,
  importSnapshotJson,
  loadSavedDraft,
  normalizeSnapshot,
  saveDraft,
  scheduleDraftSave,
} from "./persistence";
import { bootstrapWorkspace } from "./startup";

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
  reject: (reason?: unknown) => void;
}

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  let reject!: (reason?: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

async function withMockFetch(
  handler: (url: string, init?: RequestInit) => Promise<Response>,
  run: () => Promise<void>,
): Promise<void> {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = ((input: RequestInfo | URL, init?: RequestInit) =>
    handler(String(input), init)) as typeof fetch;
  try {
    await run();
  } finally {
    globalThis.fetch = originalFetch;
  }
}

async function waitForCondition(
  predicate: () => boolean,
  label: string,
): Promise<void> {
  for (let i = 0; i < 50; i++) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  throw new Error(`Timed out waiting for ${label}`);
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
  clearDraft();
  memoryStorage.clear();
  setWorkspaceProgram(
    [{ path: "initial.dao", source: "module initial\n" }],
    "initial.dao",
  );

  const pendingByUrl = new Map<string, Deferred<Response>>();
  await withMockFetch(
    (url) => {
      const gate = deferred<Response>();
      pendingByUrl.set(url, gate);
      return gate.promise;
    },
    async () => {
      // 1. Start loading slow example A, then fast example B.
      const reqA = loadExampleByName("astar.dao", { persistDraft: true });
      const reqB = loadExampleByName("hello.dao", { persistDraft: true });

      // 2. Example B arrives first and populates the workspace + draft.
      pendingByUrl.get("/api/examples/hello.dao")!.resolve(
        jsonResponse({
          name: "hello.dao",
          files: [{ path: "hello.dao", source: "module hello\nfn main(): i32 -> 0\n" }],
          document: "hello.dao",
          source: "module hello\nfn main(): i32 -> 0\n",
        }),
      );
      assertEqual(await reqB, "applied", "Example B should apply");
      assertEqual(getActivePath(), "hello.dao", "Active path is hello.dao");

      // 3. User edits example B and saves draft before stale request A completes.
      const editedSource = "module hello\nfn main(): i32 -> 42\n";
      updateActiveUserSource(editedSource);
      saveDraft();
      const revAfterEdit = getWorkspaceRevision();

      // 4. Stale request A resolves late — must return "superseded" and preserve workspace/draft.
      pendingByUrl.get("/api/examples/astar.dao")!.resolve(
        jsonResponse({
          name: "astar.dao",
          files: [{ path: "astar.dao", source: "module astar\nfn main(): i32 -> 99\n" }],
          document: "astar.dao",
          source: "module astar\nfn main(): i32 -> 99\n",
        }),
      );
      assertEqual(await reqA, "superseded", "Stale request A must be superseded");
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
        jsonResponse({
          name: "math.dao",
          files: [{ path: "math.dao", source: "module math\nfn main(): i32 -> 1\n" }],
          document: "math.dao",
          source: "module math\nfn main(): i32 -> 1\n",
        }),
      );
      assertEqual(await reqC, "superseded", "Request C superseded by user edit");
      assertEqual(getActiveSource(), secondEdit, "Second user edit survives single-flight race");
      assertEqual(loadSavedDraft()?.files[0].source, secondEdit, "Draft keeps second edit");

      // 6. Non-superseded HTTP failure reports "failed", while superseded failure reports "superseded".
      const reqFail = loadExampleByName("missing.dao", { persistDraft: false });
      pendingByUrl.get("/api/examples/missing.dao")!.resolve(
        jsonResponse({ error: "not found" }, 404),
      );
      assertEqual(await reqFail, "failed", "Unmodified failing load reports failed");

      const reqSupersededFail = loadExampleByName("broken.dao", { persistDraft: false });
      updateActiveUserSource("module hello\nfn main(): i32 -> 88\n");
      pendingByUrl.get("/api/examples/broken.dao")!.resolve(
        jsonResponse({ error: "not found" }, 404),
      );
      assertEqual(
        await reqSupersededFail,
        "superseded",
        "Edited failing load reports superseded rather than failed",
      );
    },
  );
}

async function testBootstrapStartupEditsAndGenuineFallback(): Promise<void> {
  const versionPayload = {
    commit: "deadbeef",
    llvmVersion: "21.1.8",
    targetTriple: "arm64-apple-darwin",
    preludeFiles: ["stdlib/core/option.dao"],
  };
  const catalogPayload = {
    examples: [{ name: "hello.dao", isMultifile: false }],
  };

  async function runStartupEditBeforeHelloSettles(
    label: string,
    editedSource: string,
    settleHello: (gate: Deferred<Response>) => void,
  ): Promise<void> {
    clearDraft();
    memoryStorage.clear();
    setWorkspaceProgram([{ path: "main.dao", source: "module main\n" }], "main.dao");

    let helloRequested = false;
    const helloGate = deferred<Response>();
    await withMockFetch(
      (url) => {
        if (url === "/api/version") return Promise.resolve(jsonResponse(versionPayload));
        if (url === "/api/examples") return Promise.resolve(jsonResponse(catalogPayload));
        if (url === "/api/examples/hello.dao") {
          helloRequested = true;
          return helloGate.promise;
        }
        return Promise.resolve(jsonResponse({ error: "unexpected" }, 404));
      },
      async () => {
        const bootDone = bootstrapWorkspace();
        await waitForCondition(() => helloRequested, `${label}: hello.dao fetch in flight`);

        updateActiveUserSource(editedSource);
        scheduleDraftSave();

        settleHello(helloGate);
        await bootDone;

        assertEqual(getActivePath(), "main.dao", `${label}: keeps user file path`);
        assertEqual(getCurrentExampleName(), "", `${label}: keeps exampleName unset`);
        assertEqual(getActiveSource(), editedSource, `${label}: preserves user edit`);

        await new Promise((resolve) => setTimeout(resolve, 430));
        const saved = loadSavedDraft();
        assert(saved !== null, `${label}: autosave persisted`);
        assertEqual(saved.document, "main.dao", `${label}: autosaved document is main.dao`);
        assertEqual(
          saved.files[0].source,
          editedSource,
          `${label}: autosave persisted user edit, never FALLBACK_FILES`,
        );
      },
    );
  }

  // 1. Startup `hello.dao` pending -> user edits buffer -> stale 200 OK arrives.
  await runStartupEditBeforeHelloSettles(
    "stale-200",
    "module custom\nfn main(): i32 -> 123\n",
    (gate) =>
      gate.resolve(
        jsonResponse({
          name: "hello.dao",
          files: [{ path: "hello.dao", source: "module server_hello\nfn main(): i32 -> 0\n" }],
          document: "hello.dao",
          source: "module server_hello\nfn main(): i32 -> 0\n",
        }),
      ),
  );

  // 2. Startup `hello.dao` pending -> user edits buffer -> HTTP 404 or network failure arrives.
  await runStartupEditBeforeHelloSettles(
    "http-404",
    "module keep_404\nfn main(): i32 -> 9\n",
    (gate) => gate.resolve(jsonResponse({ error: "missing hello.dao" }, 404)),
  );
  await runStartupEditBeforeHelloSettles(
    "network-reject",
    "module keep_net\nfn main(): i32 -> 10\n",
    (gate) => gate.reject(new Error("simulated transport disconnect")),
  );

  // 3. Genuine startup load failure with NO intervening user edit installs FALLBACK_FILES.
  clearDraft();
  memoryStorage.clear();
  setWorkspaceProgram([{ path: "main.dao", source: "module main\n" }], "main.dao");

  await withMockFetch(
    (url) => {
      if (url === "/api/version") return Promise.resolve(jsonResponse(versionPayload));
      if (url === "/api/examples") return Promise.resolve(jsonResponse(catalogPayload));
      return Promise.resolve(jsonResponse({ error: "service unavailable" }, 404));
    },
    async () => {
      await bootstrapWorkspace();
      assertEqual(getActivePath(), "hello.dao", "Genuine startup failure installs hello.dao");
      assertEqual(getCurrentExampleName(), "hello.dao", "Genuine startup failure sets exampleName");
      assertEqual(
        getActiveSource(),
        FALLBACK_FILES[0].source,
        "Genuine startup failure installs FALLBACK_FILES source",
      );
      assertEqual(
        loadSavedDraft(),
        null,
        "Genuine startup fallback does not pollute localStorage draft",
      );
    },
  );
}

function testUndoableReplaceTransactionInvariants(): void {
  const unformatted = "module app\nfn main():i32->1+2\n";
  const formatted = "module app\n\nfn main(): i32 -> 1 + 2\n";

  const writableState = EditorState.create({
    doc: unformatted,
    selection: { anchor: unformatted.length, head: unformatted.length },
  });
  const tr = buildUndoableReplaceTransaction(writableState, unformatted, formatted);
  assert(tr !== null, "Writable matching state produces an undoable transaction");
  assert(tr.docChanged, "Transaction updates buffer text");
  assert(
    tr.annotation(Transaction.addToHistory) !== false,
    "Format transaction stays in CodeMirror undo history",
  );
  assertEqual(tr.newDoc.toString(), formatted, "Transaction produces formatted source");
  assertEqual(
    tr.newSelection.main.anchor,
    unformatted.length,
    "Selection stays valid within formatted document",
  );

  // Stale expectedSource or identical source or readOnly state must reject without mutating.
  assertEqual(
    buildUndoableReplaceTransaction(writableState, "stale source", formatted),
    null,
    "Mismatched expectedSource returns null",
  );
  assertEqual(
    buildUndoableReplaceTransaction(writableState, unformatted, unformatted),
    null,
    "Unchanged source returns null",
  );
  const readOnlyState = EditorState.create({
    doc: unformatted,
    extensions: [EditorState.readOnly.of(true)],
  });
  assertEqual(
    buildUndoableReplaceTransaction(readOnlyState, unformatted, formatted),
    null,
    "Read-only state refuses undoable replacement",
  );
}

async function testFormatterTargetsWritableFileAndGuardsStaleReplies(): Promise<void> {
  clearDraft();
  memoryStorage.clear();
  setWorkspaceProgram(
    [
      { path: "a.dao", source: "module a\nfn f():i32->1+2\n" },
      { path: "b.dao", source: "module b\nfn g(): i32 -> 3\n" },
    ],
    "a.dao",
  );

  const pendingFormatGates: Deferred<Response>[] = [];

  await withMockFetch(
    (url) => {
      if (url === "/api/format") {
        const gate = deferred<Response>();
        pendingFormatGates.push(gate);
        return gate.promise;
      }
      return Promise.resolve(jsonResponse({ error: "unexpected" }, 404));
    },
    async () => {
      // 1. Read-only prelude tab refuses formatting without calling /api/format or mutating user files.
      openPreludeTab({
        path: "stdlib/core/option.dao",
        module: "core::option",
        source: "module core::option\n",
        semanticTokens: [],
        symbols: [],
      });
      assertEqual(await doFormat(), "readonly", "Prelude tab refuses formatting");
      assertEqual(pendingFormatGates.length, 0, "No /api/format request issued on prelude tab");
      closePreludeTab("stdlib/core/option.dao");

      // 2. Concurrent edit + newer format request supersedes slow in-flight format via requestId & revision.
      selectTab("a.dao");
      const firstFormat = doFormat();
      await waitForCondition(() => pendingFormatGates.length === 1, "first format call");

      const newerUserEdit = "module a\nfn f():i32->999\n";
      updateActiveUserSource(newerUserEdit);

      const secondFormat = doFormat();
      await waitForCondition(() => pendingFormatGates.length === 2, "second format call");

      // First (stale) reply resolves late — must be discarded.
      pendingFormatGates[0].resolve(
        jsonResponse({
          file: "a.dao",
          formatted: "module a\n\nfn f(): i32 -> 1 + 2\n",
          changed: true,
          diagnostics: [],
        }),
      );
      assertEqual(await firstFormat, "superseded", "First format superseded by edit + newer request");
      assertEqual(getActiveSource(), newerUserEdit, "Newer edit untouched by stale reply");

      // Second (current) reply resolves and formats only a.dao.
      const formattedSecond = "module a\n\nfn f(): i32 -> 999\n";
      pendingFormatGates[1].resolve(
        jsonResponse({
          file: "a.dao",
          formatted: formattedSecond,
          changed: true,
          diagnostics: [],
        }),
      );
      assertEqual(await secondFormat, "formatted", "Current format request applies");
      assertEqual(getActiveSource(), formattedSecond, "a.dao receives formatted source");
      assertEqual(getUserFiles()[1].source, "module b\nfn g(): i32 -> 3\n", "b.dao untouched");

      // 3. Switching tabs while /api/format is in flight discards reply rather than clobbering active tab.
      const switchedFormat = doFormat();
      await waitForCondition(() => pendingFormatGates.length === 3, "third format call");
      selectTab("b.dao");
      pendingFormatGates[2].resolve(
        jsonResponse({
          file: "a.dao",
          formatted: "module a\n\nfn f(): i32 -> 0\n",
          changed: true,
          diagnostics: [],
        }),
      );
      assertEqual(await switchedFormat, "superseded", "Format superseded by tab switch");
      assertEqual(getActiveSource(), "module b\nfn g(): i32 -> 3\n", "Tab b.dao untouched");

      // 4. Deleting or resetting a file while /api/format is in flight invalidates the format request,
      // even if a replacement workspace later reuses the same path and source text.
      selectTab("a.dao");
      const sameSourceBeforeReset = getActiveSource();
      const resetSupersededFormat = doFormat();
      await waitForCondition(() => pendingFormatGates.length === 4, "fourth format call");
      setWorkspaceProgram(
        [{ path: "a.dao", source: sameSourceBeforeReset }],
        "a.dao",
      );
      pendingFormatGates[3].resolve(
        jsonResponse({
          file: "a.dao",
          formatted: "module a\n\nfn clobbered(): i32 -> 0\n",
          changed: true,
          diagnostics: [],
        }),
      );
      assertEqual(
        await resetSupersededFormat,
        "superseded",
        "Format across workspace reset superseded even with identical path/source",
      );
      assertEqual(getActiveSource(), sameSourceBeforeReset, "Post-reset buffer preserved");

      // 5. Syntax error diagnostics from /api/format preserve broken buffer and report diagnostics.
      const brokenSource = "module a\nfn broken(: i32 -> 0\n";
      updateActiveUserSource(brokenSource);
      const diagFormat = doFormat();
      await waitForCondition(() => pendingFormatGates.length === 5, "fifth format call");
      pendingFormatGates[4].resolve(
        jsonResponse({
          file: "a.dao",
          formatted: brokenSource,
          changed: false,
          diagnostics: [
            {
              severity: "error",
              file: "a.dao",
              offset: 19,
              length: 1,
              line: 2,
              col: 11,
              message: "expected parameter name",
            },
          ],
        }),
      );
      assertEqual(await diagFormat, "diagnostics", "Syntax error format returns diagnostics");
      assertEqual(getActiveSource(), brokenSource, "Broken source preserved unchanged");
      assertEqual(getWorkspaceDiagnostics().length, 1, "Format diagnostic recorded");
    },
  );
  clearDraft();
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
  clearDraft();
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

/**
 * A completion context over `source` at its end, with the abort the editor
 * performs on a buffer swap under the test's control.
 */
function completionContextFor(source: string): { ctx: CompletionContext; abort: () => void } {
  const state = EditorState.create({ doc: source });
  const ctx = new CompletionContext(state, source.length, false);
  let aborted = false;
  Object.defineProperty(ctx, "aborted", { get: () => aborted });
  return { ctx, abort: () => { aborted = true; } };
}

async function testCompletionSourceSettlesQuietlyOnceAborted(): Promise<void> {
  setWorkspaceProgram([{ path: "app.dao", source: "module app\nfn main(): i32 -> pri" }], "app.dao");
  const source = getActiveSource();
  const options = [{ label: "print", kind: "function", type: "fn(string): void" }];

  // A live query delivers the service's options.
  await withMockFetch(async () => jsonResponse(options), async () => {
    const live = await daoCompletionSource(completionContextFor(source).ctx);
    assert(live !== null, "A live query yields a result");
    assertEqual(live.options[0].label, "print", "A live query yields the service's option");
  });

  // A query aborted while the service is answering delivers nothing.
  const lateAnswer = deferred<Response>();
  await withMockFetch(() => lateAnswer.promise, async () => {
    const { ctx, abort } = completionContextFor(source);
    const pending = daoCompletionSource(ctx);
    abort();
    lateAnswer.resolve(jsonResponse(options));
    assertEqual(await pending, null, "An aborted query drops a late answer");
  });

  // A query aborted before the service refuses it delivers nothing either.
  const lateRefusal = deferred<Response>();
  await withMockFetch(() => lateRefusal.promise, async () => {
    const { ctx, abort } = completionContextFor(source);
    const pending = daoCompletionSource(ctx);
    abort();
    lateRefusal.resolve(jsonResponse({ error: "bad offset" }, 400));
    assertEqual(await pending, null, "An aborted query swallows a late refusal");
  });

  // A live query still reports the refusal.
  await withMockFetch(async () => jsonResponse({ error: "bad offset" }, 400), async () => {
    let reported = "";
    try {
      await daoCompletionSource(completionContextFor(source).ctx);
    } catch (err) {
      reported = String(err);
    }
    assert(reported.includes("bad offset"), "A live query reports the service's refusal");
  });
}

/** The paths and sources of `files`, as one comparable string. */
function contentsOf(files: readonly { path: string; source: string }[] | undefined): string {
  return JSON.stringify((files ?? []).map((file) => [file.path, file.source]));
}

async function testResetAsksBeforeItDiscardsWork(): Promise<void> {
  clearDraft();
  const hello = { name: "hello.dao", document: "hello.dao", files: [{ path: "hello.dao", source: "module hello\n" }] };
  const pipelines = { name: "pipelines.dao", document: "pipelines.dao", files: [{ path: "pipelines.dao", source: "module pipelines\n" }] };
  const loads: string[] = [];
  const serveExamples = async (url: string): Promise<Response> => {
    loads.push(url);
    if (url.endsWith("/hello.dao")) return jsonResponse(hello);
    if (url.endsWith("/pipelines.dao")) return jsonResponse(pipelines);
    return jsonResponse({ error: "no such example" }, 404);
  };
  let asked = 0;
  const refuse = (): boolean => { asked += 1; return false; };
  const accept = (): boolean => { asked += 1; return true; };
  // What every refusal owes: one question, no request, the same files.
  const refuseReset = async (scenario: string): Promise<void> => {
    const askedBefore = asked;
    const loadsBefore = loads.length;
    const shown = contentsOf(getUserFiles());
    await resetWorkspace(refuse);
    assertEqual(asked, askedBefore + 1, `${scenario}: Reset asks once`);
    assertEqual(loads.length, loadsBefore, `${scenario}: a refusal starts no load`);
    assertEqual(contentsOf(getUserFiles()), shown, `${scenario}: a refusal keeps every file's source`);
  };

  await withMockFetch(serveExamples, async () => {
    assertEqual(await loadExampleByName("hello.dao"), "applied", "The example loads");

    // An untouched example resets without a question, with no draft stored
    // and with the draft that choosing an example stores.
    await resetWorkspace(refuse);
    assertEqual(asked, 0, "An untouched example raises no question");
    assertEqual(loads.length, 2, "An untouched example reloads");
    assertEqual(await loadExampleByName("hello.dao", { persistDraft: true }), "applied", "The example loads and stores its draft");
    assert(loadSavedDraft() !== null, "Choosing an example stores a draft");
    await resetWorkspace(refuse);
    assertEqual(asked, 0, "A stored draft equal to the untouched example raises no question");
    assertEqual(loads.length, 4, "That example reloads too");
    const loadsBeforeWork = loads.length;

    // An edited, multi-file workspace: a refusal changes nothing, on the
    // screen or in storage.
    updateActiveUserSource("module hello\n// an edit\n");
    addUserFile("extra.dao", "module extra\n");
    saveDraft();
    const editedFiles = contentsOf(getUserFiles());
    await refuseReset("Edited files");
    assertEqual(contentsOf(loadSavedDraft()?.files), editedFiles, "A refusal keeps the stored draft's sources");

    // An acceptance restores the example and deletes the draft.
    await resetWorkspace(accept);
    assertEqual(asked, 2, "Work raises the question again");
    assertEqual(loads.length, loadsBeforeWork + 1, "An acceptance starts one load");
    assertEqual(loads[loads.length - 1], "/api/examples/hello.dao", "An acceptance reloads the current example");
    assertEqual(contentsOf(getUserFiles()), contentsOf(hello.files), "An acceptance restores the example's files");
    assertEqual(loadSavedDraft(), null, "An acceptance deletes the stored draft");

    // A stored draft that the screen does not show is still work: Reset
    // deletes it.
    updateActiveUserSource("module hello\n// kept only in the draft\n");
    saveDraft();
    const draftOnly = contentsOf(loadSavedDraft()?.files);
    assertEqual(await loadExampleByName("hello.dao"), "applied", "The example loads over the draft");
    await refuseReset("A draft the screen does not show");
    assertEqual(contentsOf(loadSavedDraft()?.files), draftOnly, "A refusal keeps that draft's sources");

    // A stored draft that cannot be decoded may be the only copy of its
    // sources: it raises the question, and a refusal leaves it in storage.
    memoryStorage.clear();
    saveDraft();
    const draftKey = memoryStorage.key(0);
    assert(draftKey !== null, "The draft has a storage key");
    memoryStorage.setItem(draftKey, "{ not a snapshot");
    assertEqual(loadSavedDraft(), null, "The damaged draft does not decode");
    await refuseReset("A draft that cannot be decoded");
    assertEqual(memoryStorage.getItem(draftKey), "{ not a snapshot", "A refusal leaves the damaged draft in storage");

    // Storage that refuses the read says nothing about the draft, so an
    // untouched example whose draft cannot be read raises the question too.
    assertEqual(await loadExampleByName("hello.dao", { persistDraft: true }), "applied", "The example loads and stores its draft");
    assert(!resetWouldDiscardWork(), "With the draft readable, the untouched example holds no work");
    const readItem = memoryStorage.getItem;
    memoryStorage.getItem = (): string | null => { throw new Error("storage refused the read"); };
    try {
      await refuseReset("A draft that storage will not read");
    } finally {
      memoryStorage.getItem = readItem;
    }
    assertEqual(contentsOf(loadSavedDraft()?.files), contentsOf(hello.files), "A refusal leaves that draft in storage");

    // The files of the example loaded last, reopened under no example name,
    // as a snippet opens them: they equal the recorded files, yet Reset
    // would put hello.dao in their place.  So it asks, and an acceptance
    // asks the service for hello.dao.
    clearDraft();
    assertEqual(await loadExampleByName("pipelines.dao"), "applied", "Another example loads");
    assert(!resetWouldDiscardWork(), "That example, untouched and named, holds no work");
    setWorkspaceProgram(pipelines.files, "pipelines.dao", { exampleName: "" });
    assert(resetWouldDiscardWork(), "The same files under no example name are work");
    const loadsBeforeUnnamed = loads.length;
    await resetWorkspace(accept);
    assertEqual(asked, 6, "The unnamed workspace raises the question");
    assertEqual(loads.length, loadsBeforeUnnamed + 1, "The acceptance starts one load");
    assertEqual(loads[loads.length - 1], "/api/examples/hello.dao", "Reset restores hello.dao where no example is named");
    assertEqual(contentsOf(getUserFiles()), contentsOf(hello.files), "The workspace holds hello.dao");
  });
}

async function runAll(): Promise<void> {
  testWorkspaceRevisionIncrementsOnEveryMutation();
  await testMalformedAndPartialSnapshotsAreAtomic();
  await testOutOfOrderExampleRequestsAndEdits();
  await testBootstrapStartupEditsAndGenuineFallback();
  testUndoableReplaceTransactionInvariants();
  await testFormatterTargetsWritableFileAndGuardsStaleReplies();
  await testCompletionSourceSettlesQuietlyOnceAborted();
  await testResetAsksBeforeItDiscardsWork();
  console.log("workspace_state.test: all assertions passed");
}

void runAll().catch((err: unknown) => {
  console.error(err);
  throw err;
});
