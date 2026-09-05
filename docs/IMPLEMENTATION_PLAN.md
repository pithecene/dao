# Implementation Plan — Dao

This document is explanatory and sequencing-oriented. It captures the
concrete implementation tasks, toolchain decisions, and delivery order
for the Dao compiler project. It is subordinate to `CLAUDE.md` and
`docs/contracts/`.

## Frozen Toolchain Decisions

### Language and compiler

- Host implementation language: **C++23**
- Primary compiler: **clang++ 21+** (first-class; `profiles/clang-21-debug`
  pins `compiler.version=21`)
- Secondary compiler: **GCC 13+** (best-effort; does not block progress)
- GCC compatibility analysis deferred until enough code exists to
  benchmark compile times, diagnostic quality, and C++23 feature
  coverage

### Build system

- **CMake** with `cmake_minimum_required(VERSION 3.30)`
- CMake presets for common configurations

### Package manager

- **Conan 2.x** in manifest mode
- `conanfile.py` with custom logic for LLVM option configuration
- Compiler profiles managed via Conan profiles
  (currently `profiles/clang-21-debug`; e.g. `profiles/gcc-13-release`
  later)
- One `conan.lock` per profile, colocated under `profiles/`
  (e.g. `profiles/clang-21-debug.lock`); lockfiles are
  configuration-specific and must not be shared across profiles
- LLVM is managed by Conan like all other dependencies

### Test framework

- **boost-ext/ut 2.3.1** — single-header, macro-free, C++20/23 native
- Managed via Conan
- Test files live alongside source: `*_test.cpp` next to `*.cpp`
- `testdata/` holds fixtures and golden files only

### Formatting and static analysis

- `.clang-format`: LLVM base, 2-space indent, 100-column limit,
  no bin packing, pointer-left, no short control statements
- `.clang-tidy`: `modernize-*`, `performance-*`, `readability-*`

### CI

Status: **not implemented** — no CI pipeline exists in the repository.
Verification is local (`task test`, `task bootstrap-test`).  The design
below is the plan of record.

- **Earthly** for reproducible, cacheable builds
- GitHub Actions as the trigger/runner; Earthly defines the actual
  build pipeline
- Earthly targets produce Docker cache layers so LLVM and Conan
  dependency builds are cached across runs (the primary bottleneck
  without caching is LLVM from-source compilation at 60–90 min)
- Pipeline: checkout → `earthly +build` → `earthly +test`
- CI must verify the CMake version satisfies the `cmake_minimum_required`
  floor before building
- Ubuntu only initially; macOS deferred
- Previous GitHub Actions-only CI was removed due to impractical
  build times without layer caching

## Binary Architecture

### `daoc` — dedicated compiler

The compiler is a standalone binary with focused responsibilities:

- `daoc lex <file>` — emit token stream
- `daoc parse <file>` — emit AST
- `daoc ast <file>` — pretty-print AST
- `daoc build <file>` — compile to object/executable (later)

### `dao` — toolchain orchestrator (reserved)

Reserved for future toolchain commands. Not implemented in the initial
tasks. Potential future subcommands: `dao fmt`, `dao lsp`, `dao test`.

### Tool binaries

Tools under `tools/` (playground, LSP, formatter) get their own binaries
or server processes, kept separate from the compiler binary to avoid
dependency contamination.

## Task Sequence

### Task 0 — Toolchain and Environment

**Objective**: Establish a buildable, testable, formatted C++23 project
with a skeleton compiler driver.

Deliverables:

- `CMakeLists.txt` (root) — top-level build with C++23 target
- `CMakePresets.json` — presets for clang debug/release
- `conanfile.py` — declares boost-ext/ut, cpp-httplib, nlohmann_json, llvm-core
- `profiles/clang-21-debug` — Conan profile for primary dev
- `profiles/clang-21-debug.lock` — committed lockfile for primary profile
- `.clang-format` — LLVM base + Dao overrides
- `.clang-tidy` — strict modernize/performance/readability checks
- `.gitignore` — updated for build/, Conan output
- `.github/workflows/build.yml` — CI pipeline (not delivered; see CI
  status above)
