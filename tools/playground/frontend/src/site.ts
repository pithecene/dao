import {
  getVersionInfo,
  onRequestPlaygroundView,
  onWorkspaceChange,
} from "./document";
import {
  getCatalog,
  loadExampleByName,
  onCatalogChange,
  openSnippetInPlayground,
  type ExampleCategory,
} from "./examples";
import type { SourceInput } from "./generated/tooling_surface";
import { escapeHtml } from "./util";

export type SiteRoute =
  | "overview"
  | "getting-started"
  | "reference"
  | "examples"
  | "downloads"
  | "playground";

const VALID_ROUTES = new Set<SiteRoute>([
  "overview",
  "getting-started",
  "reference",
  "examples",
  "downloads",
  "playground",
]);

const THEME_STORAGE_KEY = "dao.theme";

let currentRoute: SiteRoute = "overview";
let activeExampleCategory: "All" | ExampleCategory = "All";
let exampleSearchQuery = "";

// ---------------------------------------------------------------------------
// Curated Runnable Snippets for Overview & Reference
// ---------------------------------------------------------------------------

interface SnippetSpec {
  id: string;
  label: string;
  document: string;
  files: SourceInput[];
}

const SNIPPETS: Record<string, SnippetSpec> = {
  hero: {
    id: "hero",
    label: "resource_pipeline.dao",
    document: "resource_pipeline.dao",
    files: [
      {
        path: "resource_pipeline.dao",
        source: `module demo

enum class Reading:
  Sample(wind_kt: i32, gust_kt: i32)
  Missing

fn score(wind: i32): i32 -> wind * 3 + 10

fn evaluate(r: Reading): i32
  match r:
    Reading::Sample(wind_kt, gust_kt):
      return (wind_kt + gust_kt) |> score
    Reading::Missing:
      return 0

fn summarize(): string
  let report: string = ""
  resource memory scratch =>
    let val: i32 = evaluate(Reading::Sample(wind_kt = 12, gust_kt = 6))
    report = "score=" + i32_to_string(val)
  return report

fn main(): i32
  print(summarize())
  return 0
`,
      },
    ],
  },
  unsafePtr: {
    id: "unsafePtr",
    label: "unsafe_ptr.dao",
    document: "unsafe_ptr.dao",
    files: [
      {
        path: "unsafe_ptr.dao",
        source: `module unsafe_ptr

fn increment_cell(ptr: Ptr<i32>): i32
  let out: i32 = 0
  mode unsafe =>
    let cur: i32 = ptr.get()
    ptr.set(cur + 1)
    out = ptr.get()
  return out

fn main(): i32
  let cell: Ptr<i32> = __dao_mem_alloc(size_of<i32>(), align_of<i32>()).cast<i32>()
  let result: i32 = 0
  mode unsafe =>
    cell.set(41)
    result = increment_cell(cell)
  __dao_mem_free(cell.cast<void>())
  print(result)
  return 0
`,
      },
    ],
  },
  resourceDomain: {
    id: "resourceDomain",
    label: "domains.dao",
    document: "domains.dao",
    files: [
      {
        path: "domains.dao",
        source: `module domains

fn collect_squares(limit: i64): Vector<i64>
  let out: Vector<i64> = Vector<i64>::new()
  resource memory scratch =>
    let tmp: string = ""
    let i: i64 = 1
    while i <= limit:
      tmp = tmp + "step;"
      out = out.push(i * i)
      i = i + 1
  // scratch arena is reclaimed; out is promoted via Vector.copy_out
  return out

fn main(): i32
  let vals: Vector<i64> = collect_squares(to_i64(4))
  let idx: i64 = 0
  while idx < vals.length():
    print(vals.get(idx))
    idx = idx + 1
  return 0
`,
      },
    ],
  },
  enumClass: {
    id: "enumClass",
    label: "structured_variants.dao",
    document: "structured_variants.dao",
    files: [
      {
        path: "structured_variants.dao",
        source: `module structured_variants

enum class Expr:
  Const(value: i32)
  Add(left: i32, right: i32)
  Zero

fn eval(e: Expr): i32
  match e:
    Expr::Const(value):
      return value
    Expr::Add(left, right):
      return left + right
    Expr::Zero:
      return 0

fn main(): i32
  let a: Expr = Expr::Add(left = 18, right = 24)
  print(eval(a))
  return 0
`,
      },
    ],
  },
  multifileSnippet: {
    id: "multifileSnippet",
    label: "geometry/ (2 files)",
    document: "main.dao",
    files: [
      {
        path: "geom.dao",
        source: `module geom

class Box:
  w: i32
  h: i32

fn perimeter(b: Box): i32 -> (b.w + b.h) * 2
`,
      },
      {
        path: "main.dao",
        source: `module main

import geom

fn main(): i32
  let frame = geom::Box(7, 5)
  print(geom::perimeter(frame))
  return 0
`,
      },
    ],
  },
};

// ---------------------------------------------------------------------------
// Theme Management
// ---------------------------------------------------------------------------

