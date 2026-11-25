import "./style.css";
import { api, onServiceStatus } from "./api";
import { initAnalysis, scheduleAnalyze } from "./analysis";
import { initDiagnostics } from "./diagnostics";
import {
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
initSiteRouter();

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
// 5. Initial Workspace Hydration (Share URL -> ?example= -> Saved Draft -> Default)
// ---------------------------------------------------------------------------

async function bootstrapWorkspace(): Promise<void> {
  const versionPromise = loadServiceVersion();
  const catalogPromise = initExampleCatalog();

  const { params } = parseLocationRoute();
  const shareParam = params.get("s");
  const exampleParam = params.get("example");

  await versionPromise;

  if (shareParam) {
    const decoded = await decodeShareToken(shareParam);
    if (decoded) {
      applySnapshot(decoded);
      syncEditorToActiveTab();
      await catalogPromise;
      return;
    }
  }

  if (exampleParam) {
    await catalogPromise;
    const loaded = await loadExampleByName(exampleParam, { persistDraft: false });
    if (loaded) return;
  }

  const draft = loadSavedDraft();
  if (draft) {
    applySnapshot(draft);
    const select = document.getElementById("example-select") as HTMLSelectElement;
    if (draft.exampleName) select.value = draft.exampleName;
    syncEditorToActiveTab();
    await catalogPromise;
    return;
  }

  await catalogPromise;
  const ok = await loadExampleByName("hello.dao", { persistDraft: false });
  if (!ok) {
    setWorkspaceProgram(FALLBACK_FILES, "hello.dao", { exampleName: "hello.dao" });
    syncEditorToActiveTab();
  }
}

void bootstrapWorkspace();
