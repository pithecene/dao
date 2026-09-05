# Task 31 — Host Multi-file Compilation

Status: implementation spec
Phase: host compiler substrate (Phases 2–4) / prerequisite for further
Phase 7 self-hosting work
Scope: make the C++ host compiler compile a program spanning multiple
Dao source files, with real module identity, import-driven discovery,
and cross-module resolution, type checking, and lowering — using the
module syntax and semantics that are already frozen

## 1. Objective

Replace the host compiler's single-buffer model (one user file
concatenated behind the stdlib prelude) with a program model: a set of
source files, each declaring one module, joined by an import graph,
resolved and type-checked as one program, lowered to one LLVM module.

The surface syntax does not change.  `module a::b` and `import a::b`
already exist in the grammar, the lexer, the parser, the AST, and
`CONTRACT_SYNTAX_SURFACE.md`.  Task 31 makes the host compiler honour
them.

## 2. Problem statement

Today `daoc build main.dao` does the following
(`compiler/driver/pipeline.cpp`):

1. reads every `.dao` under `stdlib/core/` and `stdlib/io/` in sorted
   order, blanks each file's leading `module` line
   (`support/module_utils.h`), and concatenates them
2. blanks the user file's `module` line and appends the user source
3. prepends a synthetic `module main` header
4. lexes, parses, resolves, type-checks, and lowers that one buffer

Everything downstream is shaped by this:

- `prelude_bytes` / `prelude_lines` thread through 136 call sites in
  13 files (driver, resolver, backend, analysis, playground) to tell
  "stdlib" apart from "user" by byte offset and to rebase line numbers
- `import a::b` binds `b` as a `SymbolKind::Module` whose `decl` is
  the `ImportNode` itself; trailing segments of `b::x` are never
  resolved (`resolve.cpp` §QualifiedName: "unresolvable in Task 6")
- the type checker rejects qualified type names outright
  (`type_checker.cpp`: "qualified type names are not yet supported")
- the LLVM backend keys functions by bare `symbol->name` (17 sites), so
  two modules cannot both define `add`
- the bootstrap compiler, written in Dao, must be concatenated into
  `*.gen.dao` blobs by `bootstrap/assemble.sh` because the host cannot
  compile more than one file
- the playground service duplicates the prelude concatenation
  (`tools/playground/compiler_service/run.cpp`, `analyze.cpp`)

Meanwhile the bootstrap compiler already has a module graph, per-module
export tables, resolver-bound module identity, and cross-module typing
(Tasks 25–27).  The Dao-in-Dao compiler understands modules; the C++
compiler that builds it does not.  That inversion is the largest gap
between what the language specifies and what the toolchain does.

## 3. Primary design objective

One program, many files, one set of rules:

- module identity comes from the `module` declaration, never from the
  file path (contract)
- `import a::b` binds `b`; access is `b::name` (contract)
- the module semantics frozen for the bootstrap in Task 26 §6 apply
  unchanged to the host, so the two compilers agree by construction
- no separate compilation: the whole program is resolved and checked
  in two passes, exactly as the bootstrap does today
- the stdlib keeps working unqualified through an explicit **prelude
  group**, so no example, test, or bootstrap source changes meaning

## 4. Why this task exists now

- `assemble.sh` cannot be retired without it (§6.4 explains why it is
  still not retired *by* it)
- the stdlib cannot become real modules without it
- the LSP workspace model and playground T3 (multi-file editing) have
  no substrate without it
- every further bootstrap Tier B slice adds Dao source that the host
  must concatenate; the workaround grows with the project

## 5. In scope

### 5.1 Source set and discovery

A `Program` is built from a root file plus import-driven discovery
across an ordered list of module roots, or from an explicit file list.
Both modes produce the same `Program` shape (§10).

### 5.2 Module graph

Files map 1:1 to modules; imports form directed edges; the graph is
validated (missing module, duplicate module, cycle) and topologically
ordered deterministically.

### 5.3 Program-wide source map

Spans stay `{offset, length}`; every file is assigned a disjoint base
offset in one program-wide offset space; a `SourceMap` maps any offset
back to (file, line, column).  This replaces `prelude_bytes` and
`prelude_lines` entirely (§9).

### 5.4 Per-module resolution and exports

Each module gets its own scope; all top-level declarations are exported;
`b::name` resolves through the export table of the module bound to `b`.

### 5.5 Prelude group

The stdlib modules that are implicitly visible today remain implicitly
visible, unqualified, including their `extend` methods (§7.6).

### 5.6 Cross-module type checking

`b::fn(...)`, `b::Type`, `b::Enum::Variant`, and `b::Type::method`
type-check across modules; the canonical `TypeContext` is already
program-wide.

### 5.7 Program-level IR

