# Task 41 — Bootstrap Semantic Frontend Closure

Status: implementation spec
Phase: Tier B-Bootstrap, authorized after Task 40
Scope: bring the real compiler implementation corpus (and the prelude
it needs) through resolve → typecheck → HIR with the host's semantics,
one measured slice at a time, so the Stage-2 program is semantically
valid rather than merely parsed.  Not a sequence of syntax-enabling
microtasks: a construct is either given its real meaning or rejected
explicitly, never parsed and ignored.

## 1. What the merged-main audit shows

On `main` at the Task 40 head, the closure audit
(`docs/bootstrap_closure.md`) reads, for every compiler program:

```
lex = 0   parse = 0   resolve = one family   typecheck = the frontier
```

- **resolve** is entirely `unknown name 'print'`.  Task 40's closure
  surfaces section shows `print` has **zero** compiler-library call
  sites — all 564 are the test harness's.  The compiler-path resolve is
  closed; `print` is a harness requirement whose closure needs the
  whole `derived concept` → constrained-generic → `x.to_string()`
  vertical, and it is not chased here to green a column.
- **typecheck** is the real compiler frontier.  The families, by
  frequency (probe sample, `llvm` program):
  - `type mismatch in '+': i64 vs i32` and its `-`, `==`, assignment
    siblings — integer literal width;
  - `constructor arity mismatch: expected 7 fields, got 0` — generic
    static factory calls typed as zero-arg construction;
  - `unknown type in annotation`.

## 2. The vertical Task 41 owns

Task 41 is one authorized task with several implementation PRs, each a
measured slice.  It owns, as the compiler-path closure needs them:

1. contextual integer-literal typing (slice 1 below);
2. generic static-factory / constructor-vs-method typing —
   `Vector<T>::new()`, `HashMap<V>::new()`, `Builder::new()`;
3. `unknown type in annotation` — qualified / generic annotation typing;
4. generic parameter bounds **with meaning** (a bound is a conformance
   obligation, not ignored syntax), reusing the bootstrap's existing
   concept machinery and the parser prior work at tag
   `task41-genbounds-prior-work` — not a parallel model;
5. `derived concept` represented distinctly, its normative meaning
   preserved (never silently erased);
6. constrained generic method availability (`x.to_string()` justified
   by `T: Printable`), with no special-casing of `print` or `Printable`;
7. generic call inference / substitution as a general mechanism;
8. `mode unsafe` and `yield` at the frontend — parsed, preserved,
   legality enforced, carried into HIR — with lowering left to later
   tasks (fail-closed, never erased, per Task 37's standard).

Each slice mirrors the host and the numeric / type contracts.  Where a
discrepancy is found, the procedure is: read the contract, read the
host, implement parity; if contract and host disagree, stop and report.

## 3. Slice 1 — contextual integer-literal typing

### Root cause

`check_int_literal` in the bootstrap types **every** integer literal
`i32` (`bootstrap/typecheck/impl.dao`, `ts_set_expr_type(..., to_i64(2))`),
and `check_binary_expr` requires the operands be `types_equal`.  So
`n + 1` where `n: i64` is `i64 + i32` and diagnoses `type mismatch in
'+': i64 vs i32`.  The compiler corpus mixes `i32` literals with `i64`
values throughout, and the host compiles it.

### Host behaviour (the parity target)

`TypeChecker::check_int_literal` (`compiler/frontend/typecheck/type_checker.cpp`)
adopts the **expected** integer type when there is one and defaults to
`i32` otherwise; `check_binary` checks the peer as context and
re-checks a literal operand against the peer's type, so a literal fits
its neighbour.  This is contextual literal fitting, not implicit width
coercion between two non-literal integers — the numeric contract
(`CONTRACT_NUMERIC_SEMANTICS.md`) forbids the latter and lists general
numeric-literal polymorphism as a future extension; contextual literal
fitting is the host's implemented behaviour and the parity target.

### Change

- integer-literal typing takes an expected type: in an annotated
  `let x: T = <lit>` and a `return <lit>` against the function's return
  type, and in a binary arithmetic/compare/assignment where the peer
  operand is a concrete integer type, a literal adopts that integer
  type; absent context it stays `i32`;
- `check_binary_expr` fits a literal operand to a concrete-integer peer
  before the equality check, so `i64_value + 1` types as `i64`;
- no change for two non-literal integers of different widths — that
  stays a diagnostic, as the contract requires.

### Non-goals of the slice

Float-literal work beyond the same expected-type adoption if the corpus
needs it; unsigned/width inference beyond literal fitting; any implicit
conversion between named integer values.

## 4. Non-goals (task)

- The `print` / `derived concept` / constrained-generic vertical is
  pursued only when a slice above requires it for the compiler path,
  never to resolve the harness's `print`.
- Program-level MIR, monomorphization, backend, and the Stage-2
  crossing — Tasks 42–45.
- Retiring `assemble.sh` / real modules — Task 46.
- No new numbered task; no language-syntax redesign.

## 5. Delivery

Spec PR (this file, the plan entry), then one implementation PR per
slice, all Task 41, each starting from the previous slice's merged
head, each measured by rerunning the audit.  Slice 1 is the first
implementation PR.

## 6. Tests (slice 1)

Type-checker suite:

- `let x: i64 = 1` types the literal `i64`; `let y: i32 = 1` types it
  `i32`.
- `fn f(n: i64): i64\n  return n + 1` type-checks with no diagnostic;
  the `+` node is `i64`.
- `n + 1` and `1 + n` both fit the literal to `n: i64` (either operand
  order).
- a comparison `n == 0` with `n: i64` type-checks; the result is `bool`.
- two non-literal integers of different width (`a: i64`, `b: i32`,
  `a + b`) still diagnoses `i64 vs i32` — contextual fitting does not
  become width coercion.
- `return 0` in an `i64` function types the literal `i64`.

Corpus, through the audit:

- the `i64 vs i32` family (and its `-`/`==`/assignment siblings) leaves
  the compiler programs' typecheck histogram;
- the typecheck column falls; the next family (`constructor arity` /
  generic static factories) becomes the frontier for slice 2;
- every existing bootstrap and host suite stays green; `validate_ir`
  stays green.

## 7. Acceptance (slice 1)

1. Integer literals adopt an expected/peer integer type as the host
   does; absent context they are `i32`.
2. No implicit coercion between two non-literal named integers.
3. The §6 tests pass; every existing suite and `validate_ir` stay
   green.
4. `bash bootstrap/audit_closure.sh` rerun; `docs/bootstrap_closure.md`
   committed; the integer-width family is gone from the compiler
   programs and the new frontier named.

## 8. One-sentence summary

The bootstrap types an integer literal by its context, as the host
does, so the compiler's own `i64` arithmetic with literal operands
type-checks — the first measured slice of bringing the real Stage-2
program cleanly through the semantic frontend.