- `compiler/driver/main.cpp` — skeleton driver that reads a source file
  and exits
- `compiler/driver/CMakeLists.txt` — builds `daoc`

Exit criteria:

- `cmake --build build` succeeds
- the `daoc` binary produced by the build reads a file and exits cleanly
- `clang-format` and `clang-tidy` pass on all source
- CI runs green on push (pending CI)

### Task 1 — Lexer

**Objective**: Translate `spec/grammar/dao.lex` into a working
indentation-aware lexer.

Deliverables:

- `compiler/frontend/lexer/token.h` — token kinds, source spans
- `compiler/frontend/lexer/lexer.h` / `lexer.cpp` — lexer
  implementation
- `compiler/frontend/lexer/lexer_test.cpp` — tests against syntax
  probes
- `compiler/frontend/diagnostics/source.h` — source buffer
  abstraction
- `compiler/frontend/diagnostics/diagnostic.h` — diagnostic
  structures

Requirements:

- indentation-sensitive: emit INDENT / DEDENT tokens
- tabs are illegal
- recognize all tokens from `spec/grammar/dao.lex`
- token kinds must carry enough information for trivial mapping to the
  semantic token taxonomy in `CONTRACT_LANGUAGE_TOOLING.md`
- run lexer against every file in `spec/syntax_probes/` and
  `examples/`

Exit criteria:

- all syntax probes and examples lex without error
- INDENT/DEDENT pairs are balanced
- token spans are accurate
- tests pass

### Task 2 — Parser

**Objective**: Parse the grammar in `spec/grammar/dao.ebnf` into an AST.

Deliverables:

- `compiler/frontend/ast/ast.h` — AST node definitions
- `compiler/frontend/parser/parser.h` / `parser.cpp` — recursive
  descent parser
- `compiler/frontend/parser/parser_test.cpp` — tests against syntax
  probes

Target AST nodes (minimum):

- `File`
- `Import`
- `FunctionDecl` (block-bodied and expression-bodied)
- `StructDecl`, `AliasDecl`
- `LetStatement`
- `Assignment`
- `IfStatement`, `WhileStatement`, `ForStatement`
- `ModeBlock`, `ResourceBlock`
- `ReturnStatement`
- `BinaryExpr`, `UnaryExpr`, `CallExpr`, `IndexExpr`, `FieldExpr`
- `Lambda`
- `PipeExpr`
- `Literal` (integer, float, string, bool)
- `ListLiteral`
- `Identifier`
- `Type` (named, pointer, parameterized)

Requirements:

- all productions from `spec/grammar/dao.ebnf`
- source spans on every AST node
- diagnostics for syntax errors with readable source reporting
- the parser must not invent semantics beyond the grammar

Exit criteria:

- all syntax probes parse successfully
- all examples parse successfully
- syntax errors produce stable, readable diagnostics
- golden AST snapshots in `testdata/`

### Task 3 — AST Printer

**Objective**: Add a human-readable AST dump to `daoc`.

Deliverables:

- `compiler/frontend/ast/ast_printer.h` / `ast_printer.cpp` —
  structured AST output
- `daoc ast <file>` subcommand wired up

Output format (indicative):

```
File
  FunctionDecl a_star
    Param graph: Graph
    Param start: NodeId
    Param goal: NodeId
    ReturnType: List[NodeId]
    ResourceBlock memory Search
      LetStatement open
      WhileStatement
        ...
```

Exit criteria:

- `daoc ast` produces readable output for all examples and probes
- output is deterministic (suitable for golden-file testing)

### Task 4 — Playground Integration (Structural)

**Objective**: Bring up a minimal playground tied to the lexer and
parser as early as possible, per the ROADMAP Phase 1.5 intent of
"structural highlighting first, compiler-backed semantic highlighting
as soon as frontend analysis exists."