HIR gains a `HirProgram` root of per-module `HirModule`s; MIR and LLVM
stay single-module (flattened in topological order), mirroring the
bootstrap's Task 29/30 decision.

### 5.8 Symbol naming

Dao-defined functions get module-qualified LLVM names so identical
names in different modules do not collide (§12).

### 5.9 Driver surface

`daoc build|check|hir|mir|llvm-ir|tokens|resolve <root.dao>` become
program-aware; module roots and explicit file sets are flags (§13).

### 5.10 Tooling on the same substrate

`compiler/analysis/` and the playground consume the `Program` API;
the playground's private prelude concatenation is deleted.

## 6. Non-goals

### 6.1 Language surface

- visibility modifiers (`pub`, `internal`, …) — `language_vision.md`
  lists the tiers as not frozen
- selective imports (`import a::b::{c, d}`), glob imports
  (`import a::b::*`), aliased imports (`import a::b as c`), re-exports
- package / workspace concepts; a package manager; search-path policy
  beyond §8.2

### 6.2 Compilation model

- separate compilation, object caching, incremental rebuilds
- ABI-stable mangling of Dao symbols exposed to C
  (`CONTRACT_C_ABI_INTEROP.md` §8 reserves an "export" contract)
- cross-module `extend` visibility and coherence / orphan rules
  (Task 26 §6.5 defers them; Task 31 keeps that deferral)

### 6.3 Bootstrap

No change to bootstrap code.  `bootstrap/README.md` gains the
module-system conformance table (mirrored from
`CONTRACT_MODULE_SYSTEM.md` §12) so the gaps are recorded; closing
them is Task 33 (§21).  Parity is verified by running the host on the
bootstrap's existing multi-file fixtures (§18.5), not by editing
bootstrap code.

### 6.4 `assemble.sh` retirement

Task 31 makes retirement *possible*, not *done*.  With the frozen
import rule (an import exposes only the module name, §7.2), converting
the ~14 k lines of bootstrap Dao into modules would require qualifying
every cross-file reference (`base::lex`, `base::Token`, …) or adding a
glob/selective import form.  The latter is a syntax decision that must
go through `CONTRACT_SYNTAX_SURFACE.md` first.  That decision and the
retirement are Task 32 (§21).

## 7. Semantic model

Every rule in this section is normative in `CONTRACT_MODULE_SYSTEM.md`,
which lands in the same change as this spec.  §7.1–§7.5 were already
frozen by `CONTRACT_SYNTAX_SURFACE.md`, Task 25 §10, and Task 26 §6;
§7.6–§7.8 are the rules the new contract adds.  This section explains
them; the contract governs.

### 7.1 Module identity

Every file begins with exactly one `module` declaration; that
declaration is the file's identity.  Paths never define identity.  Two
files declaring the same module are an error (no partial modules).

### 7.2 Import binding

`import a::b` binds the local name `b` to module `a::b`.  It exposes the
module name only — none of `a::b`'s declarations enter the importing
scope unqualified.  A binding name that collides with a top-level
declaration or another import in the same module is an error.

### 7.3 Exports

Every top-level declaration (function, class, enum, alias, concept) is
exported from its module.  `extend` blocks are not named and are not
exported (§7.5).

### 7.4 Same-module access

Inside a module, its own top-level declarations are visible unqualified,
with forward references, exactly as in a single file today.

### 7.5 `extend` scoping

Methods introduced by `extend` participate in method-set lookup within
the declaring module only.  Importing a module does not import its
`extend` methods.  The prelude group is the sole exception (§7.6).

### 7.6 Prelude group

**Definition.**  The prelude group is the set of modules loaded from
`stdlib/core/*.dao` and `stdlib/io/*.dao` — precisely the files the
driver concatenates today.  `stdlib/concepts/` stays outside it, as
now.

**Visibility.**  Prelude modules are treated as part of every module's
lexical environment, not as implicit imports:

- their exported declarations are visible unqualified in every module,
  including in each other (prelude modules are checked as one
  namespace, which is what the concatenated buffer gives them today)
- their `extend` methods participate in method-set lookup everywhere
  (so `x.to_string()`, `print(x)` via `Printable`, and `Vector<T>`
  methods keep working)

**Scope shape.**

```
builtins (i32, string, null_ptr, …)
  └─ prelude scope   (union of prelude module export tables)
       └─ module scope (one per non-prelude module: own decls + import bindings)
            └─ function / block / lambda scopes (unchanged)
```

**Shadowing.**  A non-prelude module's top-level declaration may
shadow a prelude name silently; lookup proceeds innermost-first, as it
already does for locals.  Today the same collision is reported as a
duplicate top-level declaration because prelude and user share one
scope; this is the one deliberate behaviour change in the task and is
listed in §19.

