import "./style.css";
import { onServiceStatus } from "./api";
import { initAnalysis, scheduleAnalyze } from "./analysis";
import { initDiagnostics } from "./diagnostics";
import { updateActiveUserSource } from "./document";
import { createEditor } from "./editor";
import { resetWorkspace, resetWouldDiscardWork } from "./examples";
import { doFormat, initFormat } from "./format";
import { gotoDefAtCursor, initGotoDef } from "./goto_def";
import { hideHoverTooltip, initHover } from "./hover";
import {
  closeSnapshotModal,
  initSnapshotModal,
  scheduleDraftSave,
} from "./persistence";
import {
  clearReferences,
  findReferencesAtCursor,
  initReferences,
} from "./references";
import { doRun } from "./run";
import { initSiteRouter, initTheme } from "./site";
import { initSplitters } from "./splitters";
import { bootstrapWorkspace, hydrateWorkspaceQueryParams } from "./startup";
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
initFormat();
initWorkspaceUi();
initSplitters();
initSnapshotModal(() => syncEditorToActiveTab());
initSiteRouter((params) => {
  void hydrateWorkspaceQueryParams(params);
});

// ---------------------------------------------------------------------------
// 3. Service Status
// ---------------------------------------------------------------------------

onServiceStatus((restarting) => {
  document.getElementById("service-status")!.hidden = !restarting;
});

// ---------------------------------------------------------------------------
// 4. Toolbar Controls, Collapsible Panels & Global Shortcuts
// ---------------------------------------------------------------------------

document.getElementById("format-btn")!.addEventListener("click", () => {
  void doFormat();
});

document.getElementById("run-btn")!.addEventListener("click", () => {
  void doRun();
});

document.getElementById("reset-workspace-btn")!.addEventListener("click", () => {
  // Reset deletes the draft and every file the user added, and nothing
  // brings them back, so it asks first when the workspace holds work.
  if (
    resetWouldDiscardWork() &&
    !window.confirm("Discard your edits and restore the example? This cannot be undone.")
  ) {
    return;
  }
  void resetWorkspace();
});

document.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
    e.preventDefault();
    void doRun();
    return;
  }
  if (e.shiftKey && e.altKey && e.code === "KeyF") {
    e.preventDefault();
    void doFormat();
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
// 5. Initial Workspace Bootstrap
// ---------------------------------------------------------------------------

void bootstrapWorkspace();