export function initTheme(): void {
  const saved = localStorage.getItem(THEME_STORAGE_KEY);
  const initial = saved === "light" ? "light" : "dark";
  applyTheme(initial);

  document.getElementById("theme-toggle")!.addEventListener("click", () => {
    const current = document.documentElement.getAttribute("data-theme");
    const next = current === "light" ? "dark" : "light";
    applyTheme(next);
    try {
      localStorage.setItem(THEME_STORAGE_KEY, next);
    } catch {
      // Ignore.
    }
  });
}

function applyTheme(theme: "dark" | "light"): void {
  document.documentElement.setAttribute("data-theme", theme);
  const btn = document.getElementById("theme-toggle");
  if (btn) {
    btn.textContent = theme === "dark" ? "Light" : "Dark";
    btn.title = `Switch to ${theme === "dark" ? "light" : "dark"} theme`;
  }
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

export function parseLocationRoute(): {
  route: SiteRoute;
  params: URLSearchParams;
  subAnchor: string;
} {
  const rawHash = window.location.hash.replace(/^#/, "");
  const searchParams = new URLSearchParams(window.location.search);

  if (!rawHash) {
    if (searchParams.has("s") || searchParams.has("example")) {
      return { route: "playground", params: searchParams, subAnchor: "" };
    }
    return { route: "overview", params: searchParams, subAnchor: "" };
  }

  const [pathPart, queryPart] = rawHash.split("?");
  const [routeCandidate, subAnchor = ""] = pathPart.split("/");
  const mergedParams = new URLSearchParams(searchParams);
  if (queryPart) {
    const hashParams = new URLSearchParams(queryPart);
    hashParams.forEach((v, k) => mergedParams.set(k, v));
  }

  if (VALID_ROUTES.has(routeCandidate as SiteRoute)) {
    return {
      route: routeCandidate as SiteRoute,
      params: mergedParams,
      subAnchor,
    };
  }
  return { route: "overview", params: mergedParams, subAnchor: "" };
}

export function ensurePlaygroundRoute(): void {
  if (currentRoute === "playground") return;
  window.history.pushState(null, "", "#playground");
  applyRoute("playground", "");
}

export function initSiteRouter(): void {
  onRequestPlaygroundView(ensurePlaygroundRoute);
  onCatalogChange(() => {
    if (currentRoute === "examples") {
      renderSiteSection("examples");
    }
  });

  onWorkspaceChange(() => {
    if (currentRoute === "downloads") {
      renderSiteSection("downloads");
    }
  });

  window.addEventListener("hashchange", () => {
    const { route, subAnchor } = parseLocationRoute();
    applyRoute(route, subAnchor);
  });

  const { route, subAnchor } = parseLocationRoute();
  applyRoute(route, subAnchor);
}

function applyRoute(route: SiteRoute, subAnchor: string): void {
  const previousRoute = currentRoute;
  currentRoute = route;

  const isPlayground = route === "playground";
  document.body.classList.toggle("site-mode", !isPlayground);
  document.getElementById("site-view")!.hidden = isPlayground;
  document.getElementById("playground-view")!.hidden = !isPlayground;
  document.getElementById("header-open-playground")!.hidden = isPlayground;

  for (const link of document.querySelectorAll<HTMLElement>(".nav-link")) {
    link.classList.toggle("active", link.dataset.route === route);
  }

  if (!isPlayground) {
    if (previousRoute !== route || !document.getElementById("site-content")!.hasChildNodes()) {
      renderSiteSection(route);
      if (!subAnchor) {
        window.scrollTo({ top: 0 });
      }
    }
    if (subAnchor) {
      const target = document.getElementById(subAnchor);
      target?.scrollIntoView({ behavior: "smooth", block: "start" });
    }
  }
}

// ---------------------------------------------------------------------------
// Site Section Rendering
// ---------------------------------------------------------------------------

function renderSiteSection(route: Exclude<SiteRoute, "playground">): void {
  const container = document.getElementById("site-content")!;
  switch (route) {
    case "overview":
      container.innerHTML = renderOverviewHtml();
      break;
    case "getting-started":
      container.innerHTML = renderGettingStartedHtml();
      break;
    case "reference":
      container.innerHTML = renderReferenceHtml();
      break;
    case "examples":
      container.innerHTML = renderExamplesHtml();
      bindExamplesEvents(container);
      break;
    case "downloads":
      container.innerHTML = renderDownloadsHtml();
      break;
  }
  bindSnippetButtons(container);
}

function bindSnippetButtons(root: HTMLElement): void {
  for (const btn of root.querySelectorAll<HTMLButtonElement>("[data-snippet]")) {
    btn.addEventListener("click", () => {
      const key = btn.dataset.snippet!;
      const runNow = btn.dataset.run === "true";
      const spec = SNIPPETS[key];
      if (spec) {
        openSnippetInPlayground(spec.files, spec.document, runNow);
      }
    });
  }
  for (const btn of root.querySelectorAll<HTMLButtonElement>("[data-open-example]")) {
    btn.addEventListener("click", () => {
      const name = btn.dataset.openExample!;
      void loadExampleByName(name, { switchRoute: true, persistDraft: true });
    });
  }
}

function renderCodeFrame(snippetKey: string, showRun = false): string {
  const spec = SNIPPETS[snippetKey];
  const displaySource = spec.files.map((f) =>
    spec.files.length > 1 ? `// --- ${f.path} ---\n${f.source.trim()}` : f.source.trim(),
  ).join("\n\n");

  return `
    <div class="code-frame">
      <div class="code-frame-header">
        <span>${escapeHtml(spec.label)}</span>
        <div style="display:flex;gap:6px;">
          <button type="button" class="btn btn-xs" data-snippet="${escapeHtml(snippetKey)}">Open in Playground</button>
          ${showRun ? `<button type="button" class="btn btn-xs btn-run" data-snippet="${escapeHtml(snippetKey)}" data-run="true">▶ Run</button>` : ""}
        </div>
      </div>
      <pre><code>${escapeHtml(displaySource)}</code></pre>
    </div>
  `;
}

function renderOverviewHtml(): string {
  return `
    <section class="hero-grid">
      <div>
        <div class="eyebrow">Systems &amp; Structured Compute</div>
        <h1 class="hero-title">Explicit execution contexts, lexical memory domains, and checked numerics.</h1>
        <p class="hero-summary">
          Dao is an indentation-significant programming language compiled via LLVM.
          It separates ordinary control flow (<code>:</code>) from execution and resource
          boundaries (<code>=&gt;</code>), eliminates pointer sigil ambiguity, traps on signed
          overflow by default, and lowers single-source functions to both CPU machine code
          and verified GPU compute kernels.
        </p>
        <div class="hero-actions">
          <a href="#playground" class="btn btn-primary">Launch Multi-File Playground</a>
          <a href="#getting-started" class="btn">Build from Source</a>
          <a href="#reference" class="btn btn-ghost">Language Reference →</a>
        </div>
        <div class="hero-facts">
          <div class="fact-item">
            <span class="fact-label">Compiler Chain</span>
            <span class="fact-value">C++23 Host + Self-Hosted Dao</span>
          </div>
          <div class="fact-item">
            <span class="fact-label">Backends</span>
            <span class="fact-value">LLVM Native &amp; Metal MSL</span>
          </div>
          <div class="fact-item">
            <span class="fact-label">License</span>
            <span class="fact-value">Apache-2.0 WITH LLVM-exception</span>
          </div>
        </div>
      </div>
      <div>
        ${renderCodeFrame("hero", true)}
      </div>
    </section>

    <section>
      <h2 class="section-heading">Architectural Foundations</h2>
      <p class="section-sub">
        Every surface construct in Dao is governed by normative contracts under
        <code>docs/contracts/</code> before compiler implementation lands.
      </p>

      <div class="pillar-grid">
        <article class="pillar-card">
          <div class="eyebrow">Execution Modes &amp; Pointers</div>
          <h3><code>mode unsafe =&gt;</code> &amp; Sigil-Free <code>Ptr&lt;T&gt;</code></h3>
          <p>
            Dao rejects <code>*T</code>, <code>*p</code>, and <code>&amp;x</code> sigil grammars.
            <code>Ptr&lt;T&gt;</code> is the sole pointer type, using ordinary calls
            (<code>new</code>, <code>get</code>, <code>set</code>, <code>offset</code>,
            <code>cast&lt;U&gt;</code>, <code>is_null</code>) with dereference and traversal
            gated lexically by <code>mode unsafe =&gt;</code>.
          </p>
          ${renderCodeFrame("unsafePtr")}
        </article>

        <article class="pillar-card">
          <div class="eyebrow">Allocation Lifetimes</div>
          <h3><code>resource memory name =&gt;</code></h3>
          <p>
            Scoped allocation domains bind arenas lexically without threading allocator
            types through every signature. Leaving a block reclaims all intermediate
            allocations wholesale while promoting escaping values cleanly via compiler-emitted
            <code>copy_out</code> hooks.
          </p>
          ${renderCodeFrame("resourceDomain")}
        </article>

        <article class="pillar-card">
          <div class="eyebrow">Algebraic &amp; Value Data</div>
          <h3><code>enum</code> classifies, <code>class</code> composes, <code>enum class</code> branches</h3>
          <p>
            Fieldless <code>enum</code> handles pure classification; <code>class</code>
            composes value-semantic records; <code>enum class</code> defines closed structured
            alternatives with mandatory named fields and exhaustive <code>match</code> destructuring.
          </p>
          ${renderCodeFrame("enumClass")}
        </article>

        <article class="pillar-card">
          <div class="eyebrow">Modules &amp; Determinism</div>
          <h3>Explicit Module Graph (<code>module</code> &amp; <code>import</code>)</h3>
          <p>
            Every source file declares its own <code>module a::b</code> identity.
            <code>import a::b</code> binds the final segment <code>b</code> locally for
            unambiguous <code>b::Member</code> qualification, and program diagnostics sort
            deterministically independent of input file order.
          </p>
          ${renderCodeFrame("multifileSnippet")}
        </article>
      </div>
    </section>

    <section class="prose-grid">
      <div class="info-panel">
        <h3>Single-Source CPU &amp; Metal Compute</h3>
        <p>
          Dao's typed Mid-level IR (<code>MirModule</code>) lowers pure scalar elementwise
          functions both to native executables via <code>LlvmBackend</code> and to verified
          Metal Shading Language kernels via <code>MslBackend</code> (<code>dao_msl_emit</code>).
        </p>
        <ul>
          <li>Preserves checked <code>i32</code> overflow traps on GPU via 64-bit range checks and an <code>atomic_uint</code> status buffer.</li>
          <li>Emits SHA-256 verified kernel manifests recording module provenance, source coordinates, and effect profiles.</li>
          <li>Zero bespoke kernel syntax or macro duplication.</li>
        </ul>
      </div>

      <div class="info-panel">
        <h3>Shared Compiler Analysis &amp; Self-Hosting</h3>
        <p>
          The compiler preserves an explicit phase boundary:
          <code>lexer → parser → AST → resolve → typecheck → HIR → MIR → LLVM → driver</code>.
        </p>
        <ul>
          <li><strong>One semantic table:</strong> <code>compiler/analysis/tooling_surface.h</code> drives CLI dumps and Playground highlighting, hover, definitions, references, and completions.</li>
          <li><strong>Three-tier permanence:</strong> C++23 bootstrap seed (<code>compiler/</code>) beside the self-hosting Dao compiler (<code>bootstrap/</code>), verified across Stage 1 → Stage 2 → Stage 3 fixed-point crossing.</li>
        </ul>
      </div>
    </section>
  `;
}

function renderGettingStartedHtml(): string {
  return `
    <header class="site-page-header">
      <div class="eyebrow">Toolchain &amp; Workflow</div>
      <h1>Getting Started</h1>
      <p class="lead">
        Build the C++23 seed compiler, standard library, native C runtime, and local
        multi-file playground from source using Conan 2 and CMake.
      </p>
    </header>

    <div class="prose-grid">
      <div class="info-panel">
        <h2>1. Prerequisites</h2>
        <ul>
          <li><code>mise</code> — task runner and pinned tool version manager (<code>Task 3.40+</code>, <code>Conan 2.26+</code>, <code>Node 22</code>)</li>
          <li><code>Clang 21+</code> with C++23 support</li>
          <li><code>CMake 3.30+</code></li>
          <li>Conan 2.x in manifest mode (<code>conanfile.py</code> — never system package workarounds)</li>
        </ul>
      </div>

      <div class="info-panel">
        <h2>2. Build, Test &amp; Serve</h2>
        <div class="code-frame">
          <div class="code-frame-header"><span>Shell</span></div>
          <pre><code>task build        # install Conan deps, configure CMake, build all
task test         # run CTest suite (frontend, IR, LLVM, playground)
task playground   # build frontend &amp; start http://localhost:8090</code></pre>
        </div>
        <p style="margin-top:12px;">
          Build parallelism defaults to <code>min(4, nproc)</code> via <code>DAO_BUILD_JOBS</code>
          in <code>mise.toml</code>. Override locally in gitignored <code>mise.local.toml</code>.
        </p>
      </div>
    </div>

    <div class="info-panel" style="margin-bottom:32px;">
      <h2>3. Driver CLI (<code>daoc</code>)</h2>
      <p>
        The <code>daoc</code> driver exposes every compiler phase directly for inspection,
        verification, and native executable linking:
      </p>
      <table class="data-table">
        <thead>
          <tr>
            <th>Command</th>
            <th>Scope</th>
            <th>Output</th>
          </tr>
        </thead>
        <tbody>
          <tr>
            <td><code>daoc check &lt;root.dao&gt;</code></td>
            <td>Program</td>
            <td>Runs lex, parse, module graph, name resolution, and type checking; prints <code>ok</code> or ordered diagnostics.</td>
          </tr>
          <tr>
            <td><code>daoc build &lt;root.dao&gt; [link-args...]</code></td>
            <td>Program</td>
            <td>Lowers through HIR, MIR, monomorphization, and LLVM object emission; links against <code>libdao_runtime.a</code>.</td>
          </tr>
          <tr>
            <td><code>daoc ast &lt;file.dao&gt;</code></td>
            <td>Single file</td>
            <td>Deterministic pretty-printed Abstract Syntax Tree prior to module assembly.</td>
          </tr>
          <tr>
            <td><code>daoc tokens &lt;root.dao&gt;</code></td>
            <td>Program</td>
            <td>Compiler-classified semantic tokens (<code>decl.function</code>, <code>mode.unsafe</code>, <code>resource.binding</code>, etc.).</td>
          </tr>
          <tr>
            <td><code>daoc resolve &lt;root.dao&gt;</code></td>
            <td>Program</td>
            <td>Declared user symbols and resolved reference spans across modules.</td>
          </tr>
          <tr>
            <td><code>daoc hir &lt;root.dao&gt;</code></td>
            <td>Program</td>
            <td>Typed High-level IR dump.</td>
          </tr>
          <tr>
            <td><code>daoc mir &lt;root.dao&gt;</code></td>
            <td>Program</td>
            <td>Basic-block Mid-level IR after generic monomorphization and domain instrumentation.</td>
          </tr>
          <tr>
            <td><code>daoc llvm-ir &lt;root.dao&gt;</code></td>
            <td>Program</td>
            <td>Target-lowered textual LLVM IR module.</td>
          </tr>
        </tbody>
      </table>
    </div>

    <div class="prose-grid">
      <div class="info-panel">
        <h2>4. Multi-File Module Discovery</h2>
        <p>
          Dao supports two deterministic program assembly modes:
        </p>
        <ul>
          <li>
            <strong>Root-file discovery:</strong>
            <code>daoc build examples/modules/main.dao [--module-root DIR]</code>
            resolves each <code>import a::b</code> to <code>a/b.dao</code> across the root file's
            directory, any <code>--module-root</code> directories, and <code>--stdlib-root</code>.
          </li>
          <li>
            <strong>Explicit file set:</strong>
            <code>daoc build --source main.dao --source shapes.dao [--entry main]</code>
            compiles the exact listed files with zero filesystem search.
          </li>
        </ul>
        <div style="margin-top:14px;">
          <button type="button" class="btn btn-xs btn-primary" data-open-example="modules">Open modules/ in Playground</button>
        </div>
      </div>

      <div class="info-panel">
        <h2>5. Live Compiler + Playground Loop</h2>
        <p>
          When iterating on compiler analysis or frontend features, run:
        </p>
        <div class="code-frame">
          <div class="code-frame-header"><span>Development Server</span></div>
          <pre><code>task playground-dev
# Vite HMR on http://localhost:5173
# C++ compiler service on http://127.0.0.1:8090</code></pre>
        </div>
        <p style="margin-top:12px;">
          Rebuilding <code>dao_playground</code> automatically restarts the backend while the
          frontend retries transient requests seamlessly.
        </p>
      </div>
    </div>
  `;
}

function renderReferenceHtml(): string {
  return `
    <header class="site-page-header">
      <div class="eyebrow">Frozen Surface Summary</div>
      <h1>Language Reference</h1>
      <p class="lead">
        Concise reference to Dao's frozen syntax and semantic rules as specified in
        <code>CONTRACT_SYNTAX_SURFACE.md</code>, <code>CONTRACT_NUMERIC_SEMANTICS.md</code>,
        <code>CONTRACT_EXECUTION_CONTEXTS.md</code>, <code>ADR_ENUM_CLASS.md</code>, and
        <code>ADR_RAW_POINTER_SURFACE.md</code>.
      </p>
    </header>

    <div class="ref-layout">
      <nav class="ref-toc" aria-label="Reference sections">
        <span class="ref-toc-title">On this page</span>
        <a href="#reference/ref-taxonomy">Visual &amp; Arrow Taxonomy</a>
        <a href="#reference/ref-numerics">Types &amp; Checked Numerics</a>
        <a href="#reference/ref-functions">Functions, Lambdas &amp; Pipes</a>
        <a href="#reference/ref-data">Classes, Enums &amp; Match</a>
        <a href="#reference/ref-generics">Generics &amp; Concepts</a>
        <a href="#reference/ref-unsafe">Modes &amp; Raw Pointers</a>
        <a href="#reference/ref-resources">Scoped Resource Domains</a>
        <a href="#reference/ref-generators">Generators &amp; Iteration</a>
      </nav>

      <div class="ref-content">
        <section id="ref-taxonomy" class="ref-section">
          <div class="ref-section-header">
            <h2>1. Visual &amp; Arrow Taxonomy</h2>
          </div>
          <p>
            Dao uses indentation-significant blocks with strict punctuation roles so control flow,
            expressions, namespaces, and execution contexts never blur:
          </p>
          <table class="data-table">
            <thead>
              <tr><th>Form</th><th>Role</th><th>Examples</th></tr>
            </thead>
            <tbody>
              <tr><td><em>(newline + indent)</em></td><td>Block-bodied function suite</td><td><code>fn compute(x: i32): i32</code></td></tr>
              <tr><td><code>:</code></td><td>Control-flow suites, type annotations, type bodies</td><td><code>if x &gt; 0:</code>, <code>while cond:</code>, <code>class Point:</code>, <code>match v:</code></td></tr>
              <tr><td><code>-&gt;</code></td><td>Single yielded expression (functions &amp; lambdas)</td><td><code>fn add(a: i32, b: i32): i32 -&gt; a + b</code>, <code>|x| -&gt; x * 2</code></td></tr>
              <tr><td><code>=&gt;</code></td><td>Semantic execution or resource context entry</td><td><code>mode unsafe =&gt;</code>, <code>resource memory pool =&gt;</code></td></tr>
              <tr><td><code>::</code> vs <code>.</code></td><td>Module/type namespace qualification vs runtime value member</td><td><code>shapes::Rect</code>, <code>Option::Some</code> vs <code>rect.width</code></td></tr>
            </tbody>
          </table>
        </section>

        <section id="ref-numerics" class="ref-section">
          <div class="ref-section-header">
            <h2>2. Types &amp; Checked Numerics</h2>
            <button type="button" class="btn btn-xs" data-open-example="checked_arithmetic.dao">Open checked_arithmetic.dao</button>
          </div>
          <p>
            Fixed-width two's-complement signed integers (<code>i8</code>, <code>i16</code>,
            <code>i32</code>, <code>i64</code>), unsigned integers (<code>u8</code>, <code>u16</code>,
            <code>u32</code>, <code>u64</code>), IEEE 754 binary floats (<code>f32</code>,
            <code>f64</code>), <code>bool</code>, <code>string</code>, and <code>void</code>.
          </p>
          <ul>
            <li><strong>Checked default overflow:</strong> Signed <code>+</code>, <code>-</code>, and <code>*</code> trap deterministically on overflow in every build mode — never undefined behavior or debug-only semantics.</li>
            <li><strong>Explicit overflow families:</strong> <code>wrapping_add/sub/mul</code> (two's-complement wrap), <code>saturating_add/sub/mul</code> (min/max clamp), and <code>checked_add/sub/mul</code> returning <code>Option&lt;T&gt;</code>.</li>
            <li><strong>Strict IEEE 754 floats:</strong> Preserves NaN partial-order comparisons, signed zero, and infinities; optimizer fast-math reassociation is forbidden under default semantics.</li>
            <li><strong>No implicit promotion lattice:</strong> Binary operators require matching concrete types; conversions (<code>to_i64</code>, <code>f64_to_i32</code>, etc.) are explicit and trap on NaN/Inf/out-of-range.</li>
          </ul>
        </section>

        <section id="ref-functions" class="ref-section">
          <div class="ref-section-header">
            <h2>3. Functions, Lambdas &amp; Pipelines</h2>
            <button type="button" class="btn btn-xs" data-open-example="pipelines.dao">Open pipelines.dao</button>
          </div>
          <p>
            Block-bodied functions require explicit <code>return</code> statements (no implicit tail-expression return).
            Expression-bodied functions and lambdas use <code>-&gt;</code>, and <code>|&gt;</code> is a first-class left-associative pipeline operator:
          </p>
          <div class="code-frame">
            <div class="code-frame-header"><span>Functions &amp; Pipelines</span></div>
            <pre><code>module pipes

extern fn puts(msg: Ptr&lt;u8&gt;): i32

fn double(x: i32): i32 -&gt; x * 2
fn step(x: i32): i32
  let shifted: i32 = x + 1
  return shifted

fn run_pipe(x: i32): i32 -&gt; x |&gt; double |&gt; step</code></pre>
          </div>
        </section>

        <section id="ref-data" class="ref-section">
          <div class="ref-section-header">
            <h2>4. Data: <code>class</code>, <code>enum</code>, and <code>enum class</code></h2>
            <button type="button" class="btn btn-xs" data-open-example="enums_payload.dao">Open enums_payload.dao</button>
          </div>
          <ul>
            <li><strong><code>class Name:</code></strong> Open nominal product type with value semantics. Fields (<code>name: Type</code> without <code>let</code>) precede methods (<code>fn m(self): R</code>).</li>
            <li><strong><code>enum Name:</code></strong> Closed set of fieldless atomic variants (<code>Visibility::Public</code>).</li>
            <li><strong><code>enum class Name:</code></strong> Closed set of structured variants where every payload field is named. Constructed with named arguments (<code>Token::Int(value = 42)</code>) and matched exhaustively with <code>::</code> patterns (<code>Token::Int(value):</code>, trailing <code>..</code> wildcard, or <code>as binding</code>).</li>
          </ul>
          ${renderCodeFrame("enumClass")}
        </section>

        <section id="ref-generics" class="ref-section">
          <div class="ref-section-header">
            <h2>5. Generics &amp; Concepts</h2>
            <button type="button" class="btn btn-xs" data-open-example="concepts.dao">Open concepts.dao</button>
          </div>
          <p>
            Type parameters (<code>&lt;T&gt;</code>, <code>&lt;T: Printable&gt;</code>) monomorphize to concrete types.
            <code>concept</code> declares required behavioral methods; <code>derived concept</code> synthesizes structural
            conformance automatically when every field of a class conforms (unless suppressed via <code>deny Concept</code>).
            <code>extend Type as Concept:</code> attaches scoped conformances, including to builtin scalars and <code>string</code>.
          </p>
          <div class="code-frame">
            <div class="code-frame-header"><span>Concept &amp; Extend</span></div>
            <pre><code>concept Describe:
  fn describe(self): string

extend i32 as Describe:
  fn describe(self): string -&gt; "i32=" + self.to_string()

fn show&lt;T: Describe&gt;(v: T): void -&gt; print(v.describe())</code></pre>
          </div>
        </section>

        <section id="ref-unsafe" class="ref-section">
          <div class="ref-section-header">
            <h2>6. Execution Modes (<code>mode unsafe =&gt;</code>) &amp; <code>Ptr&lt;T&gt;</code></h2>
            <button type="button" class="btn btn-xs" data-open-example="unsafe.dao">Open unsafe.dao</button>
          </div>
          <p>
            <code>Ptr&lt;T&gt;</code> is a compiler-standard generic type lowering directly to LLVM <code>ptr</code>.
            Value-level operations (<code>Ptr&lt;T&gt;::new()</code>, <code>p.is_null()</code>, <code>p.cast&lt;U&gt;()</code>)
            are safe anywhere; memory access and pointer arithmetic (<code>p.get()</code>, <code>p.set(v)</code>,
            <code>p.offset(n)</code>) require an enclosing <code>mode unsafe =&gt;</code> block. Note that
            <code>mode unsafe =&gt;</code> never disables checked integer overflow.
          </p>
          ${renderCodeFrame("unsafePtr")}
        </section>

        <section id="ref-resources" class="ref-section">
          <div class="ref-section-header">
            <h2>7. Scoped Resource Domains (<code>resource memory =&gt;</code>)</h2>
            <button type="button" class="btn btn-xs" data-open-example="resource.dao">Open resource.dao</button>
          </div>
          <p>
            <code>resource memory pool =&gt;</code> establishes a lexical allocation domain. Every allocation performed
            while the block is active draws from that domain and is reclaimed on fall-through, <code>break</code>, or
            <code>return</code>. Values stored into outer bindings or returned out of the block are copied into the
            enclosing domain automatically (calling <code>copy_out(self)</code> on owning containers such as
            <code>Vector&lt;T&gt;</code> and <code>HashMap&lt;V&gt;</code>). Coroutine <code>yield</code> inside a
            memory resource block and escaping <code>Generator&lt;T&gt;</code> values are rejected at compile time.
          </p>
          ${renderCodeFrame("resourceDomain")}
        </section>

        <section id="ref-generators" class="ref-section">
          <div class="ref-section-header">
            <h2>8. Generators &amp; Iteration (<code>Generator&lt;T&gt;</code>, <code>yield</code>)</h2>
            <button type="button" class="btn btn-xs" data-open-example="generators.dao">Open generators.dao</button>
          </div>
          <p>
            Functions returning <code>Generator&lt;T&gt;</code> suspend via <code>yield expr</code> and are consumed
            exclusively by <code>for x in gen:</code>. Integers do not implicitly coerce to iterables; standard library
            <code>range(n)</code> is an ordinary generator function returning <code>Generator&lt;i32&gt;</code>.
          </p>
          <div class="code-frame">
            <div class="code-frame-header"><span>Generator Coroutine</span></div>
            <pre><code>module iter_demo

fn evens(count: i32): Generator&lt;i32&gt;
  let i: i32 = 0
  while i &lt; count:
    yield i * 2
    i = i + 1

fn main(): i32
  for n in evens(4):
    print(n)
  return 0</code></pre>
          </div>
        </section>
      </div>
    </div>
  `;
}

function renderExamplesHtml(): string {
  const categories: ("All" | ExampleCategory)[] = [
    "All",
    "Fundamentals",
    "Data & Generics",
    "Systems & Memory",
    "Compute & Search",
    "Multi-file Modules",
  ];

  const items = getCatalog().filter((item) => {
    const matchesCat =
      activeExampleCategory === "All" || item.category === activeExampleCategory;
    const q = exampleSearchQuery.trim().toLowerCase();
    const matchesQuery =
      !q ||
      item.name.toLowerCase().includes(q) ||
      item.summary.toLowerCase().includes(q) ||
      item.category.toLowerCase().includes(q);
    return matchesCat && matchesQuery;
  });

  const pillsHtml = categories
    .map(
      (cat) =>
        `<button type="button" class="filter-pill${cat === activeExampleCategory ? " active" : ""}" data-cat="${escapeHtml(cat)}">${escapeHtml(cat)}</button>`,
    )
    .join("");

  const cardsHtml =
    items.length === 0
      ? `<div class="info-panel"><p>No examples match "${escapeHtml(exampleSearchQuery)}".</p></div>`
      : items
          .map(
            (item) => `
          <article class="example-card">
            <div class="example-card-top">
              <span class="example-name">${escapeHtml(item.isMultifile ? `${item.name}/` : item.name)}</span>
              <span class="badge${item.isMultifile ? " badge-accent" : ""}">${escapeHtml(item.isMultifile ? "multi-file" : item.category)}</span>
            </div>
            <p class="example-desc">${escapeHtml(item.summary)}</p>
            <div class="example-card-footer">
              ${
                item.knownIssue
                  ? `<span class="badge badge-warn" title="${escapeHtml(item.knownIssue)}">regression fixture</span>`
                  : `<span class="fact-label">${escapeHtml(item.category)}</span>`
              }
              <button type="button" class="btn btn-xs btn-primary" data-open-example="${escapeHtml(item.name)}">Open in Playground</button>
            </div>
          </article>
        `,
          )
          .join("");

  return `
    <header class="site-page-header">
      <div class="eyebrow">Verified Corpus</div>
      <h1>Examples Gallery</h1>
      <p class="lead">
        Every example in <code>examples/</code> is continuously verified by
        <code>playground_service_test</code> for full semantic token coverage and golden
        execution output. Click any program to inspect and run it in the workspace.
      </p>
    </header>

    <div class="examples-toolbar">
      <div class="filter-pills" role="tablist" aria-label="Filter examples by category">
        ${pillsHtml}
      </div>
      <input
        id="examples-search"
        type="search"
        class="search-input"
        placeholder="Filter examples…"
        value="${escapeHtml(exampleSearchQuery)}"
        aria-label="Search examples"
      >
    </div>

    <div class="examples-grid">
      ${cardsHtml}
    </div>
  `;
}

function bindExamplesEvents(container: HTMLElement): void {
  for (const pill of container.querySelectorAll<HTMLButtonElement>(".filter-pill")) {
    pill.addEventListener("click", () => {
      activeExampleCategory = pill.dataset.cat as "All" | ExampleCategory;
      renderSiteSection("examples");
    });
  }

  const search = container.querySelector<HTMLInputElement>("#examples-search");
  if (search) {
    search.focus();
    search.setSelectionRange(search.value.length, search.value.length);
    search.addEventListener("input", () => {
      exampleSearchQuery = search.value;
      renderSiteSection("examples");
    });
  }
}

function renderDownloadsHtml(): string {
  const version = getVersionInfo();
  const liveBanner = version
    ? `
      <div class="info-panel" style="margin-bottom:24px;border-color:var(--accent-border);">
        <div class="eyebrow">Connected Compiler Service</div>
        <h3>Active Local Toolchain</h3>
        <p>
          Running <code>daoc</code> commit <code>${escapeHtml(version.commit)}</code>
          backed by <code>LLVM ${escapeHtml(version.llvmVersion)}</code> targeting
          <code>${escapeHtml(version.targetTriple)}</code> (${version.preludeFiles.length} stdlib prelude modules loaded).
        </p>
      </div>
    `
    : "";

  return `
    <header class="site-page-header">
      <div class="eyebrow">Distribution &amp; Verification</div>
      <h1>Downloads &amp; Toolchain</h1>
      <p class="lead">
        Dao ships as a self-contained native compiler, static runtime archive, and
        standard library tree under <code>Apache-2.0 WITH LLVM-exception</code>.
      </p>
    </header>

    ${liveBanner}

    <div class="prose-grid">
      <div class="info-panel">
        <h2>Supported Host Targets</h2>
        <table class="data-table">
          <thead>
            <tr><th>Target</th><th>Architecture &amp; ABI</th><th>Tooling &amp; Backends</th></tr>
          </thead>
          <tbody>
            <tr>
              <td><code>darwin-arm64</code></td>
              <td>macOS Apple Silicon (<code>arm64-apple-darwin</code>)</td>
              <td>Clang 21+, LLVM AArch64 native backend, Metal <code>dao_msl_emit</code> GPU backend</td>
            </tr>
            <tr>
              <td><code>linux-x86_64</code></td>
              <td>Linux x86-64 (<code>glibc</code>, Ubuntu 24.04 baseline)</td>
              <td>Clang 21.1.8, LLVM X86 native backend, procfs-free <code>.init_array</code> runtime, CI crossing gate</td>
            </tr>
          </tbody>
        </table>
      </div>

      <div class="info-panel">
        <h2>Turnkey Install Layout</h2>
        <p>
          <code>daoc</code> resolves its runtime archive and prelude root relative to its own
          install prefix (<code>CMAKE_INSTALL_PREFIX</code> / CPack archive):
        </p>
        <div class="code-frame">
          <div class="code-frame-header"><span>Installed Prefix Layout</span></div>
          <pre><code>&lt;prefix&gt;/
├── bin/
│   └── daoc                     # Compiler driver
├── lib/
│   └── libdao_runtime.a         # Native C ABI runtime
├── share/dao/stdlib/
│   ├── core/                    # Prelude core (Option, Vector, HashMap, overflow…)
│   ├── io/                      # Prelude file &amp; stdio bindings
│   ├── concepts/                # Additional concept modules
│   └── numerics/                # Extended numeric modules
└── share/doc/dao/
    ├── LICENSE                  # Apache-2.0 WITH LLVM-exception
    └── THIRD_PARTY_NOTICES.md   # Third-party dependency notices</code></pre>
        </div>
      </div>
    </div>

    <div class="prose-grid">
      <div class="info-panel">
        <h2>Packaging a Release Prefix</h2>
        <p>
          Produce a relocatable release installation or CPack archive from any clean checkout:
        </p>
        <div class="code-frame">
          <div class="code-frame-header"><span>Release Build &amp; Install</span></div>
          <pre><code>conan install . -pr profiles/clang-21-debug \\
  -pr:b profiles/clang-21-debug \\
  --build=missing -of build/release -s build_type=Release
cmake --preset release
cmake --build build/release -- -j4
cmake --install build/release --prefix ./dao-dist</code></pre>
        </div>
      </div>

      <div class="info-panel">
        <h2>Reproducible Bootstrap Verification</h2>
        <p>
          Dao verifies its self-hosted compiler in <code>bootstrap/</code> through an isolated
          three-stage crossing harness (<code>bootstrap/stage2.sh</code>):
        </p>
        <div class="code-frame">
          <div class="code-frame-header"><span>Stage 1 → Stage 2 → Stage 3 Crossing</span></div>
          <pre><code>task bootstrap-crossing
# Or run with an explicit isolated directory &amp; resource caps:
STAGE2_RUN_DIR=/tmp/dao_crossing \\
  STAGE_STACK_KB=1048576 \\
  STAGE_MEMORY_KB=8388608 \\
  bash bootstrap/stage2.sh build/debug</code></pre>
        </div>
        <p style="margin-top:12px;">
          The harness compiles Stage 1 with the C++ seed, lowers the Dao bootstrap compiler to
          <code>stage2.ll</code>, rebuilds <code>stage3.ll</code> with Stage 2, asserts
          byte-for-byte fixed-point equality (<code>cmp -s stage2.ll stage3.ll</code>), and emits
          <code>result.json</code> plus <code>source_closure.sha256</code>.
        </p>
      </div>
    </div>
  `;
}