**Builtins are not shadowable.**  Shadowing stops at the prelude.
Compiler builtins and predeclared names (`i32`, `string`, `void`,
`Generator`, `null_ptr`, `ptr_cast`, the `size_of`/`align_of` family)
cannot be redeclared by any module, prelude included
(`CONTRACT_MODULE_SYSTEM.md` §7.6).  Today this falls out of builtins
sharing the file scope; with builtins in their own outer scope,
`Scope::declare`'s local-only check would silently let `fn size_of`
through, and §12's intrinsic recognition would then lower calls to it
as the intrinsic instead of its body.  Pass 1 therefore checks every
top-level declaration against the builtins scope explicitly (§11.2)
and keeps today's `duplicate top-level declaration` diagnostic.  The
`__dao_` prefix likewise stays reserved to prelude modules
(`CONTRACT_MODULE_SYSTEM.md` §7.7), keyed on the declaring module's
`is_prelude` rather than on a byte offset.

**Qualified access.**  Prelude modules are ordinary modules with
identities (`core::vector`, `io::file`); `import core::vector` followed
by `vector::Vector` is legal and resolves to the same symbols.

### 7.7 Entry module and `main`

Exactly one module of a program is the entry module.  In root-file
mode it is the module declared by the root file.  In explicit file-list
and in-memory modes it is selected by `--entry <module>`; when `--entry`
is absent and exactly one module declares `fn main`, that module is the
entry; zero or several such modules without `--entry` is an error.
Entry selection is therefore a function of the file set and the
options, never of input order (§8.4).  The entry module's `fn main` is
the program entry and is emitted with the LLVM name `main`.  A `main`
in any other module is an ordinary function (mangled per §12).

### 7.8 Qualified forms

| Form | Resolves via | Status |
|---|---|---|
| `b::f(...)` | export table of module bound to `b` | in scope |
| `b::T` in type position | export table; `T` must be a type | in scope |
| `b::E::V` | export table → enum `E` → variant `V` | in scope |
| `b::T::m(...)` | export table → type `T` → static method `m` | in scope (host already has `T::m`) |
| `T::m`, `E::V` (unqualified type) | unchanged | unchanged |
| deeper paths (`b::c::x`) | — | error: imports bind one segment |

## 8. Source set and discovery

### 8.1 Inputs

```
SourceInput { std::string display_path; std::string text; }   // in-memory
```

Three ways to build a program:

1. **root-file mode** (default CLI): one root path; imports drive
   discovery (§8.3)
2. **explicit file list mode**: every file enumerated; discovery is
   off; unresolved imports are diagnosed, never searched.  The entry
   module is chosen per §7.7 (`--entry`, or the unique `main`).  This
   is the primitive the tests and the bootstrap fixtures use.
3. **in-memory mode**: `SourceInput`s instead of paths (playground,
   LSP, unit tests).  Identical to (2) otherwise.

In every mode the prelude group (§7.6) is loaded from the stdlib root
and added to the file set before discovery runs.

### 8.2 Module roots

An ordered list of directories searched in root-file mode:

1. the directory containing the root file
2. each `--module-root DIR`, in command-line order
3. the stdlib root (`DAO_SOURCE_DIR/stdlib` by default;
   `--stdlib-root DIR` overrides)

### 8.3 Mapping rule

`import a::b::c` is satisfied by the first existing
`<root>/a/b/c.dao` over the roots in order.  The file's declared module
must equal `a::b::c`; a mismatch is an error naming both.  This makes
the existing stdlib layout (`stdlib/core/vector.dao` ↔ `core::vector`)
the convention without inferring identity from it: the declaration is
still the authority, the path only has to agree.

Discovery is transitive and terminates because the file set is finite
and each module is loaded once.

### 8.4 Determinism

- `file_id` is assigned in lexical order of the normalized absolute
  path (in-memory inputs: lexical order of `display_path`)
- `module_id` is assigned in registration order, which is a pure
  function of `file_id` order
- topological order is Kahn's algorithm with the ready set kept in
  lexical order of module display name
- diagnostics are emitted in `file_id` order, then offset order

- the entry module is selected per §7.7, which does not consult
  input order

The same file set with the same entry selection, in any input order,
yields byte-identical output.

### 8.5 Graph diagnostics

- module declared by two files
- imported module not found (root-file mode: after searching all
  roots; explicit mode: not in the set)
- path/declaration mismatch (§8.3)
- import cycle, with the cycle trace and without acyclic dependents in
  the trace (mirrors bootstrap graph test 11)
- self-import
- root file has no `module` declaration (already a parse error; the
  program layer must survive it in analysis mode, §16.3)

## 9. Program-wide source map

