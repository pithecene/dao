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
  openPreludeTab,
  renameUserFile,
  requestPlaygroundView,
  selectTab,
  type PreludeTab,
} from "./document";
import { getEditorView, jumpInEditor, loadEditorBuffer } from "./editor";
import { showNotice } from "./notice";
import { saveDraft } from "./persistence";

const preludeCache = new Map<string, PreludeTab>();
let renamingPath: string | null = null;
let creatingNewFile = false;

export function initWorkspaceUi(): void {
  onWorkspaceChange(renderWorkspaceChrome);

  document.getElementById("add-file-btn")!.addEventListener("click", () => {
    creatingNewFile = true;
    renamingPath = null;
    renderWorkspaceChrome();
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
  renderWorkspaceChrome();
}

/** Sync CodeMirror buffer with the currently active tab and analyze. */
export function syncEditorToActiveTab(selection?: {
  offset: number;
  length?: number;
}): void {
  const path = getActivePath();
  const readOnly = isActivePrelude();
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
  renderNewFileSlot();
  renderFileList();
  renderPreludeList();
  renderEditorTabs();
}

function renderNewFileSlot(): void {
  const slot = document.getElementById("new-file-slot")!;
  if (!creatingNewFile) {
    slot.replaceChildren();
    return;
  }

  const existingCount = getUserFiles().length + 1;
  const suggested = `mod${existingCount}.dao`;

  const form = document.createElement("form");
  form.className = "file-inline-form";
  const input = document.createElement("input");
  input.type = "text";
  input.className = "file-inline-input";
  input.value = suggested;
  input.placeholder = "filename.dao";
  input.setAttribute("aria-label", "New file name");

  const commit = () => {
    if (!creatingNewFile) return;
    const raw = input.value.trim();
    creatingNewFile = false;
    if (!raw) {
      renderWorkspaceChrome();
      return;
    }
    const res = addUserFile(raw);
    if (!res.ok) {
      showNotice(res.error);
      renderWorkspaceChrome();
      return;
    }
    saveDraft();
    syncEditorToActiveTab();
    getEditorView()?.focus();
  };

  form.addEventListener("submit", (e) => {
    e.preventDefault();
    commit();
  });
  input.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      e.stopPropagation();
      creatingNewFile = false;
      renderWorkspaceChrome();
    }
  });
  input.addEventListener("blur", commit);

  form.append(input);
  slot.replaceChildren(form);
}

function renderFileList(): void {
  const list = document.getElementById("file-list")!;
  const files = getUserFiles();
  const active = getActivePath();
  const diags = getWorkspaceDiagnostics();

  const items: HTMLLIElement[] = [];
  for (const file of files) {
    const li = document.createElement("li");

    if (renamingPath === file.path) {
      const form = document.createElement("form");
      form.className = "file-inline-form";
      const input = document.createElement("input");
      input.type = "text";
      input.className = "file-inline-input";
      input.value = file.path;

      const finishRename = () => {
        if (renamingPath !== file.path) return;
        const nextRaw = input.value.trim();
        renamingPath = null;
        if (!nextRaw || nextRaw === file.path) {
          renderWorkspaceChrome();
          return;
        }
        const res = renameUserFile(file.path, nextRaw);
        if (!res.ok) {
          showNotice(res.error);
          renderWorkspaceChrome();
          return;
        }
        saveDraft();
        syncEditorToActiveTab();
      };

      form.addEventListener("submit", (e) => {
        e.preventDefault();
        finishRename();
      });
      input.addEventListener("keydown", (e) => {
        if (e.key === "Escape") {
          e.stopPropagation();
          renamingPath = null;
          renderWorkspaceChrome();
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
    li.title = file.path;
    li.addEventListener("click", () => {
      if (file.path === getActivePath()) return;
      selectTab(file.path);
      saveDraft();
      syncEditorToActiveTab();
    });
    li.addEventListener("dblclick", () => {
      renamingPath = file.path;
      creatingNewFile = false;
      renderWorkspaceChrome();
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

    if (errCount > 0 || warnCount > 0) {
      const countBadge = document.createElement("span");
      countBadge.className = `file-diag-count ${errCount > 0 ? "error" : "warning"}`;
      countBadge.textContent = String(errCount > 0 ? errCount : warnCount);
      rightSide.append(countBadge);
    }

    const actions = document.createElement("span");
    actions.className = "file-item-actions";

    const renameBtn = document.createElement("button");
    renameBtn.type = "button";
    renameBtn.className = "icon-btn";
    renameBtn.title = "Rename file";
    renameBtn.textContent = "✎";
    renameBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      renamingPath = file.path;
      creatingNewFile = false;
      renderWorkspaceChrome();
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
          saveDraft();
          syncEditorToActiveTab();
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
      saveDraft();
      syncEditorToActiveTab();
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
