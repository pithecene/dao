# Task 34 — Bootstrap Closure Audit

Status: **complete** — first audit generated; rerun with
`task bootstrap-audit` after every bootstrap slice.

## 1. Objective

Replace the generic "Tier B deferrals" list with a measured answer to one
question: **what does the Dao compiler's own source corpus need in order
to be compiled by the Dao compiler?**  The answer defines
**Tier B-Bootstrap** — enough Tier B to cross the bootstrap — as distinct
from **Tier B-Parity**, every feature promised for the tier
(`docs/ROADMAP.md`, "Delivery Sequence").

## 2. Why this task exists now

The Tier B backlog lists generators, monomorphization, modes and
resources, enum payloads, lambdas, `try`, `for`, indexing, and
`break`/`continue` as bootstrap deferrals.  Some of those the compiler's
own sources never use; some they use on every page.  Sequencing the
backlog by the list rather than by the corpus would spend the next
several slices on features the crossing does not need.  The audit makes
the corpus, not the list, the authority.

## 3. Method

Three mechanical measurements, produced by `bootstrap/audit_closure.sh`
and written to `docs/bootstrap_closure.md` (generated; never edited by
hand):

1. **Construct inventory.**  The host AST printer over every assembled
   bootstrap program (`bootstrap/*/*.gen.dao`), counting each printer
   label.  A construct absent from the inventory is not a bootstrap
   blocker whatever its Tier B status.
2. **Forced prelude instantiations.**  The host LLVM lowering of the
   largest program, listing every `core::` function it instantiates and
   how often.  This is the stdlib the bootstrap must be able to compile
   for itself; it is what makes generics and methods the first vertical.
3. **Self-compile matrix.**  The bootstrap pipeline over its own eight
   programs, stage by stage — `lex`, `parse`, `hir` (resolve, typecheck,
   and HIR lowering together), `mir`, `llvm` — recording the diagnostic
   count per stage and the earliest failing stage's first diagnostic.
   The probe lives in `bootstrap/llvm/impl.dao` (`closure_probe`) and
   runs only when `bootstrap/llvm/out/.closure_probe` exists, which the
   script creates around its run, so `task bootstrap-test` is unchanged.
   The script runs one process per program — a Dao panic aborts the
   process and must not hide the other programs' lines — bounded in
   virtual memory and wall time (`AUDIT_MEMORY_KB`, `AUDIT_SECONDS`;
   6 GiB and 600 s by default) so a runaway run is recorded as such
   rather than exhausting the machine.  The probe announces each stage
   on stderr, which is unbuffered, so a killed process still shows the
   stage it died in.

The first blocking diagnostic per program names the construct to
implement next for that stage.  Diagnostics are the bootstrap's own,
so the matrix is only as precise as its "unsupported ..." messages;
sharpening a message sharpens the audit.

## 4. Reading the result

- **Capacity comes first.**  The matrix records peak memory and wall
  time per program.  The first audit found the bootstrap unable to hold
  programs of its own size: 11–14 GiB and 30–40 s for its three smallest
  programs (3–4k lines) before reaching the LLVM stage, and more than
  16 GiB in `hir`/`mir` for the larger ones.  Until that is fixed the
  construct columns of the larger programs cannot even be measured, so
  the bootstrap's memory behaviour (state threaded by value through
  every lowering step, copying its vectors) is the first item of
  Tier B-Bootstrap, ahead of any language construct.
- **Then the stages in order.**  The first audit's parser column shows
  the bootstrap parser rejecting dozens of sites in every program
  ("expected expression") — syntax the compiler's own code uses that
  Tier A parsing does not cover — before any lowering question arises;
  the document's per-stage histograms name the messages.
- **Tier B-Bootstrap** = that capacity work, the parser gaps, then
  constructs in the inventory that the self-compile matrix rejects at
  `mir`/`llvm`, plus forced prelude functions the bootstrap cannot yet
  compile.
- A stage with zero diagnostics on every program is closed for the
  corpus.
- Constructs the inventory does not contain (and prelude functions the
  corpus does not force) join the backlog *after* the crossing.

## 5. Non-goals

- The audit does not decide implementation order inside
  Tier B-Bootstrap; the delivery sequence does (methods + generics +
  monomorphization first, then the execution surface the matrix
  demands).
- It does not run the bootstrap's semantic suites under a Dao-built
  compiler; that is the Stage 2/3 harness (Order 7).
- It is not a parity table for the module system (`bootstrap/README.md`,
  Task 33) or for the language contracts.

## 6. Deliverables

- `bootstrap/audit_closure.sh <build dir>` and `task bootstrap-audit`
- `closure_probe` in `bootstrap/llvm/impl.dao`, opt-in via marker file
- `docs/bootstrap_closure.md`, generated
- `IMPLEMENTATION_PLAN.md` entry; `ARCH_INDEX.md` entries

## 7. Keeping it current

Rerun after every bootstrap slice and commit the regenerated document
with the slice, so the matrix in `main` always describes `main`.  When a
stage closes for the corpus, its column reads zero across every row.