Deliverables:

- `tools/playground/compiler_service/` — minimal service wrapping the
  lexer and parser; transport (in-process, HTTP, or IPC) is decided at
  execution time per `docs/COMPILER_SERVICE_API.md`
- Playground frontend (stack TBD) showing:
  - structural token highlighting (keyword, operator, literal
    classification from the token stream)
  - AST panel
  - diagnostics panel
- loads examples from `examples/`

Playground stack decision is deferred to Task 4 execution. The service
layer must consume compiler frontend output, not reimplement language
logic.

Prerequisites:

- Tasks 1-3 complete
- parser handles all syntax probes and examples

Exit criteria:

- paste Dao code into the playground and see structural coloring, AST,
  and diagnostics
- playground consumes compiler frontend, not bespoke regexes
- examples load from the `examples/` directory

### Task 5 — Semantic Token Classification

**Objective**: Produce compiler-backed token classification per the
taxonomy in `CONTRACT_LANGUAGE_TOOLING.md`, and upgrade the playground
from structural to semantic highlighting.

Deliverables:

- `compiler/analysis/semantic_tokens.h` / `semantic_tokens.cpp` —
  classification API
- `daoc tokens <file>` subcommand
- Playground upgraded: `/tokens` endpoint, semantic highlighting
  replaces structural highlighting

Approach:

- lexical tokens (keywords, operators, literals, punctuation) are
  classifiable immediately from the token stream
- declaration/use distinction and type vs. function classification
  require AST — classify what is available from lexical and
  structural context; tokens that cannot yet be classified are omitted
  from the semantic token stream until resolution exists
- this layer does not reimplement parsing; it consumes the frontend

Exit criteria:

- all categories from the frozen taxonomy that are lexically or
  structurally determinable are classified
- playground shows semantic highlighting
- output is suitable for consumption by future LSP

## What Comes After

Tasks 6–13 (resolve, types, typecheck, HIR, MIR, LLVM backend,
generics, coroutines) are complete or substantially complete.
Task 15 (C ABI interop) v1 and v2 are complete — struct-by-value,
function pointer types, and named-function callbacks all landed.
Task 18 (enum payloads and match destructuring) is complete.
Task 19 (diagnostic formatter, Phase 7 entry leaf) is complete.
Task 20 (bootstrap lexer extraction) is complete — the lexer probe
has been promoted to `bootstrap/lexer/lexer.dao` with verified C++
parity, 97+ golden tests, and self-lex regression.
Task 21 (bootstrap parser extraction) is complete — the parser is
promoted to `bootstrap/parser/parser.dao` with Tier A syntax coverage,
arena-indexed AST, 36 golden tests, and self-parse of real Dao source.
Task 22 (bootstrap resolver) is complete — two-pass name resolution
with scope chains, symbol tables, uses map, 34 tests (including
cross-file resolution and program wrapper tests).
Task 23 (bootstrap type checker) is complete — expression/statement
type checking with 43 tests (including cross-module calls, concept
binding identity, variant validation, and on-disk multi-file fixtures).
Task 24 (bootstrap HIR) is complete — typed AST lowered to
compiler-owned HIR with 22 tests (including program-level lowering
and on-disk multi-file smoke test).  Shared substrate consolidated
in `bootstrap/shared/base.dao`; assembly via `bootstrap/assemble.sh`.
Task 29 (bootstrap MIR) is complete — HIR lowered to basic-block MIR
with 8 tests.
Task 30 (bootstrap LLVM backend) is complete — MIR lowered to
deterministic textual LLVM IR with 17 tests.

