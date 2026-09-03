# Roadmap — Dao

This document is explanatory and sequencing-oriented. It is not a
normative contract unless a milestone is later promoted into
`docs/contracts/`.

## Guiding Delivery Shape

Dao should be built in stages that preserve feedback loops:
- freeze surface syntax and semantic taxonomy before broad implementation
- get a parser and diagnostics loop working before type-system ambition
- lower into target-agnostic IR before leaning on LLVM-specific features
- stabilize a minimal runtime and stdlib before broad language surface
- self-host only after the implementation language boundary is an aid,
  not an anchor
- treat the playground, semantic highlighting, and IntelliSense as core
  hardening loops rather than as late polish

## Phase 0 — Constitutional Baseline

Status: **complete**

Goals:
- repository constitution, contracts, and architecture index in place
- initial `.bonsai.yaml` / `.grove.yaml` in place
- language surface captured in syntax and execution-context contracts
- compiler topology frozen at a high level (frontend → HIR → MIR → LLVM)
- baseline semantic token taxonomy and initial LSP slice frozen in tooling
  contracts

Exit criteria:
- governance docs are internally consistent
- repository shape is stable enough for targeted implementation work

## Host Implementation Baseline

Frozen decision:
- the initial host implementation language is C++

Rationale:
- direct LLVM integration is cleaner
- compiler-control and memory-control concerns align with C++ better than
  with Go for this project
- it reduces early cross-language seams in the compiler core

## Phase 1 — Frontend Skeleton

Status: **complete**

Goals:
- indentation-aware lexer
- parser for declarations, statements, expressions, lambdas, pipelines,
  `mode`, and `resource`
- AST definitions and source-span plumbing
- first-class diagnostics with readable source reporting
- syntax probes and parser golden tests under `spec/` and `testdata/`
- reserve explicit compiler phase roots for `resolve/`, `typecheck/`, and
  lowering rather than allowing a monolithic semantic blob

Non-goals:
- optimization
- self-hosting
- advanced type inference

Exit criteria:
- representative Dao samples parse successfully
- syntax errors produce stable diagnostics
- AST shape is good enough to drive HIR lowering

## Phase 1.5 — Playground and Example Hardening Loop

Status: **complete** — Vite + TypeScript frontend with HMR, semantic
tokens, IR panels, run/console, generic print

Goals:
- bring up a small web playground tied to the local `examples/` directory
- support structural highlighting first, then compiler-backed semantic
  highlighting as soon as frontend analysis exists
- make diagnostics readability part of day-to-day UAT
- treat examples as both teaching corpus and regression corpus
- establish semantic token rendering and document-symbol inspection as core
  compiler feedback loops

Exit criteria:
- the playground can load and edit local examples
- compiler-produced diagnostics and semantic token streams are visible in
  the browser once frontend analysis is available

## Phase 2 — Semantic Frontend + HIR

Status: **complete** — resolve, typecheck, concepts, generics,
monomorphization, extend method lowering, HIR

Goals:
- name resolution and scope analysis
- type checking for foundational scalar, pointer, function, and container
  forms
- HIR construction preserving source-level meaning where valuable
- lowering of pipes and lambdas into analyzable HIR forms
- explicit representation of `mode` and `resource` semantics in HIR
- semantic tokens, document symbols, and hover classification driven from
  the same analysis

Focus decisions:
- keep HIR close to Dao mental models
- do not let LLVM details leak upward

Exit criteria:
- small programs type-check end to end
- HIR dumps are readable and useful for debugging

## Phase 3 — MIR + Execution Semantics Lowering

Status: **complete** — basic-block MIR, HIR→MIR lowering, generator
coroutines, scoped mode/resource region lowering with enter/exit
instructions, reverse-order cleanup on early return

Goals:
- explicit control-flow lowering to MIR
- ownership-free but scoped lowering model for `resource memory ... =>`
- lowering preparation for `mode unsafe`, `mode parallel`, and future
  `mode gpu`