### 9.1 The problem

`Span` is `{uint32_t offset, uint32_t length}` with no file identity.
Tokens, AST nodes, symbols, `uses` keys, diagnostics, and scope ranges
all carry bare offsets.  Adding a file id to `Span` would touch every
one of them.

### 9.2 Decision

Keep `Span` unchanged.  Give each file a **base offset** in one
program-wide offset space, with one reserved position after every file
so that a file's end-of-file offset is its own:

```
file 0 (prelude core::builtins)   [0,       size0]        EOF at size0
                                   size0+1  reserved — never a position of any file
file 1 (prelude core::comparable) [base1,   base1+size1]  base1 = size0 + 1
…
file n (user app::main)           [base_n,  base_n+size_n]
```

Each range is closed: `base + size` is the file's EOF position, the
offset at which the lexer already emits its zero-length `Eof` token
(`lexer.cpp`: `emit(TokenKind::Eof, pos_, 0)`) and at which
unexpected-end diagnostics are reported.  Because
`base_{n+1} = base_n + size_n + 1`, the EOF position of file *n* is
never the first byte of file *n+1*, and an empty file (`size = 0`) has
the single representable position `base`.  Base offsets are assigned
only after the whole file set is loaded and the position budget in
§9.6 has been checked.

`lex(const SourceBuffer&, uint32_t base_offset = 0)` adds the base to
every span it emits.  Nothing downstream changes: offsets are already
opaque, and they are already disjoint between "prelude" and "user"
today — this generalizes the one boundary to n boundaries.

`std::string_view`s (token text, symbol names) keep pointing into each
file's own buffer; the `Program` owns every buffer for its lifetime,
which is the same ownership rule `ResolveResult` documents today.

### 9.3 `SourceMap`

```cpp
struct SourceLocation { const SourceFile* file; uint32_t line; uint32_t col; };

class SourceMap {
  auto file_for(uint32_t offset) const -> const SourceFile*;   // binary search on base offsets
  auto locate(uint32_t offset) const -> SourceLocation;
  auto text(Span) const -> std::string_view;
};
```

`file_for(o)` returns file *n* iff `base_n <= o <= base_n + size_n`
(closed range, §9.2); an offset in a reserved gap is a programming
error and asserts.  `locate(base_n + size_n)` is the EOF location of
file *n*: its last line, column one past the last character (`1:1` for
an empty file) — the same answer `SourceBuffer::line_col(size)` gives
today for a single file.

### 9.4 What it replaces

Every `prelude_bytes` / `prelude_lines` parameter and every
`offset < prelude_bytes` test becomes a `SourceMap` query
(`file_for(offset)->module->is_prelude`) or disappears.  `blank_leading_module`,
`strip_leading_module`, and `starts_with_module` in
`support/module_utils.h` are deleted once no caller remains.

### 9.5 Diagnostics

`print_diagnostics` prints `<path>:<line>:<col>` from
`SourceMap::locate`; the line-rebasing arithmetic is deleted.  The
playground's JSON diagnostics carry the same triple.

### 9.6 Position budget

Every file consumes `size + 1` positions (§9.2), so the bound is on
positions, not bytes.  Before any base offset is assigned, the loader
computes `Σ (size_i + 1)` over all files in 64-bit arithmetic and
requires it to be `≤ 2^32`; the highest position then used,
`base_last + size_last = Σ − 1`, fits in `uint32_t`.  Exceeding the
budget is a load-time error naming the file count and byte total
(`program exceeds the 4 GiB offset space: N files, M bytes`), never a
wrapped offset.  The check runs before lexing, so no span is ever
created outside the space.  N empty files consume N positions; a byte
total just under 4 GiB can still exceed the budget once the reserved
positions are counted.

## 10. Data model

Names are illustrative; use repository-native names where they exist.

```cpp
struct SourceFile {
  uint32_t file_id;
  std::filesystem::path path;      // or display_path for in-memory
  SourceBuffer buffer;
  uint32_t base_offset;
  LexResult lex;
  ParseResult parse;               // FileNode* is parse.file
  ModuleInfo* module = nullptr;    // null until the module table is built
};

struct ModuleInfo {
  uint32_t module_id;
  std::vector<std::string_view> segments;   // canonical identity
  std::string display;                      // "a::b::c", for diagnostics and mangling
  SourceFile* file;
  bool is_prelude;
  std::vector<ModuleInfo*> imports;         // edges, in declaration order
  Scope* scope = nullptr;                   // module scope; its declarations are the export table
};

struct Program {
  std::vector<std::unique_ptr<SourceFile>> files;    // file_id order
  std::vector<std::unique_ptr<ModuleInfo>> modules;  // module_id order
  std::vector<ModuleInfo*> topo_order;
  ModuleInfo* entry = nullptr;
  SourceMap source_map;
  std::vector<Diagnostic> diagnostics;               // load + graph diagnostics
};
```