The Tier A bootstrap frontend-to-IR-to-text pipeline (lex → parse →
resolve → typecheck → HIR → MIR → LLVM text) is complete.
Tasks 25–27 (multi-file substrate) are complete — the `Program`
value threads through resolve → typecheck → HIR → MIR with
canonical type identity, cross-module qualified name typing, and
program-level HIR aggregation.  Task 28 (generic body lowering
boundary) is complete — see below.  Task 29 (bootstrap MIR Tier A)
is complete — see below.  Task 30 (bootstrap LLVM backend Tier A)
is complete — see below.

### Task 25 — Bootstrap Multi-file Compilation + Imports (v1)

Status: **complete**

See `docs/task_specs/TASK_25_BOOTSTRAP_MULTIFILE.md`.

- ✓ `module` keyword in bootstrap lexer and `dao.lex`
- ✓ `ModuleDeclN` and `ImportDeclN` AST nodes
- ✓ `FileN` with mandatory leading module decl
- ✓ `ProgramGraph` with deterministic topo sort and cycle detection
- ✓ diagnostics for missing/duplicate modules and import cycles

### Task 26 — Bootstrap Cross-file Resolution

Status: **complete**

See `docs/task_specs/TASK_26_BOOTSTRAP_CROSS_FILE_RESOLUTION.md`.

- ✓ `Module` symbol kind with per-module export tables
- ✓ import bindings as Module symbols in importing scope
- ✓ qualified names resolve through module export tables
- ✓ concepts in module namespace (§6.1)
- ✓ `extend` blocks scoped at module granularity (§6.5)

### Task 27 — Bootstrap Cross-file Typecheck + HIR Aggregation

Status: **complete** (#207–#215)

See `docs/task_specs/TASK_27_BOOTSTRAP_PROGRAM_TYPECHECK_AND_HIR.md`.

- ✓ `Program` value threaded through resolve → typecheck → HIR
- ✓ program-wide canonical type table (builtins seeded once)
- ✓ `ExprId` composite keying (`module_id:node_idx`)
- ✓ resolver-bound concept identity (D3: no name-based scans)
- ✓ `owner_module_id` on every symbol (D2: extend-method scoping)
- ✓ cross-module qualified name typing (D4: `mod::fn`, `mod::Type`,
  `mod::Enum::Variant` with variant validation)
- ✓ `program_run_typecheck` — two-pass architecture
  (pass1 all modules → pass2 all modules)
- ✓ `HirProgram(module_list_lp)` / `HirModule(name_tok, decls, mid)`
- ✓ `program_run_hir` — program-level HIR lowering
- ✓ on-disk multi-file test fixtures under
  `testdata/bootstrap/multifile/`
- ✓ Taskfile updated: HIR included in `bootstrap-test`

Known gaps (documented, not blocking):
- qualified concept references in extend blocks (`as mod::C:`)
  not supported — parser only accepts unqualified names
- extend-method isolation verified at HIR level by PR #242's
  method-dispatch desugaring; exact-symbol-identity assertion in
  the isolation test is still coarse-grained
- bootstrap HashMap-in-while codegen bug: worked around via
  triple-scan fallback in `lookup_use` and `hir_lookup_use`

Resolved:
- concept satisfaction for extend blocks on builtin types in
  program mode was fixed implicitly by PR #238 (concept bindings
  HashMap → triples); locked in by `concept_sat_builtin_program`
  regression test in bootstrap/typecheck

After Tasks 25–27, the remaining feature-oriented Tier B slices
(associated items, method dispatch, richer patterns) have a sane
multi-file substrate to sit on.

### Task 28 — Proper Generic Body Lowering Boundary

Status: **complete**

**Objective**: Replace the current workaround for generic MIR
lowering with the proper architectural boundary: uninstantiated
generic function bodies must not be lowered to MIR.

See `docs/task_specs/TASK_28_GENERIC_BODY_LOWERING_BOUNDARY.md`.

- ✓ `HirFunction::has_type_params` propagated from AST declaration
- ✓ `MirBuilder::build()` separates generic templates from monomorphic functions
- ✓ Combined generic detection: declaration-based (own type params) + signature-based (enclosing class generic params)
- ✓ `lowering_generic_template_` flag gates field-access tolerance during template lowering
- ✓ `monomorphize()` accepts templates map directly; no Phase 5 removal needed
- ✓ Generic enum payload sizing guard in LLVM type lowering

