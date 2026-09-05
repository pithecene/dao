# Architecture Index — Dao

Navigation-only lookup table for agents.
Normative behavior lives in `CLAUDE.md` and `docs/contracts/`.

## Root files

| File | Purpose |
|------|---------|
| `CLAUDE.md` | Repo constitution and structural invariants |
| `AGENTS.md` | Contributor and agent guardrails |
| `README.md` | Project overview |
| `.bonsai.yaml` | Repo-local Bonsai routing hints |
| `.grove.yaml` | Grove project metadata and consolidation hints |
| `mise.toml` | mise tool/runtime pins, env-based build parallelism cap |
| `Taskfile.yml` | Task runner commands (build, test, playground, etc.) |
| `CMakeLists.txt` / `CMakePresets.json` | CMake build entry and debug/release presets |
| `conanfile.py` | Conan 2 dependency manifest (llvm-core, boost-ext/ut, cpp-httplib, nlohmann_json) |
| `.clang-format` / `.clang-tidy` | C++ formatting and static-analysis configuration |

## `profiles/`

Conan profiles and their lockfiles (`clang-21-debug`,
`clang-21-debug.lock`). See `docs/building.md`.

## `docs/`

Contracts and explanatory material.

- `ARCH_INDEX.md` — this file
- `contracts/` — normative contracts for structure, syntax, the module system, execution contexts, compiler architecture, bootstrap/interop posture, tooling boundaries, runtime ABI, numeric semantics, and C ABI interop
- `ROADMAP.md` — staged implementation plan from frontend skeleton to self-hosting, tooling maturity, and GPU expansion
- `IMPLEMENTATION_PLAN.md` — concrete task sequence, toolchain decisions, and delivery order (Tasks 0–5 in full; Tasks 6+ summarized with status)
- `task_specs/` — detailed per-task design specs for Tasks 6+
- `compiler_bootstrap_and_architecture.md` — explanatory notes on preferred compiler internals and staged bootstrap posture
- `PLAYGROUND_ARCHITECTURE.md` — explanatory architecture for the playground as a first-class development surface and future web IDE
- `IDE_AND_TOOLING.md` — explanatory posture for semantic tooling, LSP, and why some tooling decisions are contract-level while others stay freestanding
- `COMPILER_SERVICE_API.md` — explanatory shared analysis payloads for CLI, playground, and LSP
- `building.md` — build prerequisites, parallelism cap (`DAO_BUILD_JOBS`), and override instructions
- `language_vision.md` — explanatory design doctrine, stdlib posture, module/namespace design, and GPU strategy

## `spec/`

Reference language material.

- `grammar/` — parser-facing reference grammar, lexical surface, and indentation rules
- `semantics/` — semantic notes that are below contract level but above examples
- `syntax_probes/` — small snippets used to test readability and parser direction
- `examples/` — reference examples housed under `spec/examples/`

## `compiler/`

Compiler implementation roots only.

### `compiler/support/`

Shared low-level utilities used across compiler subsystems.

- `arena.h` — bump allocator with destructor tracking for compiler-owned object graphs

### `compiler/frontend/`

Source-facing compiler pipeline.

- `lexer/` — indentation-aware tokenization and lexical rules
- `module/` — program assembly: `SourceFile`/`SourceMap` (program-wide offset space with per-file base offsets and reserved EOF positions), `Program` construction from inputs, prelude-group loading (`CONTRACT_MODULE_SYSTEM.md`; Task 31)
- `parser/` — parsing and surface grammar ingestion
- `ast/` — syntax tree / declaration and expression representation
- `diagnostics/` — spans, reporting, and source diagnostics plumbing
- `resolve/` — name resolution: scope chain, symbol binding, and identifier resolution
- `types/` — canonical semantic type universe: type kinds, interning, context, printing
- `typecheck/` — first type-checking pass: side-table typing, assignability, expression/statement validation, diagnostics
- surface-to-HIR lowering lives in `compiler/ir/hir/` (the HIR builder); there is no separate `lower/` root

### `compiler/ir/`

Compiler-internal target-agnostic representations.

- `hir/` — typed, symbol-linked HIR: arena-owned node hierarchy, AST-to-HIR builder, and debug printer
- `mir/` — basic-block MIR with typed instructions, place/value distinction, HIR-to-MIR builder, monomorphization pass, and debug printer

### `compiler/backend/`

Target-specific lowering.

- `llvm/` — initial backend target: MIR→LLVM IR lowering, type lowering, runtime hook declarations, function/block/instruction emission, x86-64 C ABI struct coercion, textual IR output, native object emission

### `compiler/driver/`

Compiler orchestration, sessions, target selection, and top-level build
coordination.

### `compiler/analysis/`

Shared analysis APIs that expose semantic tokens, diagnostics, hover,
completion, definitions, references, and document symbol payloads to the
CLI, playground, and LSP.

- `semantic_tokens.h` / `semantic_tokens.cpp` — token classification per
  the frozen taxonomy in `CONTRACT_LANGUAGE_TOOLING.md`
