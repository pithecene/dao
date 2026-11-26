import { api } from "./api";
import { doAnalyze } from "./analysis";
import {
  addUserFile,
  closePreludeTab,
  deleteUserFile,
  getActivePath,
  getActivePreludeTab,
  getActiveSource,
  getPreludeTabs,
  getUserFiles,
  getVersionInfo,
  getWorkspaceDiagnostics,
  hasUserFile,
  isActivePrelude,
  onWorkspaceChange,
  onWorkspaceLifecycle,
  openPreludeTab,
  renameUserFile,
  requestPlaygroundView,
  selectTab,
  type PreludeTab,
} from "./document";
import { getEditorView, jumpInEditor, loadEditorBuffer } from "./editor";
import { setTokens } from "./highlighting";
import { showNotice } from "./notice";
import { saveDraft } from "./persistence";

const preludeCache = new Map<string, PreludeTab>();
let renamingPath: string | null = null;
let creatingNewFile = false;

export function initWorkspaceUi(): void {
  onWorkspaceLifecycle({
    onReset: () => {
      creatingNewFile = false;
      renamingPath = null;
    },
  });
  onWorkspaceChange(renderWorkspaceChrome);

  document.getElementById("add-file-btn")!.addEventListener("click", () => {
    if (renamingPath !== null) {
      renamingPath = null;
      renderFileList(true);
    }
    creatingNewFile = true;
    renderNewFileSlot(true);
    const input = document.querySelector<HTMLInputElement>(
      "#new-file-slot .file-inline-input",
    );
    input?.focus();
    input?.select();
  });

  document.getElementById("toggle-sidebar-btn")!.addEventListener("click", () => {
    document.getElementById("workspace-main")!.classList.toggle("sidebar-collapsed");
  });

  renderWorkspaceChrome();
}

export function cancelInlineFileForms(): void {
  if (!creatingNewFile && !renamingPath) return;
  creatingNewFile = false;
  renamingPath = null;
  renderNewFileSlot(true);
  renderFileList(true);
}

/** Sync CodeMirror buffer with the currently active tab and analyze. */
export function syncEditorToActiveTab(selection?: {
  offset: number;
  length?: number;
}): void {
  const view = getEditorView();
  if (!view) return;
  const path = getActivePath();
  const readOnly = isActivePrelude();
  if (!readOnly) {
    setTokens([], view);
  }
  loadEditorBuffer(path, getActiveSource(), readOnly, selection);
  void doAnalyze();
}

/**
 * Navigate to any file position across user workspace files and stdlib
 * prelude files. Switches tabs (or opens a read-only prelude tab) and
 * scrolls the exact span into view.
 */
export async function navigateToPosition(
  file: string,
  offset: number,
  length = 0,
): Promise<void> {
  if (!file) return;
  requestPlaygroundView();

  if (file === getActivePath()) {
    jumpInEditor(offset, length);
    return;
  }

  if (hasUserFile(file)) {
    selectTab(file);
    syncEditorToActiveTab({ offset, length });
    getEditorView()?.focus();
    return;
  }

  if (getPreludeTabs().has(file)) {
    selectTab(file);
    syncEditorToActiveTab({ offset, length });
    getEditorView()?.focus();
    return;
  }

  if (file.startsWith("stdlib/")) {
    await openPreludeFileAt(file, { offset, length });
    return;
  }

  showNotice(`${file}:${offset}`);
}

export async function openPreludeFileAt(
  path: string,
  selection?: { offset: number; length?: number },
): Promise<void> {
  try {
    let tab = preludeCache.get(path);
    if (!tab) {
      const resp = await api("prelude", { path });
      tab = {
        path: resp.file,
        module: resp.module,
        source: resp.source,
        semanticTokens: resp.semanticTokens,
        symbols: resp.symbols,
      };
      preludeCache.set(path, tab);
    }
    openPreludeTab(tab);
    syncEditorToActiveTab(selection);
    getEditorView()?.focus();
  } catch (err) {
    showNotice(`Cannot open ${path}: ${(err as Error).message}`);
  }
}

function renderWorkspaceChrome(): void {
  renderNewFileSlot(false);
  renderFileList(false);
  renderPreludeList();
  renderEditorTabs();
}

