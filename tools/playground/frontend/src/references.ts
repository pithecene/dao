import type { EditorView } from "@codemirror/view";
import { api } from "./api";
import { inDocument, isActivePrelude, programRequest } from "./document";
import type { Reference } from "./generated/tooling_surface";
import { getSource } from "./editor";
import { setReferenceMarks } from "./highlighting";
import { showNotice } from "./notice";
import { navigateToPosition } from "./workspace_ui";

let boundView: EditorView | null = null;

export function initReferences(view: EditorView): void {
  boundView = view;

  document
    .getElementById("close-references-btn")!
    .addEventListener("click", () => clearReferences());

  view.dom.addEventListener("click", (e: MouseEvent) => {
    if (!e.altKey) return;
    e.preventDefault();

    const pos = view.posAtCoords({ x: e.clientX, y: e.clientY });
    if (pos === null) return;
    void findReferencesAtOffset(pos);
  });
}

export async function findReferencesAtCursor(): Promise<void> {
  if (!boundView) return;
  await findReferencesAtOffset(boundView.state.selection.main.head);
}

export function clearReferences(): void {
  const drawer = document.getElementById("references-drawer");
  if (drawer) drawer.hidden = true;
  if (boundView) {
    setReferenceMarks(boundView, []);
  }
}

async function findReferencesAtOffset(offset: number): Promise<void> {
  if (!boundView || isActivePrelude()) return;

  try {
    const refs = await api("references", {
      ...programRequest(getSource()),
      offset,
    });
    const docLength = boundView.state.doc.length;
    const here = refs
      .filter(
        (r) =>
          inDocument(r.file) && r.length > 0 && r.offset + r.length <= docLength,
      )
      .map((r) => ({
        from: r.offset,
        to: r.offset + r.length,
        isDefinition: r.isDefinition,
      }));

    setReferenceMarks(boundView, here);

    if (refs.length === 0) {
      clearReferences();
      showNotice("No references found");
      return;
    }

    renderReferencesDrawer(refs);
  } catch (err) {
    console.error("references failed:", err);
  }
}

function renderReferencesDrawer(refs: Reference[]): void {
  const drawer = document.getElementById("references-drawer")!;
  const title = document.getElementById("references-title")!;
  const list = document.getElementById("references-list")!;

  const fileCount = new Set(refs.map((r) => r.file)).size;
  title.textContent = `References (${refs.length} across ${fileCount} ${fileCount === 1 ? "file" : "files"})`;

  const rows = refs.map((ref) => {
    const row = document.createElement("button");
    row.type = "button";
    row.className = "reference-row";

    const badge = document.createElement("span");
    badge.className = `ref-badge ${ref.isDefinition ? "def" : "use"}`;
    badge.textContent = ref.isDefinition ? "def" : "use";

    const loc = document.createElement("span");
    loc.textContent = `${ref.file}:${ref.line}:${ref.col}`;

    row.append(badge, loc);
    row.addEventListener("click", () => {
      void navigateToPosition(ref.file, ref.offset, ref.length);
    });
    return row;
  });

  list.replaceChildren(...rows);
  drawer.hidden = false;
}