- `hover.h` / `hover.cpp` — hover info: symbol kind, name, type at offset
- `goto_definition.h` / `goto_definition.cpp` — jump-to-declaration from use site
- `completion.h` / `completion.cpp` — scope-aware symbol completion at cursor offset
- `document_symbols.h` / `document_symbols.cpp` — hierarchical symbol tree from AST
- `references.h` / `references.cpp` — find all use-sites of a symbol

## `runtime/`

Execution support for lowered programs.

- `core/` — minimal native runtime linked into every executable: ABI declarations (`dao_abi.h`) and hooks for IO (`io.c`), equality (`equality.c`), scalar-to-string conversion (`convert.c`), strings (`string.c`), checked overflow (`overflow.c`), panics (`panic.c`), resource domains (`resource.c`), and generators (`generator.c`)
- `memory/` — scoped resource and allocation-domain support
- `modes/` — runtime integration for `mode` semantics
- `gpu/` — GPU/runtime bindings and execution support

## `stdlib/`

Dao standard library surface and future implementation roots.

- `core/` — foundational types/utilities (auto-imported as prelude)
- `concepts/` — deferred concept definitions not yet auto-imported
- `numerics/` — compute- and math-oriented surface
- `io/` — explicit IO-facing surface

## `bootstrap/`

Self-hosting compiler subsystems written in Dao.

- `README.md` — scope, parity notes, and subsystem inventory
- `shared/` — single source of truth for the bootstrap frontend pipeline:
  token model, lexer, AST, parser, module graph (`base.dao`); assembled
  into `*.gen.dao` files via `assemble.sh`
- `assemble.sh` — concatenates `shared/base.dao` with subsystem sources
  to produce compilable `*.gen.dao` outputs (gitignored build artifacts)
- `lexer/` — indentation-aware lexer matching the host compiler's token
  surface; tests in `tests.dao` (Task 20)
- `parser/` — recursive-descent parser producing arena-indexed AST for
  Tier A Dao syntax; tests in `tests.dao` (Task 21)
- `graph/` — module graph construction, cycle detection, and topological
  sort for multi-file compilation; tests in `tests.dao` (Task 25)
- `resolver/` — two-pass name resolver with scope chains, symbol tables,
  and uses map; logic + tests in `impl.dao` (Task 22)
- `typecheck/` — type checker assigning types to expressions and
  validating type correctness; logic + tests in `impl.dao` (Task 23)
- `hir/` — HIR lowering pass producing compiler-owned typed IR from
  the type-checked AST; logic + tests in `impl.dao` (Task 24)
- `mir/` — Tier A MIR lowering pass producing basic-block MIR from
  HIR: functions, locals, blocks, constants, binary/unary, let/assign,
  calls, returns, field-access reads, and if/else + while control
  flow; logic + tests in `impl.dao` (Task 29)
- `llvm/` — LLVM backend: lowers bootstrap MIR to deterministic
  textual LLVM IR via a backend-private Dao-side LLVM mini-IR and text
  serializer.  Tier A covers functions, locals, constants,
  binary/unary, calls, returns, if/else, while, and string literals.
  First Tier B slice adds struct type definitions, struct construction
  via alloca + per-field GEP stores, struct field reads via
  `extractvalue`, and struct-typed call/return edges.  Logic + tests
  in `impl.dao` (Task 30 + struct field access slice).

## `examples/`

Small Dao programs and human-readable examples. Non-authoritative.
Also serves as a playground corpus and early regression corpus.

## `testdata/`

Fixtures and golden inputs/outputs for parser/compiler tests.

- `ast/` — golden AST printer output for examples, stdlib, and syntax probes
- `module/` — on-disk fixtures for host root-file discovery
  (`smoke/`: transitive imports and a prelude import; `mismatch/`: a
  located file declaring a different module; `roots/`: a second module
  root)
- `bootstrap/multifile/` — on-disk multi-file test fixtures for the
  bootstrap program pipeline (Task 27 D9/D10)
  - `smoke/` — three-module import graph (core::fmt, app::math, app::main)
  - `cross_module_enum/` — imported enum variant access
  - `extend_isolation/` — cross-module import with class export

## `tools/`

Developer-surface tooling built on compiler analysis.

- `playground/` — first-class web playground and future web-IDE surface
  - `compiler_service/` — HTTP server wrapping the compiler frontend (cpp-httplib + nlohmann/json); shared pipeline utilities in `pipeline.h/.cpp`
  - `frontend/` — Vite + TypeScript with CodeMirror 6; dev mode uses HMR with API proxy, prod builds to `dist/` served by the compiler service
- `lsp/` — reserved LSP root (not yet implemented; the analysis APIs it will wrap live in `compiler/analysis/`)
- `formatter/` — reserved canonical formatter root
- `diagnostics/` — presentation and diagnostics tooling experiments

## `ai/`

Bonsai governance assets.

- `skills/` — repo-local skills
- `baselines/` — accepted baseline findings / JSON snapshots
- `out/` — generated Bonsai outputs (git-ignored)
