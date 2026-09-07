import "./style.css";
import { createEditor } from "./editor";
import { initAnalysis, scheduleAnalyze } from "./analysis";
import { doRun } from "./run";
import { loadExamples } from "./examples";
import { initHover } from "./hover";
import { initGotoDef } from "./goto_def";
import { initReferences } from "./references";
import { onServiceStatus } from "./api";

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

const editor = createEditor(scheduleAnalyze);
initAnalysis(editor);
initHover(editor);
initGotoDef(editor);
initReferences(editor);

// ---------------------------------------------------------------------------
// Service status (the compiler service restarts after every rebuild)
// ---------------------------------------------------------------------------

onServiceStatus((restarting) => {
  document.getElementById("service-status")!.hidden = !restarting;
});

// ---------------------------------------------------------------------------
// Run button + keyboard shortcut
// ---------------------------------------------------------------------------

document.getElementById("run-btn")!.addEventListener("click", doRun);

document.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
    e.preventDefault();
    doRun();
  }
});

// ---------------------------------------------------------------------------
// Collapsible panels (outline, compiler IR) + IR tab switching
// ---------------------------------------------------------------------------

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
// Load examples (auto-loads hello.dao and triggers first analysis)
// ---------------------------------------------------------------------------

loadExamples();
