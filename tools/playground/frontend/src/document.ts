import type {
  Diagnostic,
  DocumentSymbol,
  SemanticToken,
  SourceInput,
  VersionResponse,
} from "./generated/tooling_surface";
import { deriveModuleName, normalizeDaoPath, validateUserPath } from "./util";

export interface UserFile {
  path: string;
  source: string;
}

export interface PreludeTab {
  path: string;
  module: string;
  source: string;
  semanticTokens: SemanticToken[];
  symbols: DocumentSymbol[];
}

type WorkspaceListener = () => void;

export interface WorkspaceLifecycleHooks {
  onReset?: () => void;
  onDelete?: (path: string) => void;
  onRename?: (oldPath: string, newPath: string) => void;
}

const DEFAULT_PATH = "main.dao";
const DEFAULT_SOURCE = `module main

fn main(): i32
  print("Hello, Dao!")
  return 0
`;

let userFiles: UserFile[] = [{ path: DEFAULT_PATH, source: DEFAULT_SOURCE }];
let preludeTabs = new Map<string, PreludeTab>();
let activePath = DEFAULT_PATH;
let lastUserPath = DEFAULT_PATH;
let currentExampleName = "";
let workspaceRevision = 0;
let moduleByPath = new Map<string, string>();
let diagnostics: Diagnostic[] = [];
let versionInfo: VersionResponse | null = null;

const listeners: WorkspaceListener[] = [];
const lifecycleHooks: WorkspaceLifecycleHooks[] = [];
let playgroundViewRequester: (() => void) | null = null;

export function onWorkspaceChange(listener: WorkspaceListener): void {
  listeners.push(listener);
}

export function onWorkspaceLifecycle(hooks: WorkspaceLifecycleHooks): void {
  lifecycleHooks.push(hooks);
}

export function onRequestPlaygroundView(fn: () => void): void {
  playgroundViewRequester = fn;
}

export function requestPlaygroundView(): void {
  playgroundViewRequester?.();
}

function notify(): void {
  updateHeaderBadges();
  for (const listener of listeners) {
    listener();
  }
}

function updateHeaderBadges(): void {
  if (typeof document === "undefined") return;
  const modLabel = document.getElementById("module-label");
  const roBadge = document.getElementById("readonly-badge");
  const modName = moduleByPath.get(activePath) ?? "";
  if (modLabel) {
    modLabel.textContent = modName ? `module ${modName}` : "";
  }
  if (roBadge) {
    roBadge.hidden = !isActivePrelude();
  }
}

export function getWorkspaceRevision(): number {
  return workspaceRevision;
}

export function getUserFiles(): readonly UserFile[] {
  return userFiles;
}

export function getPreludeTabs(): ReadonlyMap<string, PreludeTab> {
  return preludeTabs;
}

export function getActivePath(): string {
  return activePath;
}

export function isActivePrelude(): boolean {
  return preludeTabs.has(activePath);
}

export function getActivePreludeTab(): PreludeTab | undefined {
  return preludeTabs.get(activePath);
}

export function getActiveSource(): string {
  const prelude = preludeTabs.get(activePath);
  if (prelude) return prelude.source;
  return userFiles.find((f) => f.path === activePath)?.source ?? "";
}

/** The user file to name as `document` in ProgramRequest / AnalyzeRequest. */
export function getAnalysisDocumentPath(): string {
  if (userFiles.some((f) => f.path === activePath)) {
    return activePath;
  }
  if (userFiles.some((f) => f.path === lastUserPath)) {
    return lastUserPath;
  }
  return userFiles[0].path;
}

export function hasUserFile(path: string): boolean {
  return userFiles.some((f) => f.path === path);
}

export function getCurrentExampleName(): string {
  return currentExampleName;
}

export function setCurrentExampleName(name: string): void {
  currentExampleName = name;
}

/**
 * Construct a program-shaped request carrying every user file in the
 * workspace and naming the active user document.
 */
export function programRequest(overrideActiveSource?: string): {
  files: SourceInput[];
  document: string;
} {
  const docPath = getAnalysisDocumentPath();
  const files = userFiles.map((f) => ({
    path: f.path,
    source:
      overrideActiveSource !== undefined &&
      f.path === activePath &&
      !isActivePrelude()
        ? overrideActiveSource
        : f.source,
  }));
  return { files, document: docPath };
}

/** Replace the workspace's user files (e.g. loading an example or snapshot). */
export function setWorkspaceProgram(
  files: SourceInput[],
  documentPath: string,
  options?: { exampleName?: string; keepPreludeTabs?: boolean },
): void {
  const nextFiles =
    files.length > 0
      ? files.map((f) => ({ path: f.path, source: f.source }))
      : [{ path: DEFAULT_PATH, source: DEFAULT_SOURCE }];

  userFiles = nextFiles;
  ++workspaceRevision;
  if (!options?.keepPreludeTabs) {
    preludeTabs.clear();
  }
  moduleByPath.clear();
  diagnostics = [];
  for (const h of lifecycleHooks) h.onReset?.();

  const validDoc = nextFiles.some((f) => f.path === documentPath)
    ? documentPath
    : nextFiles[0].path;
  activePath = validDoc;
  lastUserPath = validDoc;
  currentExampleName = options?.exampleName ?? "";
  notify();
}