function renderNewFileSlot(forceMount: boolean): void {
  const slot = document.getElementById("new-file-slot")!;
  if (!creatingNewFile) {
    if (slot.hasChildNodes()) {
      slot.replaceChildren();
    }
    return;
  }

  // Preserve in-progress typing across background analysis notifications.
  if (!forceMount && slot.querySelector(".file-inline-form")) {
    return;
  }

  const existingCount = getUserFiles().length + 1;
  const suggested = `mod${existingCount}.dao`;

  const form = document.createElement("form");
  form.className = "file-inline-form";
  const input = document.createElement("input");
  input.type = "text";
  classNameAndAttrs(input, suggested);

  let settled = false;
  const commit = () => {
    if (settled || !creatingNewFile) return;
    settled = true;
    const raw = input.value.trim();
    creatingNewFile = false;
    slot.replaceChildren();
    if (!raw) {
      return;
    }
    const res = addUserFile(raw);
    if (!res.ok) {
      showNotice(res.error);
      renderWorkspaceChrome();
      return;
    }
    syncEditorToActiveTab();
    saveDraft();
    getEditorView()?.focus();
  };

  form.addEventListener("submit", (e) => {
    e.preventDefault();
    commit();
  });
  input.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      e.stopPropagation();
      settled = true;
      creatingNewFile = false;
      slot.replaceChildren();
    }
  });
  input.addEventListener("blur", commit);

  form.append(input);
  slot.replaceChildren(form);
}

function classNameAndAttrs(input: HTMLInputElement, suggested: string): void {
  input.className = "file-inline-input";
  input.value = suggested;
  input.placeholder = "filename.dao";
  input.setAttribute("aria-label", "New file name");
}

function updateFileListMetadataInPlace(list: HTMLElement): void {
  const active = getActivePath();
  const diags = getWorkspaceDiagnostics();

  for (const li of list.querySelectorAll<HTMLLIElement>("li.file-item[data-path]")) {
    const path = li.dataset.path!;
    li.classList.toggle("active", path === active);

    const badgeSlot = li.querySelector<HTMLElement>(".file-diag-slot");
    if (!badgeSlot) continue;
    const fileDiags = diags.filter((d) => d.file === path);
    const errCount = fileDiags.filter((d) => d.severity === "error").length;
    const warnCount = fileDiags.filter((d) => d.severity === "warning").length;

    if (errCount > 0 || warnCount > 0) {
      const countBadge = document.createElement("span");
      countBadge.className = `file-diag-count ${errCount > 0 ? "error" : "warning"}`;
      countBadge.textContent = String(errCount > 0 ? errCount : warnCount);
      badgeSlot.replaceChildren(countBadge);
    } else {
      badgeSlot.replaceChildren();
    }
  }
}

function renderFileList(forceRebuild: boolean): void {
  const list = document.getElementById("file-list")!;
  const files = getUserFiles();

  // If an inline rename input is mounted for a file that still exists, update
  // sibling badges/selection in place so we never blur or clobber user typing.
  if (renamingPath !== null && !hasUserFile(renamingPath)) {
    renamingPath = null;
  }
  if (
    !forceRebuild &&
    renamingPath !== null &&
    list.querySelector(`[data-renaming="${CSS.escape(renamingPath)}"]`)
  ) {
    updateFileListMetadataInPlace(list);
    return;
  }

  const active = getActivePath();
  const diags = getWorkspaceDiagnostics();

  const items: HTMLLIElement[] = [];
  for (const file of files) {
    const li = document.createElement("li");

    if (renamingPath === file.path) {
      li.dataset.renaming = file.path;
      const form = document.createElement("form");
      form.className = "file-inline-form";
      const input = document.createElement("input");
      input.type = "text";
      input.className = "file-inline-input";
      input.value = file.path;

      let settled = false;
      const finishRename = () => {
        if (settled || renamingPath !== file.path) return;
        settled = true;
        const nextRaw = input.value.trim();
        renamingPath = null;
        if (!nextRaw || nextRaw === file.path) {
          renderFileList(true);
          return;
        }
        const res = renameUserFile(file.path, nextRaw);
        if (!res.ok) {
          showNotice(res.error);
          renderFileList(true);
          return;
        }
        syncEditorToActiveTab();
        saveDraft();
      };

      form.addEventListener("submit", (e) => {
        e.preventDefault();
        finishRename();
      });
      input.addEventListener("keydown", (e) => {
        if (e.key === "Escape") {
          e.stopPropagation();
          settled = true;
          renamingPath = null;
          renderFileList(true);
        }
      });
      input.addEventListener("blur", finishRename);

      form.append(input);
      li.append(form);
      items.push(li);
      queueMicrotask(() => {
        input.focus();
        input.select();
      });
      continue;
    }

    li.className = `file-item${file.path === active ? " active" : ""}`;
    li.dataset.path = file.path;
    li.title = file.path;
    li.addEventListener("click", () => {
      if (file.path === getActivePath()) return;
      selectTab(file.path);
      syncEditorToActiveTab();
      saveDraft();
    });
    li.addEventListener("dblclick", () => {
      creatingNewFile = false;
      renderNewFileSlot(true);
      renamingPath = file.path;
      renderFileList(true);
    });

    const label = document.createElement("span");
    label.className = "file-item-label";
    if (/\bfn\s+main\s*\(/.test(file.source)) {
      const dot = document.createElement("span");
      dot.className = "entry-dot";
      dot.title = "Declares fn main";
      label.append(dot);
    }
    const nameSpan = document.createElement("span");
    nameSpan.textContent = file.path;
    label.append(nameSpan);

    const fileDiags = diags.filter((d) => d.file === file.path);
    const errCount = fileDiags.filter((d) => d.severity === "error").length;
    const warnCount = fileDiags.filter((d) => d.severity === "warning").length;

    const rightSide = document.createElement("span");
    rightSide.style.display = "inline-flex";
    rightSide.style.alignItems = "center";
    rightSide.style.gap = "4px";

    const badgeSlot = document.createElement("span");
    badgeSlot.className = "file-diag-slot";
    if (errCount > 0 || warnCount > 0) {
      const countBadge = document.createElement("span");
      countBadge.className = `file-diag-count ${errCount > 0 ? "error" : "warning"}`;
      countBadge.textContent = String(errCount > 0 ? errCount : warnCount);
      badgeSlot.append(countBadge);
    }
    rightSide.append(badgeSlot);

    const actions = document.createElement("span");
    actions.className = "file-item-actions";

    const renameBtn = document.createElement("button");
    renameBtn.type = "button";
    renameBtn.className = "icon-btn";
    renameBtn.title = "Rename file";
    renameBtn.textContent = "✎";
    renameBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      creatingNewFile = false;
      renderNewFileSlot(true);
      renamingPath = file.path;
      renderFileList(true);
    });
    actions.append(renameBtn);

    if (files.length > 1) {
      const delBtn = document.createElement("button");
      delBtn.type = "button";
      delBtn.className = "icon-btn";
      delBtn.title = "Delete file";
      delBtn.textContent = "✕";
      delBtn.addEventListener("click", (e) => {
        e.stopPropagation();
        if (deleteUserFile(file.path)) {
          syncEditorToActiveTab();
          saveDraft();
        }
      });
      actions.append(delBtn);
    }

    rightSide.append(actions);
    li.append(label, rightSide);
    items.push(li);
  }

  list.replaceChildren(...items);
}