- canonical representation for calls, control flow, temporaries, and
  memory-region lifetimes

Focus decisions:
- MIR is where execution becomes explicit
- MIR remains target-agnostic

Exit criteria:
- MIR can represent A*, ETL pipelines, and numeric kernels without
  surface-language leakage
- region/resource lifetime boundaries are explicit in MIR

## Phase 4 — LLVM Backend + Native Driver

Status: **complete** — MIR→LLVM lowering, object emission, linking,
`daoc build`, runtime ABI

Goals:
- LLVM lowering for scalar arithmetic, control flow, calls, aggregates,
  and resource lifetime intrinsics
- compiler driver capable of producing object files, executables, and IR
  dumps
- host-target compilation on one primary platform first
- baseline debug info and source-location preservation where practical

Initial target posture:
- prioritize one host platform and one LLVM toolchain path
- broaden targets only after correctness and diagnostics stabilize

Exit criteria:
- Dao "hello world" compiles and runs
- small routing / ETL / numerics examples compile through LLVM

## Phase 5 — Runtime and Initial Standard Library

Status: **runtime backbone complete, stdlib incremental** — 14 runtime
hooks across 6 domains (io, eq, conv, gen, mem, str), scoped resource
domains with enter/exit handles, prelude with Printable/Equatable/
Comparable/Numeric concepts, string concat/length, generic math
(abs, min, max, clamp), range generator. Mode plumbing: unsafe is a
correct no-op; parallel/gpu deferred to Phase 8. Stdlib breadth
(io, numerics modules, containers) continues incrementally without
blocking Phase 6+ work.

Goals:
- runtime memory support for scoped resource domains
- initial mode plumbing for `unsafe`, `parallel`, and a staged `gpu`
  execution story
- foundational stdlib modules under `stdlib/core`, `stdlib/io`, and
  `stdlib/numerics`
- explicit ABI-facing surface needed for compiler/runtime interop

Stdlib priority order:
1. core scalar/container foundations
2. strings, slices, and iterators/pipeline support
3. IO and file surface
4. numerics / math / vector-friendly primitives
5. concurrency primitives only after semantics are stable

Exit criteria:
- the compiler-generated binaries can rely on a minimal runtime/stdlib
  without ad hoc host-language glue in normal execution

## Phase 6 — C ABI Interop and Host Integration

Status: **v2 complete** — v1 (scalar/pointer C ABI calls, driver link
passthrough, extern fn type validation, E2E examples) and v2
(struct-by-value with repr-C predicate, x86-64 SysV eightbyte
classification, byval/sret for >16 B structs, function pointer types
at extern boundary with named-function callbacks) both landed.
Variadics, C unions, and indirect calls through C-supplied function
pointers with struct params/returns are deferred.

Goals:
- stable C ABI entry/exit surface for initial foreign function calls
- ability to call C libraries from Dao through explicit declarations
- ability to expose Dao functions as C-callable symbols where practical
- clear boundary between C ABI compatibility and broader C++ ergonomics

Important boundary:
- initial compatibility target is the C ABI
- direct C++ source-level interop is deferred and may be served through C
  shims first

Exit criteria:
- small Dao programs can call into a C library
- Dao-produced artifacts can be linked into a C/C++ host through the C ABI

## Phase 7 — Bootstrap Compiler

