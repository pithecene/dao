import { api } from "./api";
import {
  getCurrentExampleName,
  getUserFiles,
  getWorkspaceRevision,
  requestPlaygroundView,
  setWorkspaceProgram,
} from "./document";
import type { ExampleEntry, SourceInput } from "./generated/tooling_surface";
import { showNotice } from "./notice";
import { clearDraft, readStoredDraft, saveDraft } from "./persistence";
import { syncEditorToActiveTab } from "./workspace_ui";
import { doRun } from "./run";

export type ExampleCategory =
  | "Fundamentals"
  | "Data & Generics"
  | "Systems & Memory"
  | "Compute & Search"
  | "Multi-file Modules";

export type ExampleLoadOutcome = "applied" | "superseded" | "failed";

export interface WorkspaceLoadGuard {
  readonly requestId: number;
  readonly revision: number;
}

export interface CatalogItem {
  name: string;
  category: ExampleCategory;
  summary: string;
  isMultifile: boolean;
  knownIssue?: string;
}

interface ParsedMeta {
  category: ExampleCategory;
  summary: string;
  isMultifile: boolean;
  knownIssue?: string;
}

// Compact pipe-delimited catalog metadata: name | category | flags | summary | optional issue.
const CATALOG_SPEC = `
hello.dao|Fundamentals|single|Minimal Dao entry module printing to standard output.
arithmetic.dao|Fundamentals|single|Fixed-width integer and IEEE 754 floating-point arithmetic.
math.dao|Fundamentals|single|Numeric helper functions, expressions, and comparisons.
i64.dao|Fundamentals|single|64-bit signed integer arithmetic and string formatting.
conversions.dao|Fundamentals|single|Explicit numeric widening, narrowing, and float-int conversions.
checked_arithmetic.dao|Fundamentals|single|Explicit checked_add/sub/mul returning Option<T> alongside wrapping and saturating ops.
control_flow.dao|Fundamentals|single|Indentation-significant if / else suites and early returns.
loops.dao|Fundamentals|single|Structured while and for ... in loops with break control.
strings.dao|Fundamentals|single|UTF-8 string literals, concatenation, and byte length.
text_builder.dao|Fundamentals|single|Incremental text assembly using standard library helpers.
collections_and_text.dao|Compute & Search|single|Expanded Vector operations, overflow-safe stepped ranges, Builder helpers, and integer parsing.
pipelines.dao|Fundamentals|single|First-class left-associative |> pipelines over expression-bodied functions.
lambdas.dao|Fundamentals|single|Expression lambdas (|x| -> ...), function types, and higher-order composition.
structs.dao|Data & Generics|single|Nominal class declarations with value semantics and field access.
methods.dao|Data & Generics|single|Instance methods with bare self, static :: constructors, and generic classes.
enums.dao|Data & Generics|single|Fieldless closed enum variants for classification and equality.
enums_payload.dao|Data & Generics|single|enum class structured alternatives with named fields and exhaustive match destructuring.
generic_enums.dao|Data & Generics|single|Parameterized enum class types such as Option<T> and Result<T, E>.
match.dao|Data & Generics|single|Exhaustive pattern matching over closed variants and ? error propagation.
generics.dao|Data & Generics|single|Generic functions and classes, type argument inference, and explicit specialization.
generic_bounds.dao|Data & Generics|single|Constrained type parameters (<T: Concept>) with monomorphized static dispatch.
concepts.dao|Data & Generics|single|Behavioral concepts and module-scoped extend T as Concept: conformances on builtins.
derived_concepts.dao|Data & Generics|single|Structural derived concept conformance synthesized transitively across class fields.
resource.dao|Systems & Memory|single|Scoped resource memory domains with wholesale arena reclamation and automatic copy_out promotion.
unsafe.dao|Systems & Memory|single|Sigil-free Ptr<T> allocation, cast<T>(), and dereference inside mode unsafe =>.
raw_memory.dao|Systems & Memory|single|Low-level pointer offset traversal and manual buffer management.|Tracked MIR concreteness regression input
generators.dao|Compute & Search|single|Resumable Generator<T> coroutines suspending with yield and consumed via for ... in.
iteration.dao|Compute & Search|single|Composable generator pipelines and range-driven loops.
vectors.dao|Compute & Search|single|Growable prelude Vector<T> operations with value-semantic ownership.
vector_aliasing.dao|Compute & Search|single|Independent copies and alias-free updates across Vector<T> bindings.
hashmap.dao|Compute & Search|single|Prelude HashMap<V> insertion, lookup, and Option<V> probing.
hashmap_aliasing.dao|Compute & Search|single|Value-isolated HashMap<V> mutation and domain escape behavior.
astar.dao|Compute & Search|single|Grid A* pathfinding combining classes, vectors, loops, and structured state.
forecast_plans.dao|Compute & Search|single|Point-in-time heterogeneous DAG verifier & single-source CPU / Metal kernel workload.
resource_pipeline.dao|Systems & Memory|single|The site's opening program: an enum class reading scored through a pipeline inside a resource memory domain.
unsafe_ptr.dao|Systems & Memory|single|One heap cell allocated and cast outside mode unsafe =>, then written and read through Ptr<T> inside it.
domains.dao|Systems & Memory|single|A Vector<i64> built inside a resource memory domain and copied out as the domain ends.
structured_variants.dao|Data & Generics|single|An enum class expression evaluated by an exhaustive match over its named fields.
geometry|Multi-file Modules|multi|A class and a function that the entry module reaches through an import.
modules|Multi-file Modules|multi|Three-file program (main.dao, shapes.dao, report.dao) demonstrating module identity and :: imports.
`.trim();

