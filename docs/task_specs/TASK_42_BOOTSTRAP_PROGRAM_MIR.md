# Task 42 — Bootstrap Program MIR

Status: implementation spec
Phase: Tier B-Bootstrap, authorized after Task 41
Scope: bring the real compiler corpus, with the prelude, through a
program-level HIR → MIR pipeline whose HIR and MIR have the host's
shape, one measured slice at a time.  Monomorphic functions lower to
MIR; generic bodies lower to templates, kept out of the module, as the
host keeps them.  A construct is lowered to its real meaning or
rejected explicitly, never lowered as something else.

## 1. What the merged-main audit shows

On `main` at the Task 41 head, `docs/bootstrap_closure.md` reads zero
diagnostics through lex, parse, resolve, typecheck and HIR for every
compiler program and prelude file, and:

```
mir = 943 … 4,432   (single-source adapter, no prelude)
```

That column measures nothing about the compiler: `lower_to_mir` runs
one file without the prelude, so every prelude name is `unknown name
'Vector'` / `'char_at'`.  Three facts stand behind it.

1. **No program-level MIR.**  `program_run_hir` lowers every module
   and keeps only counts (`HirData`); the arena is dropped, so nothing
   can lower a program to MIR.  `lower_to_mir` reads one module's
   tokens and source (`MS.toks`, `MS.src`, `MS.file_id`).
2. **Bootstrap HIR is not the host's HIR.**  Zero HIR diagnostics
   means nothing was rejected, not that the HIR means what the host's
   means.  The host (`daoc hir`) lowers:
   - `match` to a scrutinee `let` and an `if` chain over
     `EnumDiscriminant == <variant index>`, each binding a `let` of
     `EnumPayload variant=i field=j` — the bootstrap lowers it to
     `HirBinary(op = −1, scrutinee, <pattern as an expression>)`, no
     binding ever given its payload;
   - `P(1)` to `Construct P`, `Shape::Circle(r = 2)` to
     `EnumConstruct Shape.Circle` — the bootstrap to `HirCall`s whose
     callee is a type or a `HirQualName`;
   - `p.x = 3` to an `Assign` whose target is a place — MIR lowers it
     to `store _0.x`; bootstrap MIR rejects any non-local target;
   - `break`, `mode`, `resource` to `Break`, `mode_enter`/`mode_exit`,
     `resource_enter`/`resource_exit` — bootstrap MIR rejects all of
     them.
3. **Bootstrap MIR is Tier A.**  It has constants, binary/unary,
   load/store to locals, field reads, fn refs, calls, and
   `br`/`cond_br`/`return`.  The host's MIR (`mir_kind.h`) adds
   `Construct`, `EnumConstruct`, `EnumDiscriminant`, `EnumPayload`,
   `PtrOp`, `IndexAccess`, place stores, and region enter/exit — the
   kinds the compiler corpus uses (§1 of the closure report:
   `MatchStatement` ×968, `Assignment` ×10,424, `BreakStatement` ×64,
   `ResourceBlock` ×41).

## 2. The vertical Task 42 owns

One authorized task, several implementation PRs, each a measured
slice.  In dependency order:

1. **The program MIR driver.**  The HIR pass keeps its arena;
   `program_run_mir` lowers every module in program order, each with
   its own tokens, source and file id; a function whose declaration has
   type parameters or whose signature mentions a generic parameter is
   lowered as a template, recorded by symbol and kept out of the
   module's function list (the host's `hir_function_is_generic` rule,
   Task 28).  The audit's `mir` column runs the program pipeline with
   the prelude, counting the compiler program's own file, as the `hir`
   column does (Task 41 slice 12).  This names the true MIR frontier.
2. **`match` with the host's shape** — HIR discriminant tests and
   payload bindings; MIR `enum_discriminant` / `enum_payload`.
3. **Construction** — HIR `Construct` / `EnumConstruct`; MIR
   `construct` / `enum_construct`.
4. **Places** — assignment to a field (and what else the corpus
   stores through); MIR stores to places.