Status: **Tier A frontend-to-IR-to-text pipeline complete** — Tasks
19–30 complete.  Eight bootstrap subsystems share a consolidated
substrate (`bootstrap/shared/base.dao`): lexer (105 tests), parser
(51 tests), graph (12 tests), resolver (34 tests), type checker
(43 tests), HIR lowering (22 tests), MIR lowering (8 tests), and
LLVM backend (17 tests) — 292 bootstrap tests total.  The Tier A
pipeline covers lex → parse → resolve → typecheck → HIR → MIR →
LLVM text.  The `Program` value threads through resolve →
typecheck → HIR at both single-file and program level with canonical
type
identity, resolver-bound concept identity, module-scoped extend
methods, cross-module qualified name typing, and program-level HIR
aggregation (`HirProgram`/`HirModule`).  On-disk multi-file test
fixtures exercise the full pipeline end-to-end.  Task 28 (generic
body lowering boundary) enforces clean separation of generic
templates from monomorphic functions at the HIR → MIR boundary in
the host compiler.  Task 29 (bootstrap MIR) lowers HIR to a
basic-block MIR mirroring the host compiler structure with full
control flow (if/else, while) and diagnostic-emitting unsupported-
kind handling.  Task 30 (bootstrap LLVM backend) lowers MIR to
deterministic textual LLVM IR via a backend-private mini-IR and
text serializer, with alloca-everything SSA, param seeding, fail-
closed type/terminator validation, and correct string-literal
escape handling.  The first Tier B slice (#255) adds struct type
definitions, struct construction, `extractvalue` field reads, and
struct-typed call/return edges to the bootstrap MIR → LLVM path.
Next: `llc`/`clang` validation of emitted IR (Task 30.5), then
further Tier B bootstrap feature slices.

Goals:
- begin implementing non-trivial compiler subsystems in Dao itself
- establish a bootstrap chain from host implementation → mixed
  implementation → self-hosted compiler
- keep test parity between the host compiler and the Dao-implemented
  compiler as the handoff proceeds

Entry decision:
- six bootstrap probes (mini_lexer through type_checker) proved language
  viability but are architecturally too simplified to extract from
  directly (flat scope, integer type constants, string-only diagnostics)
- the diagnostic formatter is the correct entry leaf because it is a
  genuine compiler subsystem, is fully isolated, and forces the shared
  substrate (Span, SourceBuffer, line/col mapping, string formatting)
  that every subsequent extraction depends on
- see `docs/task_specs/TASK_19_DIAGNOSTIC_FORMATTER.md` for the full
  task spec
- the full lexer was promoted from probe to maintained bootstrap
  subsystem in Task 20 — see `docs/task_specs/TASK_20_BOOTSTRAP_LEXER.md`

Recommended bootstrap sequence:
1. keep the initial compiler in an implementation language suited to
   rapid frontend/backend construction
2. implement leaf or utility components in Dao first — starting with
   diagnostic formatting (Task 19)
3. promote probes into maintained subsystems once parity is verified —
   lexer (Task 20) is the first extraction
4. migrate increasingly central compiler phases only when Dao can
   express them ergonomically and compile them reliably
4. reach stage-2 self-hosting before claiming the compiler is truly
   self-hosted

Exit criteria:
- the Dao compiler can compile itself with only bounded host assistance
- rebuild parity and test parity are stable across repeated bootstrap
  cycles

## Tooling Track — Semantic Tooling, IntelliSense, and Web IDE

### Tooling T1 — Semantic Highlighting

Status: **complete**

- compiler-produced semantic token streams
- frozen baseline token taxonomy implemented end to end
- category distinction for declarations, calls, types, lambdas, pipes,
  modes, resources, and bindings

### Tooling T2 — Initial IntelliSense Slice

Status: **complete** — all five analysis APIs implemented as shared
compiler analysis with playground HTTP endpoints.

- hover ✓
- go-to-definition ✓
- document symbols ✓
- references ✓
- completion ✓ (scope-aware identifier completion with type info)
- symbol identity hardening across compiler sessions — deferred
  (not blocking; identity is stable within a single session)

### Tooling T3 — Web IDE North Star
- AST / HIR / MIR panes
- workspace-aware browser surface
- persistent multi-file editing when compiler incrementality is ready
- future rename and refactor support only after edit safety stabilizes

## Phase 8 — GPU and Numerics Expansion

Goals:
- principled `mode gpu =>` semantics
- first-class numerics support mature enough for dense compute kernels