const EXAMPLE_META = new Map<string, ParsedMeta>(
  CATALOG_SPEC.split("\n").map((line) => {
    const [name, category, mode, summary, knownIssue] = line.split("|");
    return [
      name,
      {
        category: category as ExampleCategory,
        summary,
        isMultifile: mode === "multi",
        knownIssue: knownIssue || undefined,
      },
    ];
  }),
);

export const FALLBACK_FILES: SourceInput[] = [
  {
    path: "hello.dao",
    source: `module hello

fn main(): i32
  print("Hello, Dao!")
  return 0
`,
  },
];

let catalog: CatalogItem[] = buildCatalogFromEntries([]);
const catalogListeners: (() => void)[] = [];
const boundCatalogSelects = new WeakSet<HTMLSelectElement>();
let latestExampleRequestId = 0;

export function onCatalogChange(fn: () => void): void {
  catalogListeners.push(fn);
}

export function getCatalog(): readonly CatalogItem[] {
  return catalog;
}

/** Snapshot the current example-request counter and workspace revision. */
export function captureWorkspaceLoadGuard(): WorkspaceLoadGuard {
  return {
    requestId: latestExampleRequestId,
    revision: getWorkspaceRevision(),
  };
}

/** True when neither a newer example load nor a workspace mutation has occurred. */
export function isWorkspaceLoadGuardCurrent(guard: WorkspaceLoadGuard): boolean {
  return (
    guard.requestId === latestExampleRequestId &&
    guard.revision === getWorkspaceRevision()
  );
}

/**
 * Claim the next example-request slot when `expectedGuard` (if given) has not
 * already been superseded by a newer request or workspace edit.
 */
export function beginExampleLoadGuard(
  expectedGuard?: WorkspaceLoadGuard,
): WorkspaceLoadGuard | null {
  if (expectedGuard && !isWorkspaceLoadGuardCurrent(expectedGuard)) {
    return null;
  }
  const revision = expectedGuard?.revision ?? getWorkspaceRevision();
  const requestId = ++latestExampleRequestId;
  return { requestId, revision };
}

function getExampleSelect(): HTMLSelectElement | null {
  if (typeof document === "undefined") return null;
  return document.getElementById("example-select") as HTMLSelectElement | null;
}

export function setExampleSelectValue(value: string): void {
  const select = getExampleSelect();
  if (select) {
    select.value = value;
  }
}

function buildCatalogFromEntries(entries: ExampleEntry[]): CatalogItem[] {
  if (entries.length === 0) {
    return Array.from(EXAMPLE_META.entries()).map(([name, meta]) => ({
      name,
      category: meta.category,
      summary: meta.summary,
      isMultifile: meta.isMultifile,
      knownIssue: meta.knownIssue,
    }));
  }

  return entries.map((entry) => {
    const meta = EXAMPLE_META.get(entry.name);
    return {
      name: entry.name,
      category:
        meta?.category ??
        (entry.isMultifile ? "Multi-file Modules" : "Fundamentals"),
      summary:
        meta?.summary ??
        (entry.isMultifile
          ? "Multi-file Dao program."
          : "Runnable Dao example program."),
      isMultifile: entry.isMultifile,
      knownIssue: meta?.knownIssue,
    };
  });
}

function populateExampleDropdown(): void {
  const select = getExampleSelect();
  if (!select) return;
  const current = getCurrentExampleName();

  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = "— load example —";

  const categories: ExampleCategory[] = [
    "Multi-file Modules",
    "Fundamentals",
    "Data & Generics",
    "Systems & Memory",
    "Compute & Search",
  ];

  const groups: HTMLElement[] = [placeholder];
  for (const cat of categories) {
    const items = catalog.filter((item) => item.category === cat);
    if (items.length === 0) continue;
    const group = document.createElement("optgroup");
    group.label = cat;
    for (const item of items) {
      const opt = document.createElement("option");
      opt.value = item.name;
      opt.textContent = item.isMultifile ? `${item.name}/ (multi-file)` : item.name;
      group.append(opt);
    }
    groups.push(group);
  }

  select.replaceChildren(...groups);
  select.value = current;
}