Deliverables:

- explicit lowerability classification at HIR → MIR boundary
- generic declaration bodies skipped during MIR worklist construction
- concrete instantiation path: substitution → lowered MIR body
- MIR concreteness invariant assertion (no generic parameter residue)
- removal of PR #237 workaround (MIR error suppression for
  non-struct receivers)
- regression tests: eager skip, concrete lowering, multiple
  instantiations, cross-module generic use, dedup

This is architectural cleanup of the host compiler, not a bootstrap
task.  It lands before Tier B bootstrap slices because those slices
(methods, concept dispatch, generic semantics) depend on a clean
generic compilation pipeline.

### Task 29 — Bootstrap MIR (Tier A)

Status: **complete** (#249)

First iteration of the bootstrap compiler's MIR layer.  HIR lowers
to a basic-block MIR mirroring the host compiler structure
(`compiler/ir/mir/mir.h`).  Closes the Tier A self-hosting arc for
the frontend-to-IR pipeline: `lex → parse → resolve → typecheck →
HIR → MIR`.

- ✓ `MirNode` arena-indexed flat node graph
- ✓ `MirModule` / `MirFunction` / `MirLocal` / `MirBlock` structural
  nodes
- ✓ instruction set: `MirConstInt`/`Float`/`Bool`/`String`,
  `MirLoad`/`Store`, `MirBinary`/`Unary`, `MirFieldAccess`,
  `MirFnRef`/`Call`, `MirReturn`/`Br`/`CondBr`, `MirErrorExpr`
- ✓ basic-block CFG: `MS.fn_blocks` accumulates per-function blocks,
  `BlockR.sealed` tracks terminator emission, `block_seal` rewrites
  each `MirBlock` with its instruction list offset and count
- ✓ `ExprR { br, value }` threading for expression lowering — Dao
  classes are value-copied across function boundaries, so explicit
  state threading is required
- ✓ if/else lowering: `cond_br → then/else → br → merge` with
  early-return detection
- ✓ while lowering: `br → header (cond_br) → body (br header) / exit`
- ✓ program pipeline routing: `lower_to_mir` threads through
  `build_program → program_run_resolve → program_run_typecheck →
  program_run_hir`, walking both `HirFile` and `HirProgram` roots
- ✓ unsupported statement kinds emit diagnostics via `ms_add_diag`
  instead of silently dropping control flow
- ✓ 8 Tier A regression tests: `minimal_program`,
  `let_binary_return`, `function_call`, `multi_function`,
  `param_locals`, `extern_function`, `if_stmt`, `while_stmt`

HIR schema improvements landed alongside:

- ✓ `HirLet.sym` stores resolver symbol index (not declaration
  token); `lower_let_stmt` resolves and stores up-front
- ✓ `HirFunction.sym` renamed from `name`, actually populated with
  `fn_sym` from `hir_find_sym_by_decl` (was storing a token index)
- ✓ `HirFunction` params list stores `(sym, type_idx)` pairs
- ✓ `BEGIN_HIR_TESTS` marker added so MIR assembly can include the
  HIR library without pulling in test helpers

Deferred to Tier B (same deferrals as the bootstrap HIR, plus):

- Generators (iter init/has_next/next/destroy/yield)
- Monomorphization / generic template separation
- Mode/resource region enter/exit
- Enum construction / discriminant / payload
- Lambda / closures
- Try operator
- For-over-iterable
- Index expressions
- Break/continue

See `bootstrap/mir/impl.dao` and `bootstrap/README.md`.

### Task 30 — Bootstrap LLVM Backend (Tier A)

Status: **complete** (#252)

First iteration of the bootstrap LLVM backend.  Lowers Task 29
bootstrap MIR to deterministic textual LLVM IR via a backend-private
Dao-side mini-IR and text serializer.  Closes the Tier A
frontend-to-IR-to-text pipeline: `lex → parse → resolve → typecheck
→ HIR → MIR → LLVM text`.

- ✓ Backend-private mini-IR: `LlModule` / `LlFunction` / `LlBlock` /
  `LlInst` / `LlInstKind` / `LlType` / `LlGlobal` / `LlParam`
- ✓ `MirBackendInput` bundles `MirResult` + symbols + types + tokens
  + source, preserving Task 29's `MirResult` contract
- ✓ Type lowering centralized in `ll_type_from_mir` for Tier A
  primitives (i32, i64, f32, f64, bool→i1, void, string→ptr)
- ✓ Alloca-everything SSA with mandatory param seeding (§5.4/§10.3)
- ✓ Type-dispatched arithmetic (add/sub/mul/sdiv/srem, fadd/fsub/
  fmul/fdiv), comparisons (icmp/fcmp), calls, terminators
- ✓ String literals → private module-global `[N x i8]` + GEP with
  correct escape decoding/re-encoding
- ✓ Fail-closed: `MirFieldAccess`, `MirErrorExpr`, anonymous syms,
  unsupported types, unterminated blocks all produce diagnostics
- ✓ Deterministic output (byte-identical across runs)
- ✓ MirModule root contract honored (`MirResult.root` → fn_list walk)
- ✓ `write_llvm_text` wired to stdlib `write_file`
- ✓ 12 Tier A regression tests

Narrow upstream fix landed alongside:

- ✓ `tc_register_fn_sig_core` writes `sym_types` for each param
  during function-signature registration (fixes extern fn param
  types being -1)

See `docs/task_specs/TASK_30_BOOTSTRAP_LLVM_BACKEND.md` and
`bootstrap/llvm/impl.dao`.

### Task 31 — Host Multi-file Compilation

Status: **complete** — D0 (program-wide source map; prelude loaded
as separate files; `prelude_bytes` machinery and `blank_leading_module`
removed; `compiler/frontend/module/` created), D1 (`ModuleInfo`,
module graph with import edges, lexical Kahn ordering and cycle traces,
entry selection per §7.7, root-file discovery with the §8.3 mapping
rule, lexical file-id order per §8.4, `--module-root` / `--stdlib-root`
/ `--source` / `--entry`), D2 (builtins → prelude → per-module scopes,
`Symbol::module`, imports bound to `ModuleInfo`, export-table
resolution of qualified names, builtins unshadowable, prelude shadowing)
and D3 (qualified forms type-check through the resolver's per-segment
entries: `b::f`, `b::T` in type position, `b::T::m`, `b::E::V`; modules
checked in topological order) and D4 (`HirProgram` of per-module
`HirModule`s, MIR flattened in program order, `llvm_function_name` by
symbol identity with `<module>::<name>` mangling and the entry-module
`main` rule, intrinsic and hook recognition gated on identity, the
first multi-file executable), D5 (analysis and the playground on the
program's in-memory mode; `daoc tokens` / `resolve` dumps select user
modules by `is_prelude`; cross-file hover and definition covered) and
D6 (`docs/building.md` driver usage; this entry) landed.  Explicit
deferrals stay as the spec lists them: Task 32 (import forms,
`assemble.sh` retirement) and Task 33 (bootstrap module-system parity).

**Objective**: make the C++ host compiler compile a program spanning
multiple Dao source files with real module identity, import-driven
discovery, and cross-module resolution, type checking, and lowering —
using the `module` / `import` syntax and the module semantics already
frozen for the bootstrap (Tasks 25–27).

See `docs/task_specs/TASK_31_HOST_MULTIFILE_COMPILATION.md`.

Deliverables:

- `CONTRACT_MODULE_SYSTEM.md` (landed with the spec): module identity,
  import binding and exposure, exports, `extend` scoping, qualified
  forms, the prelude, entry-module selection, determinism
- program-wide source map: per-file base offsets with a reserved
  position after each file (so end-of-file diagnostics locate to the
  right file) replace every `prelude_bytes` / `prelude_lines` site
  (136 across 13 files) and the prelude concatenation in the driver
  and playground
- `Program` / `ModuleInfo` / module graph with discovery over ordered
  module roots, explicit file-list mode, in-memory inputs, and
  deterministic topological order
- per-module scopes and export tables; qualified `b::f`, `b::T`,
  `b::E::V`, `b::T::m` across modules; module-scoped `extend` sets
- prelude group: `stdlib/core` + `stdlib/io` stay visible unqualified
  (including their `extend` methods) as part of every module's scope
- `HirProgram` root; MIR flattened in topological order;
  module-qualified LLVM symbol names via one `mangled_name` helper;
  entry-module `main` rule
- analysis APIs and the playground on the shared `load_program` API
- new frontend subroot `compiler/frontend/module/` (required by the
  layout and phase contracts as of the spec; created in D0 together
  with its `ARCH_INDEX.md` entry)

This is host compiler work, not a bootstrap task.  It lands before
further bootstrap Tier B slices because every slice adds Dao source
the host must currently concatenate, and because the stdlib-as-modules,
LSP workspace, and playground T3 tracks all need this substrate.
`assemble.sh` retirement is explicitly Task 32: it needs an import-form
decision (selective/glob) in `CONTRACT_SYNTAX_SURFACE.md` first.
Bootstrap conformance to `CONTRACT_MODULE_SYSTEM.md` is Task 33.

### Task 33 — Bootstrap Module-System Parity

Status: **not started** — sequenced after Task 31 and the bootstrap
methods slice.

**Objective**: close the bootstrap compiler's gaps against
`CONTRACT_MODULE_SYSTEM.md` §12: `b::T::m` qualified static methods
(§6, once bootstrap methods exist), the prelude (§7), entry-module
selection (§8), and program-level output determinism (§9).  The
conformance table in `CONTRACT_MODULE_SYSTEM.md` §12 and its mirror in
`bootstrap/README.md` are the status of record and move only with the
bootstrap work that closes each row.

### Task 14 — Numeric Type Expansion

**Objective**: Implement the numeric semantics frozen in
`CONTRACT_NUMERIC_SEMANTICS.md` in staged tiers.

Governing contract: `docs/contracts/CONTRACT_NUMERIC_SEMANTICS.md`

#### Tier A — Near-term (Phase 5 tail / pre-Phase 6)

Status: **complete**

- ✓ f64 codegen audited against IEEE 754: all six comparison
  predicates use correct ordered/unordered semantics (oeq, une,
  olt, ole, ogt, oge); NaN propagation preserved (no fast-math
  flags); signed-zero preserved (fneg, fsub, fadd emit bare LLVM
  IR with no nsz/nnan/ninf flags)
- ✓ f32 shares the same IEEE 754-conformant codegen path
- ✓ integer overflow policy frozen: checked by default (trap via
  sadd/ssub/smul.with.overflow intrinsics for all signed types)
- ✓ no fast-math flags anywhere in the backend (CONTRACT §8.1)
- ✓ float-to-int conversions trap on NaN/Inf/out-of-range

#### Tier A+ — Post-baseline (no blocking dependency)

Status: **complete**

- ✓ explicit wrapping operations for i32 and i64: `wrapping_add`,
  `wrapping_sub`, `wrapping_mul` (+ `_i64` variants)
- ✓ explicit saturating operations for i32 and i64: `saturating_add`,
  `saturating_sub`, `saturating_mul` (+ `_i64` variants)
- ✓ explicit checked operations for all signed types (i8–i64):
  `checked_add`, `checked_sub`, `checked_mul` — return
  `Option.None` on overflow, `Option.Some(result)` otherwise;
  pure Dao implementations (no runtime hooks), enabled by
  `Option<T>` prelude promotion

#### Tier B — Phase 6 prerequisite

Status: **complete**

- ✓ `i64` surface exposure: type system (`BuiltinKind::I64`,
  `type_context.i64()`), parser recognition, LLVM backend lowering
  (`llvm::Type::getInt64Ty`), runtime hooks (`__dao_eq_i64`,
  `__dao_conv_i64_to_string`), stdlib extensions (`extend i64 as
  Numeric`), working example (`examples/i64.dao`)
- ✓ explicit numeric conversions between i32/i64 and f32/f64 with
  trapping semantics (27-function conversion matrix)
- ✓ `__dao_str_length` returns `int64_t`

#### Tier C — Phase 6+ dedicated task

Status: **complete**

- ✓ full integer width expansion: i8, i16, u8, u16, u32, u64 —
  type system, LLVM codegen, equality, to_string, C ABI, Equatable,
  Printable, Numeric concept extensions
- ✓ `f32` surface exposure: type, codegen, stdlib formatting,
  conversion, runtime hooks, equality, printing
- ✓ float-to-int trapping for all combinations (f32/f64 → i32/i64)
- ✓ full numeric conversion matrix: 27 explicit conversion functions
  covering widening, narrowing, sign-changing, and float↔int/float
- ✓ wrapping and saturating overflow for all signed types (i8–i64)

#### Tier D — Phase 8

Priority: **low** — depends on GPU and optimization infrastructure.

- fast-math / relaxed numeric opt-in mode
- GPU numeric profiles
- decimal type design and initial implementation

Exit criteria (Tier A+B): **met**

- ✓ f64 comparisons emit correct IEEE predicates in LLVM IR
- ✓ i32 overflow behavior is defined and tested
- ✓ i64 is usable end-to-end (type, codegen, runtime, examples)
- ✓ explicit numeric conversions exist between i32 and f64

### Task 16 — Error-Tolerant Parsing and Tooling Hardening

**Status**: complete

**Objective**: Make the parser produce partial ASTs for incomplete
or broken source, enabling completion and diagnostics while typing.

Governing contracts: `CONTRACT_LANGUAGE_TOOLING.md`

Deliverables:

- error recovery at ~18 parser error points: skip to next statement
  or insert synthetic nodes instead of bailing
- partial AST consumers: resolver, type checker, and analysis APIs
  must tolerate missing/error nodes gracefully
- dot completion for general expressions: use AST expression types
  (`typed.expr_type()`) instead of symbol-only heuristics, so
  `make_point().`, `p.x.`, and `arr[i].` work
- playground diagnostics suppression for incomplete constructs
  (e.g. don't report "function not found" while user is typing
  an opening paren)

Implementation summary:

- parser produces ErrorExpr/ErrorStmt/ErrorDecl placeholders with
  synchronization at statement and declaration boundaries
- statement parsers (let, if, while, for, yield, return, assignment)
  detect ErrorExpr children and promote to ErrorStmt
- resolver, type checker, and HIR builder tolerate error nodes via
  explicit cases and silent default fallthrough
- dot completion uses expr_types map scan for expression receivers
  when symbol lookup fails (handles calls, field chains, indexing)
- analyze pipeline continues through resolve/typecheck on parse
  errors to produce partial semantic tokens and AST; downstream
  cascade diagnostics are suppressed when parse errors exist;
  HIR/MIR/LLVM lowering is skipped when parse errors exist

Exit criteria:

- ✓ typing `p.` mid-statement shows dot completions without parser
  failure
- ✓ incomplete source produces partial results instead of no results
- ✓ diagnostics for in-progress constructs are deferred or suppressed

## Principles

- The grammar in `spec/grammar/` is the parser's source of truth
- Do not invent infrastructure choices beyond what is frozen here
- Minimize external dependencies
- Frontend stability before backend ambition
- The playground is a development tool, not a demo