/** Update the source of the currently active user file on editor change. */
export function updateActiveUserSource(source: string): void {
  if (isActivePrelude()) return;
  const target = userFiles.find((f) => f.path === activePath);
  if (target && target.source !== source) {
    target.source = source;
    ++workspaceRevision;
  }
}

/** Switch to an existing user file or already-opened prelude tab. */
export function selectTab(path: string): boolean {
  if (hasUserFile(path)) {
    activePath = path;
    lastUserPath = path;
    notify();
    return true;
  }
  if (preludeTabs.has(path)) {
    activePath = path;
    notify();
    return true;
  }
  return false;
}

/** Open or focus a read-only stdlib prelude file tab. */
export function openPreludeTab(tab: PreludeTab): void {
  preludeTabs.set(tab.path, tab);
  if (tab.module) {
    moduleByPath.set(tab.path, tab.module);
  }
  activePath = tab.path;
  notify();
}

/** Close a read-only prelude tab and return focus to a user file. */
export function closePreludeTab(path: string): void {
  if (!preludeTabs.delete(path)) return;
  for (const h of lifecycleHooks) h.onDelete?.(path);
  if (activePath === path) {
    activePath = hasUserFile(lastUserPath) ? lastUserPath : userFiles[0].path;
  }
  notify();
}

/** Add a new `.dao` user file with a valid `module` declaration. */
export function addUserFile(
  rawPath: string,
  initialSource?: string,
): { ok: true; path: string } | { ok: false; error: string } {
  const path = normalizeDaoPath(rawPath);
  const problem = validateUserPath(path);
  if (problem) return { ok: false, error: problem };
  if (hasUserFile(path)) {
    return { ok: false, error: `'${path}' already exists in workspace.` };
  }
  const mod = deriveModuleName(path);
  const source =
    initialSource ?? `module ${mod}\n\nfn helper(): i32 -> 42\n`;
  userFiles.push({ path, source });
  ++workspaceRevision;
  activePath = path;
  lastUserPath = path;
  notify();
  return { ok: true, path };
}

/** Rename an existing user file in the workspace. */
export function renameUserFile(
  oldPath: string,
  rawNewPath: string,
): { ok: true; path: string } | { ok: false; error: string } {
  const target = userFiles.find((f) => f.path === oldPath);
  if (!target) return { ok: false, error: "File not found." };

  const newPath = normalizeDaoPath(rawNewPath);
  if (newPath === oldPath) return { ok: true, path: oldPath };

  const problem = validateUserPath(newPath);
  if (problem) return { ok: false, error: problem };
  if (hasUserFile(newPath)) {
    return { ok: false, error: `'${newPath}' already exists.` };
  }

  target.path = newPath;
  ++workspaceRevision;
  const oldMod = moduleByPath.get(oldPath);
  if (oldMod !== undefined) {
    moduleByPath.delete(oldPath);
    moduleByPath.set(newPath, oldMod);
  }
  if (activePath === oldPath) activePath = newPath;
  if (lastUserPath === oldPath) lastUserPath = newPath;
  for (const h of lifecycleHooks) h.onRename?.(oldPath, newPath);
  notify();
  return { ok: true, path: newPath };
}

/** Delete a user file when more than one user file exists. */
export function deleteUserFile(path: string): boolean {
  if (userFiles.length <= 1) return false;
  const idx = userFiles.findIndex((f) => f.path === path);
  if (idx < 0) return false;
  userFiles.splice(idx, 1);
  ++workspaceRevision;
  moduleByPath.delete(path);
  for (const h of lifecycleHooks) h.onDelete?.(path);
  if (activePath === path || lastUserPath === path) {
    const fallback = userFiles[Math.min(idx, userFiles.length - 1)].path;
    if (activePath === path) activePath = fallback;
    if (lastUserPath === path) lastUserPath = fallback;
  }
  notify();
  return true;
}

/** Record the module identity reported for a file. */
export function setDocument(nextFile: string, nextModule: string): void {
  if (nextFile) {
    moduleByPath.set(nextFile, nextModule);
  }
  updateHeaderBadges();
  for (const listener of listeners) {
    listener();
  }
}

export function getModuleForFile(path: string): string {
  return moduleByPath.get(path) ?? "";
}

export function setWorkspaceDiagnostics(next: Diagnostic[]): void {
  diagnostics = next;
  notify();
}

export function getWorkspaceDiagnostics(): readonly Diagnostic[] {
  return diagnostics;
}

export function setVersionInfo(info: VersionResponse): void {
  versionInfo = info;
  if (typeof document !== "undefined") {
    const badge = document.getElementById("toolchain-badge");
    if (badge) {
      badge.textContent = `daoc ${info.commit} · LLVM ${info.llvmVersion}`;
      badge.hidden = false;
    }
  }
  notify();
}

export function getVersionInfo(): VersionResponse | null {
  return versionInfo;
}

/** The display path of the currently shown buffer. */
export function documentFile(): string {
  return activePath;
}

/** True if a position with this `file` lies in the currently open editor buffer. */
export function inDocument(positionFile: string): boolean {
  return positionFile === activePath;
}