/** Fetch `/api/examples`, populate toolbar dropdown, and wire change handler. */
export async function initExampleCatalog(): Promise<void> {
  const select = getExampleSelect();
  populateExampleDropdown();

  if (select && !boundCatalogSelects.has(select)) {
    boundCatalogSelects.add(select);
    select.addEventListener("change", () => {
      const name = select.value;
      if (!name) return;
      void loadExampleByName(name, { persistDraft: true });
    });
  }

  try {
    const { examples } = await api("examples", undefined);
    catalog = buildCatalogFromEntries(examples);
    populateExampleDropdown();
    for (const fn of catalogListeners) fn();
  } catch (err) {
    console.error("failed to fetch examples list:", err);
  }
}

/**
 * Load an example (single-file or multi-file) into the workspace by name.
 * Distinguishes `"applied"`, `"superseded"` (discarded because a newer request
 * or user workspace edit occurred), and `"failed"` (network/HTTP error while
 * the workspace guard remained current).
 */
export async function loadExampleByName(
  name: string,
  options?: {
    switchRoute?: boolean;
    persistDraft?: boolean;
    guard?: WorkspaceLoadGuard;
    expectedGuard?: WorkspaceLoadGuard;
  },
): Promise<ExampleLoadOutcome> {
  const guard =
    options?.guard ?? beginExampleLoadGuard(options?.expectedGuard);
  if (!guard || !isWorkspaceLoadGuardCurrent(guard)) {
    return "superseded";
  }

  try {
    const data = await api("example", { name });
    if (!isWorkspaceLoadGuardCurrent(guard)) {
      setExampleSelectValue(getCurrentExampleName());
      return "superseded";
    }

    setWorkspaceProgram(data.files, data.document, { exampleName: name });
    loadedExample = { name, files: filesFingerprint(data.files) };
    setExampleSelectValue(name);
    if (options?.switchRoute) {
      requestPlaygroundView();
    }
    syncEditorToActiveTab();
    if (options?.persistDraft) {
      saveDraft();
    }
    return "applied";
  } catch (err) {
    if (!isWorkspaceLoadGuardCurrent(guard)) {
      return "superseded";
    }
    console.error("failed to load example:", err);
    setExampleSelectValue(getCurrentExampleName());
    showNotice(`Could not load example '${name}'`);
    return "failed";
  }
}

/** The files' paths and sources as one comparable string. */
function filesFingerprint(files: readonly SourceInput[]): string {
  return JSON.stringify(files.map((file) => [file.path, file.source]));
}

// The last example the service delivered into the workspace, and its files
// as delivered.
let loadedExample: { name: string; files: string } | null = null;

/** The example that Reset restores. */
function resetTarget(): string {
  return getCurrentExampleName() || "hello.dao";
}

/**
 * Whether Reset would throw work away.  It would not only where the files on
 * screen are the files of the example it restores, as the service delivered
 * them, and the stored draft, which Reset deletes, holds nothing else.
 */
export function resetWouldDiscardWork(): boolean {
  const onScreen = filesFingerprint(getUserFiles());
  const untouched =
    loadedExample !== null && loadedExample.name === resetTarget() && loadedExample.files === onScreen;
  // A stored draft that cannot be read may hold the only copy of its
  // sources, so it counts as more than the screen shows.
  const stored = readStoredDraft();
  const draftHoldsMore =
    stored.kind === "unreadable" ||
    (stored.kind === "draft" && filesFingerprint(stored.snapshot.files) !== onScreen);
  return !untouched || draftHoldsMore;
}

const RESET_QUESTION = "Discard your edits and restore the example? This cannot be undone.";

/**
 * Reset the workspace: discard the local draft and reload the active example
 * or hello.dao.  Where that would throw work away, `confirmDiscard` is asked
 * first, and a refusal leaves the workspace and the draft as they are.
 */
export async function resetWorkspace(
  confirmDiscard: (question: string) => boolean,
): Promise<void> {
  if (resetWouldDiscardWork() && !confirmDiscard(RESET_QUESTION)) return;
  clearDraft();
  const target = resetTarget();
  const resetGuard = beginExampleLoadGuard();
  if (!resetGuard) return;

  const outcome = await loadExampleByName(target, {
    persistDraft: false,
    guard: resetGuard,
  });
  if (outcome === "superseded") {
    return;
  }
  if (outcome === "failed" && isWorkspaceLoadGuardCurrent(resetGuard)) {
    if (getWorkspaceRevision() === 0) {
      setWorkspaceProgram(FALLBACK_FILES, "hello.dao", {
        exampleName: "hello.dao",
      });
      setExampleSelectValue("hello.dao");
      syncEditorToActiveTab();
    } else {
      setExampleSelectValue(getCurrentExampleName());
    }
  }
  showNotice("Workspace reset");
}

/** Load an inline documentation code snippet directly into the Playground. */
export function openSnippetInPlayground(
  files: SourceInput[],
  documentPath: string,
  runImmediately = false,
): void {
  ++latestExampleRequestId;
  setWorkspaceProgram(files, documentPath, { exampleName: "" });
  setExampleSelectValue("");
  requestPlaygroundView();
  syncEditorToActiveTab();
  saveDraft();
  if (runImmediately) {
    void doRun();
  }
}