function renderPreludeList(): void {
  const list = document.getElementById("prelude-list")!;
  const info = getVersionInfo();
  if (!info || info.preludeFiles.length === 0) {
    list.innerHTML = `<li class="no-symbols">Connect service to browse prelude</li>`;
    return;
  }

  const active = getActivePath();
  const items = info.preludeFiles.map((path) => {
    const li = document.createElement("li");
    li.className = `file-item${path === active ? " active" : ""}`;
    li.title = `${path} (read-only stdlib prelude)`;
    const label = document.createElement("span");
    label.className = "file-item-label";
    label.textContent = path.replace(/^stdlib\//, "");
    li.append(label);
    li.addEventListener("click", () => {
      void openPreludeFileAt(path);
    });
    return li;
  });
  list.replaceChildren(...items);
}

function renderEditorTabs(): void {
  const bar = document.getElementById("editor-tabs")!;
  const active = getActivePath();
  const buttons: HTMLElement[] = [];

  for (const file of getUserFiles()) {
    const tab = document.createElement("button");
    tab.type = "button";
    tab.role = "tab";
    tab.ariaSelected = String(file.path === active);
    tab.className = `editor-tab${file.path === active ? " active" : ""}`;
    tab.title = file.path;

    const text = document.createElement("span");
    text.textContent = shortTabName(file.path);
    tab.append(text);

    tab.addEventListener("click", () => {
      if (file.path === getActivePath()) return;
      selectTab(file.path);
      syncEditorToActiveTab();
      saveDraft();
    });
    buttons.push(tab);
  }

  for (const [path] of getPreludeTabs()) {
    const tab = document.createElement("button");
    tab.type = "button";
    tab.role = "tab";
    tab.ariaSelected = String(path === active);
    tab.className = `editor-tab${path === active ? " active" : ""}`;
    tab.title = `${path} (read-only prelude)`;

    const text = document.createElement("span");
    text.textContent = shortTabName(path);
    const ro = document.createElement("span");
    ro.className = "editor-tab-ro";
    ro.textContent = "RO";

    const close = document.createElement("span");
    close.className = "editor-tab-close";
    close.title = "Close prelude tab";
    close.textContent = "✕";
    close.addEventListener("click", (e) => {
      e.stopPropagation();
      const wasActive = getActivePreludeTab()?.path === path;
      closePreludeTab(path);
      if (wasActive) {
        syncEditorToActiveTab();
      }
    });

    tab.append(text, ro, close);
    tab.addEventListener("click", () => {
      if (path === getActivePath()) return;
      selectTab(path);
      syncEditorToActiveTab();
    });
    buttons.push(tab);
  }

  bar.replaceChildren(...buttons);
}

function shortTabName(path: string): string {
  const parts = path.split("/");
  return parts.length > 1 ? parts[parts.length - 1] : path;
}
