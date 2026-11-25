import "./style.css";
import { api, onServiceStatus } from "./api";
import { initAnalysis, scheduleAnalyze } from "./analysis";
import { initDiagnostics } from "./diagnostics";
import {
  onWorkspaceLifecycle,
  setVersionInfo,
  setWorkspaceProgram,
  updateActiveUserSource,
} from "./document";
import { createEditor } from "./editor";
import {
  FALLBACK_FILES,
  initExampleCatalog,
  loadExampleByName,
  resetWorkspace,
} from "./examples";
import { gotoDefAtCursor, initGotoDef } from "./goto_def";
import { hideHoverTooltip, initHover } from "./hover";
import {
  applySnapshot,
  closeSnapshotModal,
  decodeShareToken,
  initSnapshotModal,
  loadSavedDraft,
  scheduleDraftSave,
} from "./persistence";
import {
  clearReferences,
  findReferencesAtCursor,
  initReferences,
} from "./references";
import { doRun } from "./run";
import { initSiteRouter, initTheme, parseLocationRoute } from "./site";
import { initSplitters } from "./splitters";
import {
  cancelInlineFileForms,
  initWorkspaceUi,
  navigateToPosition,
  syncEditorToActiveTab,
} from "./workspace_ui";

// ---------------------------------------------------------------------------
// 1. Theme & Site Router
// ---------------------------------------------------------------------------

initTheme();

// ---------------------------------------------------------------------------
// 2. Editor & Language Tooling Surface
// ---------------------------------------------------------------------------

const editor = createEditor((newSource) => {
  updateActiveUserSource(newSource);
  scheduleAnalyze();
  scheduleDraftSave();
});

initAnalysis(editor);
initDiagnostics((file, offset, length) => {
  void navigateToPosition(file, offset, length);
});
initHover(editor);
initGotoDef(editor);
initReferences(editor);
initWorkspaceUi();
initSplitters();
initSnapshotModal(() => syncEditorToActiveTab());
initSiteRouter((params) => {
  void hydrateWorkspaceQueryParams(params);
});

// ---------------------------------------------------------------------------
// 3. Service Status & Version Metadata
// ---------------------------------------------------------------------------

onServiceStatus((restarting) => {
  document.getElementById("service-status")!.hidden = !restarting;
});

async function loadServiceVersion(): Promise<void> {
  try {
    const info = await api("version", undefined);
    setVersionInfo(info);
  } catch (err) {
    console.error("version check failed:", err);
  }
}

// ---------------------------------------------------------------------------
// 4. Toolbar Controls, Collapsible Panels & Global Shortcuts
// ---------------------------------------------------------------------------

document.getElementById("run-btn")!.addEventListener("click", () => {
  void doRun();
});

document.getElementById("reset-workspace-btn")!.addEventListener("click", () => {
  void resetWorkspace();
});

document.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
    e.preventDefault();
    void doRun();
    return;
  }
  if (e.key === "F12" && e.shiftKey) {
    e.preventDefault();
    void findReferencesAtCursor();
    return;
  }
  if (e.key === "F12") {
    e.preventDefault();
    void gotoDefAtCursor();
    return;
  }
  if (e.key === "Escape") {
    clearReferences();
    hideHoverTooltip();
    closeSnapshotModal();
    cancelInlineFileForms();
  }
});

for (const panel of document.querySelectorAll<HTMLElement>(".collapsible")) {
  panel
    .querySelector(".collapsible-header")!
    .addEventListener("click", () => panel.classList.toggle("expanded"));
}

document.querySelector(".panel-tabs")!.addEventListener("click", (e) => {
  const tab = (e.target as HTMLElement).closest(".tab") as HTMLElement | null;
  if (!tab) return;
  const target = tab.dataset.tab;

  document
    .querySelectorAll(".panel-tabs .tab")
    .forEach((t) => t.classList.toggle("active", t === tab));
  document
    .querySelectorAll("#ir-body .tab-content")
    .forEach((c) =>
      c.classList.toggle("active", c.id === `${target}-output`),
    );
});

// ---------------------------------------------------------------------------
// 5. Initial & Fragment Workspace Hydration (Share URL -> ?example= -> Draft)
// ---------------------------------------------------------------------------

let lastHydratedQueryKey = "";
let hydratingFromQuery = false;
let catalogReadyPromise: Promise<void> | null = null;
let versionReadyPromise: Promise<void> | null = null;

onWorkspaceLifecycle({
  onReset: () => {
    if (!hydratingFromQuery) {
      lastHydratedQueryKey = "";
    }
  },
});

function queryHydrationKey(params: URLSearchParams): string {
  const share = params.get("s") ?? "";
  const example = params.get("example") ?? "";
  return share || example ? `s=${share}&example=${example}` : "";
}

async function hydrateWorkspaceQueryParams(
  params: URLSearchParams,
): Promise<boolean> {
  const key = queryHydrationKey(params);
  if (key === lastHydratedQueryKey) {
    return false;
  }
  lastHydratedQueryKey = key;
  if (!key) {
    return false;
  }

  const shareParam = params.get("s");
  const exampleParam = params.get("example");

  hydratingFromQuery = true;
  try {
    if (versionReadyPromise) {
      await versionReadyPromise;
    }

    if (shareParam) {
      const decoded = await decodeShareToken(shareParam);
      if (decoded) {
        applySnapshot(decoded);
        const select = document.getElementById("example-select") as HTMLSelectElement;
        select.value = decoded.exampleName ?? "";
        syncEditorToActiveTab();
        return true;
      }
    }

    if (exampleParam) {
      if (catalogReadyPromise) {
        await catalogReadyPromise;
      }
      const loaded = await loadExampleByName(exampleParam, { persistDraft: false });
      if (loaded) {
        return true;
      }
    }

    return false;
  } finally {
    hydratingFromQuery = false;
  }
}

async function bootstrapWorkspace(): Promise<void> {
  versionReadyPromise = loadServiceVersion();
  catalogReadyPromise = initExampleCatalog();

  const { params } = parseLocationRoute();
  const hydratedFromUrl = await hydrateWorkspaceQueryParams(params);
  if (hydratedFromUrl) {
    await catalogReadyPromise;
    return;
  }

  await versionReadyPromise;

  const draft = loadSavedDraft();
  if (draft) {
    applySnapshot(draft);
    const select = document.getElementById("example-select") as HTMLSelectElement;
    if (draft.exampleName) select.value = draft.exampleName;
    syncEditorToActiveTab();
    await catalogReadyPromise;
    return;
  }

  await catalogReadyPromise;
  const ok = await loadExampleByName("hello.dao", { persistDraft: false });
  if (!ok) {
    setWorkspaceProgram(FALLBACK_FILES, "hello.dao", { exampleName: "hello.dao" });
    syncEditorToActiveTab();
  }
}

void bootstrapWorkspace();