`Symbol` gains its owning module (mirrors the bootstrap's
`owner_module_id`, Task 27 D2):

```cpp
struct Symbol {
  SymbolKind kind;
  std::string_view name;
  Span decl_span;
  const void* decl;
  const ModuleInfo* module = nullptr;   // null for builtins/predeclared
};
```

A `SymbolKind::Module` symbol's `decl` becomes the `ModuleInfo*`
(accessor `decl_as_module()`), not the `ImportNode*`; the import site is
recoverable from `decl_span`.

`ResolveResult`, `TypeCheckResult`, and the `uses` map stay program-wide
singletons: offsets are globally unique (§9.2), so no keying change is
needed.

## 11. Pass changes

### 11.1 Lexer / parser

`lex` takes a base offset.  The parser is unchanged.

### 11.2 Resolver

`resolve(Program&) -> ResolveResult`.

Pass 1, in topological order: create the builtins scope once; create
the prelude scope; for each module create its scope (parent: prelude
scope) and declare its top-level names; for prelude modules declare
into the prelude scope instead.  Before declaring any top-level name
into a module or prelude scope, check it against the builtins scope
and reject a hit with today's `duplicate top-level declaration`
diagnostic — `Scope::declare` only checks the target scope, so this
check is explicit, not inherited from nesting.  Reject a `__dao_`-
prefixed name unless the declaring module `is_prelude`, with today's
"reserved for compiler/runtime use" diagnostic; this replaces the
`name_span.offset >= prelude_bytes_` test.  Then bind imports:
`import a::b` declares `b` → `Module` symbol → `ModuleInfo` in the
importing module's scope.

Pass 2: resolve bodies exactly as today, with two additions:

- `b::name`: look up `b`; if it is a `Module` symbol, look up `name`
  in `b.module->scope->lookup_local(...)` (export table); record the
  use at the qualified name's offset; unknown export is an error
  ("module 'a::b' has no export 'name'")
- `extend` registration keys the method set on
  (target type, declaring module); lookup consults the current module
  and the prelude group

### 11.3 Type checker

`typecheck(Program&, const ResolveResult&, TypeContext&) ->
TypeCheckResult`, two passes across all modules in topological order:
register every module's declarations, then check every module's bodies.
This preserves cross-module forward references without an ordering
dependence, as Task 27 D5 established.

Qualified forms per §7.8 type-check by following the resolver's use
entries; the "qualified type names are not yet supported" error is
deleted.

### 11.4 HIR

```cpp
struct HirModule { Span span; const ModuleInfo* module; std::vector<HirDecl*> declarations; };
struct HirProgram { std::vector<HirModule*> modules; /* topo order */ };
```

`build_hir(Program&, …) -> HirBuildResult` returns a `HirProgram`.
`HirCall` callees already reference resolved `Symbol*`s, so
cross-module calls need no new node.  The HIR printer prints the
module header per `HirModule`.

### 11.5 MIR

`build_mir(const HirProgram&, …)` lowers every module's functions into
one `MirModule`, in topological order.  Generic templates remain keyed
by `Symbol*` and are program-wide, satisfying Task 28 §16
(cross-module instantiation).

### 11.6 LLVM backend

One `llvm::Module`.  Function naming per §12.  The `prelude_bytes`
warning filter in `lower(...)` becomes a `SourceMap` query.

### 11.7 Driver

`compiler/driver/pipeline` exposes the program-level stages:

```cpp
auto load_program(const ProgramOptions&) -> Program;         // discovery + graph
auto run_frontend(Program&) -> FrontendResult;               // resolve + typecheck
auto run_through_hir(...), run_through_mir(...), lower_to_llvm(...)
```

The single-file wrappers keep their names and become one-line
adapters over `ProgramOptions{root = path}` so `cmd_*` handlers change
only in how they print locations.

## 12. Symbol naming

One function, `mangled_name(const Symbol&) -> std::string`, used by
both the function-definition site and every lookup site in the backend
(17 today).

| Symbol | LLVM name | Rationale |
|---|---|---|
| `extern fn foo` | `foo` | `CONTRACT_C_ABI_INTEROP.md` §5: exactly as written |
| runtime hook `__dao_*` | unchanged | `CONTRACT_RUNTIME_ABI.md` |
| `fn main` in the entry module | `main` | C runtime entry |
| `fn f` in module `a::b` | `a::b::f` | unambiguous, readable in IR and debuggers; LLVM quotes it |
| method `T.m` in `a::b` | `a::b::T.m` | existing `.` method mangling preserved |
| instantiation `f$i32` in `a::b` | `a::b::f$i32` | existing `$` generic mangling preserved |
| builtin intrinsic (`size_of`, `ptr_cast`) | unchanged | bodies replaced by inline IR |

