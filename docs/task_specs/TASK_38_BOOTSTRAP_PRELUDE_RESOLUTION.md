# Task 38 — Bootstrap Prelude Resolution

Status: implementation spec
Phase: Tier B-Bootstrap, sequenced by the closure audit after Task 37
Scope: the bootstrap compiler resolves its program in the environment
the host gives every program — builtins, the prelude, then the
program's own modules — so the corpus stops failing resolution for
want of declarations the prelude makes

## 1. Objective

After Tasks 36 and 37 the closure audit reports `lex = 0` and
`parse = 0` for all eight compiler programs, and every program's first
blocking diagnostic is in the resolver: `unknown name 'char_at'`,
`'Vector'`, `'Span'`, `'make_error'`, `'to_i64'`.  Those names are
prelude declarations (`core::string`, `core::vector`, `core::span`,
`core::diagnostic`, `core::convert`).  The bootstrap resolver declares
the compiler builtins and nothing else, and the probe hands it the
compiler program alone; the host compiles the same program inside a
prelude group (`CONTRACT_MODULE_SYSTEM.md` §7; Task 31 §7.6).

The task closes that environment mismatch with the prelude semantics
the contract fixes, so that the audit's later columns measure the
later stages against the program the Stage-2 compiler will actually
compile — prelude included — and so that the histograms stop being a
cascade of one absence.

This is the prelude row of `CONTRACT_MODULE_SYSTEM.md` §12, which Task
33 held as eventual parity work; the audit has sequenced it now.  Task
33 keeps its other rows (§6 `b::T::m`, §8 entry module, §9 determinism).
There is one prelude mechanism, this one.

## 2. What the contract requires (§7)

1. The prelude is every module under `stdlib/core/` and `stdlib/io/`
   (21 files at this head).  The set is the contract's; the host's
   `prelude_files` enumerates it in path order.
2. Prelude declarations are visible unqualified in every module, and
   prelude modules see one another unqualified, as one namespace.
3. A module's own top-level declaration shadows a prelude declaration
   silently; lookup is innermost-first.
4. Prelude modules keep their identities (`core::vector`) and resolve
   qualified too; both paths reach the same declaration.
5. Builtins and predeclared names can be neither shadowed nor
   redeclared by any module, prelude included.
6. Only prelude modules may declare, at top level, `__dao_`-prefixed
   names and the intrinsic family `size_of`, `align_of`, `ptr_offset`,
   `copy_out`.  A class's own `copy_out` method is not such a
   declaration (§7.8).

Scope order, outermost to innermost:

```
compiler builtins → prelude → module → function / block / lambda
```

## 3. What the bootstrap has today

- `SourceInput(path, source_text)`; `ModuleEntry(module_id, file_id,
  name)`.  No notion of a prelude group.
- `resolve_program` gives each module a `FileScope` whose parent is
  none and calls `populate_builtins` into that file scope, per module.
  Builtins and the module's declarations share one scope, so a
  declaration named like a builtin reads as a duplicate.
- Cross-module access is by import binding and export table only
  (`Module` symbols; `program_exported_symbol`).
- The reserved prefix and the intrinsic family are unenforced (the
  resolver's deferral inventory names them).
- The probe (`bootstrap/llvm/impl.dao`, `closure_probe_stage`) builds
  the program from one `SourceInput`: the assembled compiler program.

## 4. Measured: the prelude reopens the parser

Fed the 21 prelude files, the bootstrap parser at this head accepts 14
and rejects 7, each at a construct the parser's Tier B deferral list
already names:

| File | First rejection | Construct |
|---|---|---|
| `core/comparable.dao:2` | `derived concept Comparable:` | derived concept |
| `core/equatable.dao:15` | `derived concept Equatable:` | derived concept |
| `core/printable.dao:15` | `derived concept Printable:` | derived concept |
| `core/hashmap.dao:67` | `mode unsafe =>` | mode block |
| `core/vector.dao:38` | `mode unsafe =>` | mode block |
| `core/math.dao:2` | `fn min<T: Numeric>(a: T, b: T): T` | generic parameter bound |
| `core/range.dao:5` | `yield i` | yield |

This is the evidence §7 of the handoff asks for and it is reported,
never hidden: the parse column was closed over the compiler sources;
the Stage-2 program includes prelude constructs the bootstrap parser
does not read yet.  Task 38 loads the prelude as it is, lets the
parser's statement-level recovery keep what it can (a `class` whose
method body fails still declares the class; a rejected `derived
concept` declares nothing), and makes the audit say per prelude file
what is rejected.  The five acceptance names below are declared in
files that parse clean or in a declaration that survives recovery.

## 5. Design

### 5.1 Program model

