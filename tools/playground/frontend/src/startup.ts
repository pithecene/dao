import { api } from "./api";
import {
  onWorkspaceLifecycle,
  setVersionInfo,
  setWorkspaceProgram,
} from "./document";
import {
  FALLBACK_FILES,
  beginExampleLoadGuard,
  captureWorkspaceLoadGuard,
  initExampleCatalog,
  isWorkspaceLoadGuardCurrent,
  loadExampleByName,
  setExampleSelectValue,
} from "./examples";
import {
  applySnapshot,
  decodeShareToken,
  loadSavedDraft,
} from "./persistence";
import { parseLocationRoute } from "./site";
import { syncEditorToActiveTab } from "./workspace_ui";

export type QueryHydrationOutcome = "none" | "applied" | "superseded" | "failed";

let lastHydratedQueryKey = "";
let hydratingFromQuery = false;
let catalogReadyPromise: Promise<void> | null = null;
let versionReadyPromise: Promise<void> | null = null;
let lifecycleRegistered = false;

function ensureStartupLifecycle(): void {
  if (lifecycleRegistered) return;
  lifecycleRegistered = true;
  onWorkspaceLifecycle({
    onReset: () => {
      if (!hydratingFromQuery) {
        lastHydratedQueryKey = "";
      }
    },
  });
}

async function loadServiceVersion(): Promise<void> {
  try {
    const info = await api("version", undefined);
    setVersionInfo(info);
  } catch (err) {
    console.error("version check failed:", err);
  }
}

function queryHydrationKey(params: URLSearchParams): string {
  const share = params.get("s") ?? "";
  const example = params.get("example") ?? "";
  return share || example ? `s=${share}&example=${example}` : "";
}

/**
 * Hydrate workspace from `?s=` share token or `?example=` query parameters,
 * respecting workspace revision and example-request guards across awaits.
 */
export async function hydrateWorkspaceQueryParams(
  params: URLSearchParams,
): Promise<QueryHydrationOutcome> {
  ensureStartupLifecycle();
  const key = queryHydrationKey(params);
  if (!key) {
    lastHydratedQueryKey = "";
    return "none";
  }
  if (key === lastHydratedQueryKey) {
    return "none";
  }
  lastHydratedQueryKey = key;

  const shareParam = params.get("s");
  const exampleParam = params.get("example");
  const startGuard = captureWorkspaceLoadGuard();

  hydratingFromQuery = true;
  try {
    if (versionReadyPromise) {
      await versionReadyPromise;
    }
    if (!isWorkspaceLoadGuardCurrent(startGuard)) {
      return "superseded";
    }

    if (shareParam) {
      const decoded = await decodeShareToken(shareParam);
      if (!isWorkspaceLoadGuardCurrent(startGuard)) {
        return "superseded";
      }
      if (decoded) {
        applySnapshot(decoded);
        setExampleSelectValue(decoded.exampleName ?? "");
        syncEditorToActiveTab();
        return "applied";
      }
    }

    if (exampleParam) {
      if (catalogReadyPromise) {
        await catalogReadyPromise;
      }
      const exampleGuard = beginExampleLoadGuard(startGuard);
      if (!exampleGuard) {
        return "superseded";
      }
      return await loadExampleByName(exampleParam, {
        persistDraft: false,
        guard: exampleGuard,
      });
    }

    return "failed";
  } finally {
    hydratingFromQuery = false;
  }
}

/**
 * Initialize service metadata, example catalog, URL query hydration, local
 * draft restoration, or initial `hello.dao` load. Installs `FALLBACK_FILES`
 * strictly when the initial `hello.dao` load fails (`"failed"`) and the
 * captured `(requestId, revision)` guard proves no user edit or newer load
 * superseded startup.
 */
export async function bootstrapWorkspace(options?: {
  location?: { hash?: string; search?: string };
}): Promise<void> {
  ensureStartupLifecycle();
  const bootGuard = captureWorkspaceLoadGuard();

  versionReadyPromise = loadServiceVersion();
  catalogReadyPromise = initExampleCatalog();

  const { params } = parseLocationRoute(options?.location);
  const urlOutcome = await hydrateWorkspaceQueryParams(params);
  if (urlOutcome === "applied" || urlOutcome === "superseded") {
    await catalogReadyPromise;
    return;
  }

  const postUrlGuard =
    urlOutcome === "failed"
      ? {
          requestId: captureWorkspaceLoadGuard().requestId,
          revision: bootGuard.revision,
        }
      : bootGuard;
  if (!isWorkspaceLoadGuardCurrent(postUrlGuard)) {
    await catalogReadyPromise;
    return;
  }

  await versionReadyPromise;
  if (!isWorkspaceLoadGuardCurrent(postUrlGuard)) {
    await catalogReadyPromise;
    return;
  }

  const draft = loadSavedDraft();
  if (draft) {
    applySnapshot(draft);
    if (draft.exampleName) {
      setExampleSelectValue(draft.exampleName);
    }
    syncEditorToActiveTab();
    await catalogReadyPromise;
    return;
  }

  await catalogReadyPromise;
  const startupLoadGuard = beginExampleLoadGuard(postUrlGuard);
  if (!startupLoadGuard) {
    return;
  }

  const outcome = await loadExampleByName("hello.dao", {
    persistDraft: false,
    guard: startupLoadGuard,
  });
  if (
    outcome === "failed" &&
    isWorkspaceLoadGuardCurrent(startupLoadGuard)
  ) {
    setWorkspaceProgram(FALLBACK_FILES, "hello.dao", {
      exampleName: "hello.dao",
    });
    setExampleSelectValue("hello.dao");
    syncEditorToActiveTab();
  }
}