Recognition of special symbols is by identity, never by name pattern:
`is_builtin_intrinsic` applies only to symbols with no owning module
(the predeclared builtin function symbols the resolver creates), and
`is_runtime_hook` applies only to `extern fn` symbols, whose names are
never mangled.  A user function can therefore never be mistaken for an
intrinsic or a hook regardless of its name — and §11.2 rejects such
names at declaration anyway.  Names are not ABI-stable; the C-facing
export story stays reserved (§6.2).

Named-function callbacks passed to C (`examples/ffi/ffi_callback.dao`)
are unaffected: C receives a pointer, never the name.

## 13. Driver surface

```
daoc <cmd> <root.dao> [--module-root DIR]... [--stdlib-root DIR]
daoc <cmd> --source a.dao --source b.dao ... [--entry a::b]   # explicit set (§7.7)
daoc build ... [link inputs]                                  # unchanged passthrough
```

`<cmd>` ∈ `check`, `tokens`, `resolve`, `hir`, `mir`, `llvm-ir`,
`build`.  `lex`, `parse`, and `ast` stay single-file (they run before
module structure exists).  Debug dumps (`tokens`, `resolve`) print
user modules only, as they do today, selecting by `is_prelude` rather
than by offset.

## 14. Diagnostics

All diagnostics carry file provenance through `SourceMap`.  Task 31
adds at least:

```
main.dao:2:8: error: imported module 'app::util' not found (searched: ./, stdlib/)
util.dao:1:8: error: file 'app/util.dao' declares module 'app::utils', expected 'app::util'
b.dao:1:8: error: module 'app::math' already declared in 'a.dao'
main.dao:2:8: error: import cycle: app::main -> app::math -> app::main
main.dao:7:12: error: module 'app::math' has no export 'sub'
main.dao:3:8: error: 'math' is already declared in this module
main.dao:9:5: error: 'math::Vec::Item::x' — imports bind a single segment
error: no entry module: no module declares 'fn main' (use --entry)
error: ambiguous entry module: 'fn main' declared in app::main and tools::main (use --entry)
```

Wording of existing single-file diagnostics does not change.

## 15. Tooling and playground

- `compiler/analysis/*` signatures take the program-wide
  `ResolveResult` / `FileNode` they already take; hover, definition,
  references, and completion work across modules for free because
  `uses` and symbols are program-wide.  `document_symbols` drops its
  `prelude_bytes` parameter for a `SourceMap` query.
- The playground's `run.cpp` and `analyze.cpp` delete their private
  prelude concatenation (68 `prelude_*` sites) and call
  `load_program` with an in-memory `SourceInput`.  This is the
  concrete form of `CLAUDE.md` §5's "consume compiler analysis rather
  than bespoke language logic".
- The LSP, when built, receives a workspace = `Program` model without
  further substrate work.

## 16. Interactions

### 16.1 Generics (Task 28)

Unchanged.  Templates and instantiations are keyed by `Symbol*`, which
is program-unique; the concreteness invariant is unaffected.

### 16.2 Concepts and `extend`

Concepts are exported like other declarations (`b::Printable` in
bounds and `as` clauses).  Satisfaction checking runs at the `extend`
site as now.  Method-set visibility follows §7.5/§7.6.

### 16.3 Error tolerance (Task 16)

A parse error in one file must not prevent building the program: the
file's `FileNode` may carry error nodes; the module layer registers it
(with its declared identity if the `module` line parsed) so analysis
still produces partial results.  `build` still refuses to lower.

### 16.4 Bootstrap parity

`testdata/bootstrap/multifile/{smoke,cross_module_enum,extend_isolation}`
are valid programs for the host in explicit file list mode.  The host
must accept the first two and reject/accept `extend_isolation` exactly
as the bootstrap does.  These fixtures are read by both compilers and
edited by neither task.

Where the two compilers diverge from the contract today is recorded
in `CONTRACT_MODULE_SYSTEM.md` §12 and `bootstrap/README.md`: the
bootstrap conforms to §2–§5 and to the `b::f` / `b::T` / `b::E::V`
forms of §6, rejects `b::T::m` until it has methods, has no prelude
(§7) and no entry-module selection (§8), and verifies determinism at
graph level only (§9).  Task 33 closes those gaps on the bootstrap
side; Task 31 closes the host side.

## 17. Migration plan

Each slice lands green and behaviour-preserving unless noted.