5. **Control and regions** — `break`; `mode` and `resource` blocks as
   MIR regions with their enter/exit.
6. **What the audit names next** — pointer operations, index access,
   or whatever family the measured frontier shows, each against the
   host's dump.

Every slice is checked against the host: `daoc hir` and `daoc mir` on
a probe are the parity target for shape, `daoc check` for verdicts.
Where contract and host disagree, stop and report.

## 3. Slice 1 — the program MIR driver

### Change

- `program_lower_hir` returns the program's HIR arena beside the
  program (`ProgramHir`) instead of dropping it — beside, not in
  `HirData`, which `shared/base.dao` gives programs that have no HIR
  types; `program_run_hir` is it without the arena.
- `MS` reads a module's tokens, source and file id from the module it
  is lowering; `program_run_mir(p)` walks the `HirProgram` root, sets
  them per `HirModule`, and lowers each `HirFunction`.
- Generic templates: lowered into the module's template list, not its
  function list.  A template carries a call through a bound with the
  member access as its callee, as the host's template does; a concrete
  function never has a type parameter's value, and there the call stays
  rejected.  A template has no code until Task 43 instantiates it, so
  the backend fails a module whose concrete code refers to one, rather
  than calling a function it never emits.
- MIR diagnostics become file diagnostics (the module's file), so the
  audit can count the compiler program's own file by path.
- `lower_to_mir` (single-source) goes through the same driver; its
  tests stay green.
- The audit's `mir` stage probes `program_run_mir` over the program
  with the prelude; the report's wording says so; the prelude table
  gains a `mir` column.

### Non-goals of the slice

Any new MIR construct; monomorphization; LLVM.

## 4. Non-goals (task)

- Monomorphization — instantiating templates, and naming the function
  a call through a bound calls (the backend keeps rejecting a concrete
  reference to a template) — Task 43.
- Program-level LLVM lowering — Task 44.  The Stage-2 crossing — Task
  45.  Retiring `assemble.sh` — Task 46.
- Generators / `for` over iterables, lambdas, `?`, unless the measured
  frontier shows the compiler corpus needs them.
- No language-syntax change.

## 5. Delivery

Spec PR (this file, the plan entry), then one implementation PR per
slice, stacked, each measured by rerunning the audit.  Each slice:
probe the host first, write the tests from the host's dumps and
verdicts, implement, one review pass whose findings are checked
against the host.

## 6. Tests (slice 1)

MIR suite:

- a two-module program (`app::util` defines `add`, `app::main` calls
  it) lowers to one MIR module holding both functions; the call's
  `fn_ref` names `add`'s symbol;
- an integer literal in the second module lowers with its own module's
  lexeme (per-module tokens, not the first module's);
- `fn id<T>(x: T): T -> x` lowers to a template, not a module
  function; a method on `class Box<T>` does too; a non-generic
  function beside them is in the module;
- a call through a bound in a template is carried with no diagnostic;
  the backend fails `main` calling `id(1)`, naming `id`;
- a MIR diagnostic is attributed to the file of the module it came
  from;
- the existing single-source MIR tests stay green.

Corpus, through the audit:

- the `mir` column is measured through the program pipeline with the
  prelude; the prelude-name family (`unknown name 'Vector'`) is gone;
- the next families (expected: `match`, construction, places, `break`)
  are named for the slices after;
- every existing bootstrap and host suite and `validate_ir` stay green.

## 7. Acceptance (slice 1)

1. `program_run_mir` lowers a whole program, prelude included, with
   per-module tokens and source; generic bodies are templates.
2. The §6 tests pass; every existing suite stays green.
3. `bash bootstrap/audit_closure.sh` rerun; `docs/bootstrap_closure.md`
   committed with the `mir` column measured through the program
   pipeline and the next frontier named.

## 8. One-sentence summary

The bootstrap lowers the whole program, prelude and all, to MIR with
the host's HIR and MIR shape — match, construction, places, regions —
so the compiler's own monomorphic code reaches MIR meaning what the
host's does, and what remains is instantiation.
