# Task 40 — Bootstrap Corpus Boundary and Source Hygiene

Status: implementation spec
Phase: Tier B-Bootstrap, authorized after Task 39
Scope: separate the compiler implementation corpus from the bootstrap
test harness physically, and retire the marker-based source slicing in
`assemble.sh`, so the closure audit measures what the Stage-2 compiler
must compile rather than what its test executables happen to use

## 1. Objective

The bootstrap subsystems `resolver`, `typecheck`, `hir`, `mir` and
`llvm` each keep their library and their test runner in one
`impl.dao`, split only by a `// BEGIN_<X>_TESTS` comment.  A downstream
program that needs an upstream library slices it out with

```
sed '/^\/\/ BEGIN_RESOLVER_TESTS/,$d' bootstrap/resolver/impl.dao
```

This has three costs:

1. a subsystem's production code and its test `main()` are one file,
   so the closure audit — which measures the assembled programs —
   cannot tell a compiler requirement from a test-harness one;
2. the library boundary exists only as a comment, enforced by shell;
3. later phases are assembled through `mktemp` + `sed` surgery.

The compiler/test-harness split matters because it changes what the
audit says blocks Stage 2.  Measured (`docs/bootstrap_closure_split.md`,
this stack's #302): of the prelude call sites the corpus makes, the
`print` family — 569 sites, the chain behind the audit's costliest
semantic vertical — has **zero** compiler-library call sites.  It is
entirely test-status reporting.

Task 40 makes the boundary physical and removes the surgery.  It
changes no language semantics.

## 2. Required source shape

Each subsystem that has a library separates it from its runner:

```
bootstrap/resolver/impl.dao    library only, no fn main()
bootstrap/resolver/tests.dao   the test runner
bootstrap/typecheck/impl.dao   library only
bootstrap/typecheck/tests.dao  the test runner
bootstrap/hir/impl.dao         library only
bootstrap/hir/tests.dao        the test runner
bootstrap/mir/impl.dao         library only
bootstrap/mir/tests.dao        the test runner
bootstrap/llvm/impl.dao        library only  (backend lowering + closure probe library)
bootstrap/llvm/tests.dao       the test runner + the closure-probe main()
```

`lexer`, `parser` and `graph` already keep only a `tests.dao`; their
library is the shared base.  They are unchanged.

An `impl.dao` must not contain a test runner.  A `tests.dao` is never
compiled standalone — it is assembled after the base and every library
it exercises.

## 3. Remove marker surgery

`bootstrap/assemble.sh` composes explicit files in dependency order and
uses no `sed` and no `mktemp`.  A program lists the library fragments it
depends on, then its own test runner last:

```
resolver.gen  = base + resolver/impl + resolver/tests
typecheck.gen = base + resolver/impl + typecheck/impl + typecheck/tests
hir.gen       = base + resolver/impl + typecheck/impl + hir/impl + hir/tests
mir.gen       = base + resolver/impl + typecheck/impl + hir/impl + mir/impl + mir/tests
llvm.gen      = base + resolver/impl + typecheck/impl + hir/impl + mir/impl + llvm/impl + llvm/tests
```

The `BEGIN_*_TESTS` markers are deleted everywhere.

## 4. Behaviour preservation

The split is a pure reorganization.  The assembled `*.gen.dao` bodies
before and after Task 40 are identical line-for-line except for the
deleted marker comments and the rewritten fragment-header comments — no
code line changes.  Every existing bootstrap and host test passes
unchanged, and `validate_ir` is unchanged.  This equivalence is the
task's central check and is verified by diffing the assembled bodies.

## 5. Three closure surfaces

The audit gains an explicit split of the prelude call sites the corpus
makes, by the now-physical boundary:

- **compiler implementation** — `bootstrap/shared/base.dao` and every
  `impl.dao`;
- **test harness** — every `tests.dao`;
- (prelude closure is measured separately, as today, in the audit's
  prelude section.)

The audit reports the per-function library/harness split so the reader
can separate a compiler requirement from a reporting convenience.  This
is measurement only: it selects no task and changes no closure column.

## 6. Non-goals

- No language feature: no generic bounds, concepts, `yield`, `mode`,
  MIR or LLVM work.  Generic bounds are Task 41 (prior work preserved
  at tag `task41-genbounds-prior-work`).
- No change to `docs/bootstrap_closure.md`'s matrix.
- No move to real Dao modules yet: `assemble.sh` stays, composing
  explicit files.  Retiring `assemble.sh` itself is Task 46.
- No rename of subsystem directories or of `*.gen.dao`.

## 7. Delivery

Two PRs on the stack:

1. Spec (this file) + `docs/bootstrap_closure_split.md` (the motivating
   measurement) + `docs/ARCH_INDEX.md`.
2. Implementation: the physical `impl.dao`/`tests.dao` split, the
   rewritten `assemble.sh`, the audit's library/harness split section,
   `bootstrap/shared/README.md` and any subsystem header comments that
   describe the assembly, and the assembled-body equivalence check.

## 8. Tests

- `bash bootstrap/assemble.sh` succeeds and emits the eight programs
  with no `sed`/`mktemp`.
- The assembled bodies diff clean against the pre-Task-40 bodies except
  for comment lines (marker deletions, fragment-header rewrites): zero
  code-line differences.
- `task bootstrap-test` — every suite passes with the counts it had
  before (lexer 105, parser 75, graph 12, resolver 56, typecheck 55,
  hir 25, mir 9, llvm 20; `validate_ir` 19/19/13/0).
- `task test` — host suite unchanged (20/20).
- No `impl.dao` contains `fn main()`; no `tests.dao` is referenced as a
  library by another program.
- `grep -r BEGIN_.*_TESTS bootstrap/` finds nothing outside generated
  files.

## 9. Acceptance

1. Production code and test runners are physically separate files.
2. `assemble.sh` composes explicit files; no marker slicing remains.
3. Assembled programs are behaviourally identical (code-line-identical
   bodies); all suites and `validate_ir` pass at their prior counts.
4. The audit reports the compiler/test-harness split of prelude call
   sites.
5. No language semantics changed; no task number invented.

## 10. One-sentence summary

The compiler is one set of files and its tests are another, and the
build composes them explicitly — so the closure audit can tell what
the Stage-2 compiler needs from what its tests print.