- **D0 — Source map.**  `SourceFile`, `SourceMap`, `lex(base_offset)`;
  the prelude is loaded as separate files with base offsets instead of
  concatenated text; resolver/typechecker still see one file scope by
  declaring all files into it.  Every `prelude_bytes` / `prelude_lines`
  use is replaced by a `SourceMap` query; `module_utils.h` is deleted.
  All 523 host tests, all examples, and `task bootstrap-test` pass
  unchanged.
- **D1 — Program and graph.**  `Program`, `ModuleInfo`, discovery
  (§8), graph diagnostics (§8.5), topological order, `--module-root`,
  `--source`.  Single-file programs behave as before.
- **D2 — Resolver.**  Builtins/prelude/module scope shape (§7.6),
  `Symbol::module`, import → `ModuleInfo`, qualified resolution through
  export tables, module-scoped `extend` sets.  Shadowing rule lands
  here (§7.6; the one behaviour change).
- **D3 — Type checker.**  Program-wide two-pass; qualified forms
  (§7.8); cross-module call/type/variant checking; "qualified type
  names are not yet supported" deleted.
- **D4 — IR and backend.**  `HirProgram`, MIR flattening in
  topological order, `mangled_name`, entry-module `main` rule.  Golden
  HIR/MIR/LLVM fixtures regenerated once.  First multi-file executable
  builds and runs.
- **D5 — Tooling.**  Analysis APIs and the playground on
  `load_program`; playground concatenation deleted; `daoc` debug dumps
  select user modules by `is_prelude`.
- **D6 — Docs.**  `docs/building.md` documents `--module-root`,
  `--source`, and `--entry`; `IMPLEMENTATION_PLAN` and `ROADMAP` mark
  the task complete and name Task 32.  (The contracts already govern
  this task — §20; `ARCH_INDEX.md` gains the new subroot in D0, in the
  same diff that creates it.)

## 18. Test plan

### 18.1 Source map (D0)

- `base_n` equals the sum over files `< n` of `size + 1`
- position budget, as a pure function of injected sizes (no real
  allocation): `{2^32 − 2, 0}` fits (Σ = 2^32); `{2^32 − 1, 0}` is
  rejected; `{2^32 − 1}` alone is rejected; five empty files consume
  five positions; rejection happens before any base is assigned
- `locate` round-trips for the first byte, last byte, and EOF position
  of every file; `file_for(base_n + size_n)` is file *n*, not *n+1*
- an empty file between two non-empty files: its EOF locates to itself
  at `1:1`; its neighbours are unaffected
- the lexer's `Eof` token of every file maps back to that file
- a parse diagnostic at end of file (unterminated block in the middle
  file of three) prints that file's path and its last line, not the
  next file's first line
- a diagnostic in the third of three files prints that file's path and
  its local line

### 18.2 Graph (D1) — mirrors bootstrap graph tests 1–12

two files/one import; three-module chain topo order; missing module;
duplicate module; self-import; two-node cycle; deterministic cycle
trace; input-order independence; no-module file; acyclic dependent
excluded from trace; diagnostic file provenance; path/declaration
mismatch (host-only).

### 18.3 Resolver (D2)

qualified function/type/variant/static-method resolution; unknown
export; import binding collides with declaration; import binding
collides with import; prelude name visible unqualified; user
declaration shadows prelude name; prelude `extend` method visible in
user module; non-prelude `extend` method invisible across modules;
`import core::vector` + `vector::Vector` resolves to the prelude symbol;
user `fn size_of`, `class string`, and `fn null_ptr` are rejected with
today's `duplicate top-level declaration` diagnostic, in the entry
module and in an imported module alike; a prelude module redeclaring a
builtin is rejected the same way; user `fn __dao_x` is rejected with
today's reserved-prefix diagnostic while a prelude `extern fn __dao_x`
is accepted.

### 18.4 Type checker (D3)

cross-module call with wrong arity/type; `b::T` as parameter, return,
and field type; `b::E::V` construction and `match`; `b::T::m()`; generic
declared in one module instantiated from another (Task 28 §21.2).

### 18.5 IR, backend, end-to-end (D4)

- two modules both defining `add`: both lowered, distinct LLVM names,
  correct call targets
- intrinsic and hook recognition is by symbol identity: a MIR function
  whose symbol has an owning module is never treated as an intrinsic,
  and only `extern fn` symbols are matched against the runtime hook
  table (regression guard for §12 even though §11.2 rejects the names)
- `main` in a non-entry module is not `main`
- `daoc build` on a three-module fixture under `testdata/multifile/`
  (nested layout, discovery mode) produces an executable whose exit
  code and stdout match a golden
- the three `testdata/bootstrap/multifile/` fixtures in explicit mode:
  same accept/reject outcome as the bootstrap (§16.4)
- every existing example still builds and runs with identical output

### 18.6 Tooling (D5)

