# Task 46, the Compiler's First Real Module

Status: delivered
Phase: Tier B-Bootstrap, the first step of roadmap order 6
Scope: carry one real import through the host, the bootstrap compiler,
self-compilation and the tools, and keep generated sources fresh.

## 1. What Task 45 leaves

The crossing compiles one assembled file.  `assemble.sh` strips every
fragment's module declaration, so the compiler never compiles an import
of its own and every name shares one synthetic module identity.  The
design handoff measured 1,004 resolution errors with seven fragments
passed as separate files, because they depend on shared unqualified names.

## 2. The boundary

The file `bootstrap/ascii.dao` declares `module bootstrap::ascii` and holds
`is_digit`, `is_alpha` and `is_alnum`, the one definition of each.
`bootstrap/shared/base.dao` imports it and qualifies its six calls.
Every compile of a generated program passes the file explicitly: the
Taskfile's suites, `stage2.sh` and `audit_closure.sh` with its probe.

## 3. What the boundary changed

The `daoc build` command named its executable after the source whose path sorted
first, so every program would have been written to `bootstrap/ascii`.  It
now names the executable after the entry module's file, which is equally
independent of input order.

The `bootstrap-assemble` task watched `impl.dao` files and three test runners, so
an edit to any other suite reused stale generated source.  Its inputs now
cover every `impl.dao`, every `tests.dao`, the driver and `assemble.sh`.

## 4. Acceptance

- The regular suites and `bootstrap/stage2.sh <build dir>` pass, and
  stage 2 and stage 3 agree for the new source set.
- Reversing the explicit source order emits the same bytes.
- From a warm build, an edit to one omitted suite input regenerates.
- A call navigates to the authored module and back through references.
- A diagnostic in the module names its authored file and position.

## 5. Delivered

Measured on macOS arm64 (`arm64-apple-darwin25.6.0`), Apple clang
21.0.0, from `9f40922` with this change applied, using
`bash bootstrap/stage2.sh build/debug`.

| Check | Result |
|---|---|
| Stage 2 and stage 3 | byte-identical, SHA-256 `c9c56a21…bc3c` |
| Stage 2 with the sources reversed | the same SHA-256 |
| Host IR with the sources reversed | byte-identical |
| Module identity in the IR | `@"bootstrap::ascii::is_digit"`, no copy in the driver's module |
| Suites under stage 2 | lexer 105, parser 88, graph 12, resolver 64, typecheck 343, hir 47, mir 54, llvm 48, all passing |
| Suites built by the host | the same counts, all passing |
| `validate_ir.sh` | 39 modules accepted, 32 run |
| `ctest` | 20 of 20 |
| Test-only edit, former inputs | reported up to date, the edit absent from the generated file |
| Test-only edit, new inputs | regenerated, the edit present |
| Definition from the call at `driver.gen.dao:525` | `bootstrap/ascii.dao:6:4` |
| References from the call and from the definition | the same six sites |
| Diagnostic in the module, host and self-hosted driver | `bootstrap/ascii.dao:10:10` |

Measured again on x86-64 Linux, under Rosetta in a container, with clang
21.1.8.  Stage 2 and stage 3 are byte-identical, every suite passes under stage
2, and `audit_closure.sh` regenerated `docs/bootstrap_closure.md` with every
column at zero.  The first attempt exposed a segfault in stage 3 under an
unlimited stack, and the crossing now bounds its stack at 1 GiB.  A native x86-64
machine has not run it, so the cause of that segfault stays unconfirmed.

## 6. Known gaps

- Navigation starts from the generated file: a call in `base.dao` line
  522 is `driver.gen.dao` line 525.  This ends when `base.dao` is itself a
  module.