- `SourceInput` gains `is_prelude: bool`; `ModuleEntry` carries it.
  Membership is explicit input, never inferred from a name at a
  resolver call site.
- `build_program` orders inputs as today (path order, so the result is
  a function of the file set); prelude modules take part in the graph
  like any module.  Prelude modules need no import edges to see one
  another (§7.3), so their relative order is path order.
- A prelude module that fails to parse is still a module: its file
  diagnostics carry provenance, and whatever declarations survive
  recovery are collected.

### 5.2 Scopes

`resolve_program` builds, once per program:

1. a **builtins scope** — `populate_builtins` runs once, into scope 0;
2. a **prelude scope**, child of the builtins scope — every prelude
   module's top-level declarations are collected into it, in topo
   order;
3. per module, prelude or not, a **module scope**, child of the
   prelude scope, holding the module's import bindings and — for a
   non-prelude module — its own declarations.  A prelude module's
   declarations go into the shared scope; its imports stay in its own
   module scope, so an `import` in a prelude module binds there and
   nowhere else (§3).

The single-file `resolve` path uses the same layout with an empty
prelude scope, so scope indices mean one thing in both paths:
scope 0 is the builtins, scope 1 the prelude.

Export tables stay per module: each is built from the module's own
declarations, looked up in the scope they were declared into.  Through
`import core::vector`, `vector::Vector` reaches the prelude-scope
symbol and `vector::make_error` is rejected as today ("module 'vector'
has no exported symbol 'make_error'"), although `make_error` is visible
unqualified (§7.5: a qualified path reaches the same declaration as
the unqualified name, and no other).

`scope_lookup` walks parents as it does now, so innermost-first
lookup gives §7.4 shadowing for free.  What the chain alone does not
give, checked at top-level declaration with the host's diagnostics:

- **rule 6** — declaring into the prelude scope or a module scope a
  name that the builtins scope holds is an error ("duplicate
  top-level declaration 'i32'");
- **rule 7** — a non-prelude module declaring a `__dao_`-prefixed name
  is an error ("'__dao_y': the '__dao_' prefix is reserved for
  compiler/runtime use"); a prelude module may;
- **rule 8** — a non-prelude module declaring `size_of` / `align_of` /
  `ptr_offset` / `copy_out` at top level is an error ("duplicate
  top-level declaration 'size_of'"); a prelude module may.  Methods
  are declared under their mangled name (`Box.copy_out`), so a class's
  own `copy_out` is untouched.

Unqualified lookup of an ordinary (non-prelude) module's declaration
from another module stays unresolved, as today (§3: only import
bindings and qualified paths cross module boundaries).

### 5.3 Ownership and identity

Symbols keep `owner_module_id`; a prelude declaration's owner is its
prelude module, so `core::vector::Vector` and `Vector` resolve to one
symbol with one owner.  Export tables of prelude modules are built as
today from their own declarations, so `vector::Vector` through an
import binding and the bare `Vector` name identify the same
`sym_idx`.

Method-set visibility follows (§7.2): `sym_visible_in_module`, which
the type checker's and HIR's method lookups filter by, admits builtins,
the current module's symbols, and symbols declared into the prelude
scope — so a prelude `extend` method participates in every module's
method set, as the host's does.  Typing the prelude's bodies stays out
of scope (§6); only the visibility of what the prelude declares is.

### 5.4 The probe

The audit writes the prelude file list (`stdlib/core/*.dao` and
`stdlib/io/*.dao`, path order — the host's `prelude_files`) beside the
probe marker; the probe has no directory listing and membership is
explicit input.  The `typecheck` probe stage — the one that runs the
program pipeline, resolve then typecheck — builds its `Program` from
that list, each file a `SourceInput` with `is_prelude` set, plus the
compiler program, and its `resolve` and `typecheck` columns count the
compiler program's own diagnostics (by path) with the prelude present.
The `parse` stage keeps measuring the compiler program alone (that
column is the compiler sources' closure).

The `hir`, `mir`, and `llvm` stages run the single-source adapters
(`lower_to_hir(src)`, `lower_to_mir(src)`,
`lower_source_to_llvm_text(src)`) as today, without the prelude: the
bootstrap has no program-level MIR or LLVM driver.  Those three columns
measure a program the Stage-2 compiler will not compile and are
excluded from frontier selection until the program pipeline reaches
them; the audit's narrative says so, and the next task chosen from
the audit comes from the resolve and typecheck columns and the prelude
section.

The audit gains a **prelude section** from one dedicated probe run over
the prelude group alone: per prelude file, its parse, resolve, and
typecheck diagnostic counts and first diagnostic, so the table in §4
is regenerated, never hand-kept, and the prelude's own later-stage
diagnostics are reported under their own paths.

### 5.5 Determinism

Prelude membership comes from the input set; module order is path
order; the prelude scope is filled in topo order of prelude modules,
which is path order.  Permuting the inputs changes nothing.

## 6. Non-goals

- Parsing the seven constructs of §4 (`derived concept`, `mode`
  blocks, generic bounds, `yield`): the audit sequences them next if
  they are the frontier; this task reports them.
- Typing or lowering prelude bodies; generic instantiation; `Vector<T>`
  / `HashMap<V>` semantics; monomorphization; resource MIR.
- Entry-module selection (§8), program-level determinism beyond §5.5
  (§9), `b::T::m` (§6) — Task 33.
- Task 32's source migration; `assemble.sh`.
- Any stdlib change.

## 7. Delivery

Two PRs:

1. This spec; `docs/IMPLEMENTATION_PLAN.md` (Task 38 entry; Task 33's
   prelude item points here); `CONTRACT_MODULE_SYSTEM.md` §12 and its
   `bootstrap/README.md` mirror: the §7 row reads "Task 38".
2. The implementation: §5.1–§5.4, the tests of §8, the audit script's
   prelude section, `bootstrap/README.md` resolver inventory (prelude
   scope in Tier A scope; reserved prefix and intrinsic family
   enforced), the §12 row updated to "partial (Task 38)" — scope
   order, shadowing, identity, reserved names, and method visibility
   conform; the seven files of §4 load partially; the `hir`, `mir`,
   `llvm` columns are not yet measured with the prelude — and the
   audit rerun on the head.

## 8. Tests

Resolver suite, with small synthetic prelude modules (a `SourceInput`
under `stdlib/core/` with `is_prelude`):

- **visibility** — a program module refers to a prelude declaration
  unqualified without importing its module; resolved.
- **shadowing** — a module's own `fn greeting` shadows the prelude's;
  the use resolves to the module's symbol; no diagnostic.
- **builtin protection** — a module (and a prelude module) declaring
  `fn i32()` diagnoses; the builtin stays.
- **identity** — `greeting()` and `greet::greeting()` (after
  `import core::greet`) resolve to the same `sym_idx`.
- **prelude as one namespace** — one prelude module refers to
  another's declaration unqualified; resolved.
- **isolation** — a declaration in ordinary module `a` stays unresolved
  in module `b` that neither imports nor qualifies it, prelude loaded.
- **order independence** — the same inputs in two orders resolve every
  use to the same symbol.
- **absence** — a name declared nowhere still diagnoses
  `unknown name`.
- **reserved names** — `fn __dao_x()` and `fn size_of()` in an ordinary
  module diagnose; in a prelude module they declare.
- **`copy_out` method** — `class Box` with its own `fn copy_out` in an
  ordinary module declares; no diagnostic.
- **prelude import isolation** — an `import` in a prelude module binds
  in that module only: another module's unqualified use of the binding
  is `unknown name`.
- **per-module exports** — after `import core::greet`, `greet::other`
  (declared in `core::other`) diagnoses `module 'greet' has no
  exported symbol 'other'`, while `other` resolves unqualified.
- **method visibility** — a symbol declared into the prelude scope
  passes `sym_visible_in_module` from any module (typecheck suite).

Corpus, through the audit with the real prelude loaded:

- `Vector`, `char_at`, `Span`, `make_error`, `to_i64` resolve in the
  compiler programs through their prelude declarations — the resolver
  histogram no longer lists them as unknown.
- The prelude section lists the seven rejected files of §4 with their
  first construct; a file that parses clean shows zero.
- Existing bootstrap cross-module tests, host tests,
  `task bootstrap-test`, and `validate_ir` stay green.

## 9. Acceptance

1. The bootstrap resolves its program with the real prelude group
   loaded as declarations — no corpus-specific name table anywhere.
2. The five names above resolve through their declarations.
3. Prelude and program declarations keep distinct owners; qualified
   and unqualified paths identify one symbol.
4. The §8 tests pass; every existing suite stays green; `validate_ir`
   stays green.
5. `bash bootstrap/audit_closure.sh` is rerun and
   `docs/bootstrap_closure.md` committed from the head, with the
   prelude section.
6. The audit — the resolve and typecheck histograms with the prelude
   present, and the prelude section — states the next frontier; the
   `hir`, `mir`, `llvm` columns, measured without the prelude, do not
   choose it, and the audit's narrative says so.  The plan's next task
   is written from that, after this task lands.

## 10. One-sentence summary

The bootstrap compiles its program the way the host does — builtins,
then the prelude as declared, then the program — and the audit says
which prelude constructs the bootstrap parser still lacks.