- hover/definition on `math::add` in the importing module jumps to
  the declaration in the other file (path + line)
- playground `/api/analyze` on a one-buffer program returns the same
  tokens and diagnostics as before D0

### 18.7 Determinism

`daoc llvm-ir --source ... --entry m` on the same file set in two
`--source` orders is byte-identical; the same set with a different
`--entry` differs only in which function is named `main`.

## 19. Acceptance criteria

1. `daoc build main.dao` compiles a program of ≥ 3 user modules across
   ≥ 2 directories via import-driven discovery, and the binary runs.
2. Explicit file list mode compiles the bootstrap's multi-file
   fixtures with the bootstrap's outcomes.
3. `prelude_bytes` / `prelude_lines` / `blank_leading_module` no longer
   exist in the tree.
4. Every diagnostic, including those at end of file, prints the real
   file path and local line.
5. All existing examples build and produce identical output; all host
   tests pass; `task bootstrap-test` passes with no bootstrap source
   change.
6. Two modules may declare the same function name.
7. The playground has no language logic of its own for program
   assembly.
8. Output is deterministic under input-order permutation for a fixed
   file set and entry selection.
9. The one behaviour change (prelude shadowing, §7.6) is covered by a
   test and called out in the changelog.
10. Every rule in §7 is stated in `CONTRACT_MODULE_SYSTEM.md`; the
    implementation cites the contract, not this spec.

## 20. Layout and contracts

The module graph, source set, and source map are source-facing concerns
(`CONTRACT_COMPILER_PHASES.md`: the frontend owns "source management
and file loading").  They go in a new subroot:

```
compiler/frontend/module/
  source_map.h/.cpp     SourceFile, SourceMap
  module_graph.h/.cpp   ModuleInfo, discovery, graph validation, topo order
  program.h             Program
```

This is the host counterpart of `bootstrap/graph/`.  The subroot is
already required by `CONTRACT_REPOSITORY_LAYOUT.md` and
`CONTRACT_COMPILER_PHASES.md` (both amended together with this spec),
and the language rules it implements are frozen in
`CONTRACT_MODULE_SYSTEM.md`; law precedes code.  `ARCH_INDEX.md` gains
the entry in D0, in the same diff that creates the directory.
Orchestration of passes over a `Program` stays in `compiler/driver/`,
which remains a library both `daoc` and the playground link.

## 21. Explicit deferrals

- **Task 32 — import forms and `assemble.sh` retirement.**  Decide
  selective/glob/alias import syntax (contract change), then convert
  `bootstrap/` to real modules and delete `assemble.sh`.
- **Task 33 — bootstrap module-system parity.**  Bring the bootstrap
  to `CONTRACT_MODULE_SYSTEM.md` §6 (`b::T::m`, once it has methods),
  §7 (prelude), §8 (entry module), and program-level §9 determinism;
  status table in `bootstrap/README.md`.
- visibility modifiers; packages and workspaces
- cross-module `extend` visibility, coherence, orphan rules
- separate compilation, object caching, incremental rebuilds
- a prelude manifest (replacing the two hard-coded stdlib directories)
- restructuring stdlib files to import each other explicitly
- ABI-stable / C-visible export of Dao symbols

## 22. Risks

- **Hidden offset assumptions.**  136 `prelude_*` sites are the known
  ones; grep for `< prelude`, `- prelude`, and hard-coded `"module main"`
  before D0 is declared done.
- **Name-keyed backend lookups.**  Any `getFunction(symbol->name)` that
  escapes `mangled_name` produces "function not found" at link time
  for one module and silently wrong targets for another.  D4 must
  route all 17 through the helper and add the two-modules-same-name
  test first.
- **Golden churn.**  HIR/MIR/LLVM goldens change once (module headers,
  qualified names).  Regenerate in one commit, review the diff for
  anything that is not a rename.
- **Scope nesting is not a collision check.**  Splitting builtins,
  prelude, and module into separate scopes makes `Scope::declare`'s
  local-only check insufficient for the rules that must not relax
  (builtins, `__dao_`).  Any future scope-shape change must re-run the
  §18.3 collision tests.
- **Prelude group as a semantic special case.**  It is the only place
  where "part of every scope" differs from "imported".  Keep it
  confined to scope construction (§7.6) so a future prelude manifest
  can replace the directory list without touching resolution rules.
- **Playground drift.**  If D5 slips, the playground keeps a second
  copy of program assembly and the contract line added in #256 is
  false again.  D5 is not optional.

## 23. One-sentence summary

Task 31 gives the host compiler the module model the language and the
bootstrap already have: many files, one program-wide offset space, real
import resolution, module-qualified symbols, and a prelude group that
keeps today's stdlib ergonomics intact.
