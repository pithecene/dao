# Task 44 — Bootstrap Program LLVM

Status: implementation spec
Phase: Tier B-Bootstrap, authorized after Task 43
Scope: lower the concrete program Task 43 leaves — every module, the
prelude included — to textual LLVM IR with the host backend's meaning
(`compiler/backend/llvm/`), link it against the runtime, and run it.
The exit condition is the compiler programs themselves: each
`bootstrap/*/…gen.dao` program, compiled by the bootstrap, links and
passes its own suite.

## 1. What Task 43 leaves

The whole program is concrete MIR: every function a module holds, every
type a layout.  The bootstrap backend is Tier A (Task 30): one module,
the scalar types, structs by value, direct calls — and it keeps its
per-function values in linear-scan side tables, which a compiler-sized
function cannot afford.

## 2. The host's meaning (the parity target)

- Types (`llvm_type_lowering.cpp`): every integer width signed and
  unsigned; `string` is `%dao.string = { ptr, i64 }`; `Ptr<T>` a
  pointer; a class a named struct; an enum without payloads `i32`, one
  with payloads `{ i32, [N x iK] }` sized and aligned by the target
  data layout; a generator `%dao.generator = { ptr, ptr }`.
- Functions (`declare_functions`): declared before any body; a string
  parameter passed by pointer; an extern with a class or enum parameter
  or return refused with a diagnostic (§4); runtime hooks declared
  with their canonical signatures (`llvm_runtime_hooks.cpp`).
- Values: string constants as private globals and a `%dao.string`
  aggregate; loads and stores through places (field GEPs, a pointer's
  deref); construction, enum construction, discriminant and payload;
  pointer operations, `offset` among them; the intrinsics `size_of$T` /
  `align_of$T` inlined.
- Operators: signed arithmetic checked (overflow intrinsics, a trap on
  overflow), unsigned, float; comparisons; `==` on strings through
  `__dao_eq_string`, `+` through `__dao_str_concat`.
- Regions: `resource memory` enter / exit through the runtime's domain
  hooks; `mode` enter / exit.
- Generators: a frame per generator function, init and resume
  functions; iteration through the frame.

## 3. Slices

1. **The program driver and its tables** — lower every module of the
   concrete program, each function reading its own module's tokens; all
   functions declared before any body; per-function value, slot and
   label tables keyed (HashMap), not scanned.
2. **The type universe** — every builtin width, `%dao.string`,
   pointers, named structs, enums with the host's layout, generators;
   data-layout sizes and alignments computed as the target's.
3. **Values and places** — strings, loads / stores through field and
   deref paths, construction, enum operations, pointer operations,
   intrinsics.
4. **Operators** — checked signed arithmetic, unsigned and float
   operations, comparisons, string equality and concatenation.
5. **Calls and the C boundary** — string parameters by pointer, the
   refusal of a by-value class or enum at an extern, runtime hooks with
   their canonical signatures.
6. **Regions** — resource and mode enter / exit.
7. **Generators** — frames, init / resume, iteration.
8. **The programs run** — each compiler program, compiled by the
   bootstrap, is linked with the runtime and passes its own suite.

Each slice: the host's IR for a probe (`daoc build` / `daoc llvm`) is the
parity target for what the IR means, `validate_ir.sh` proves LLVM
accepts every emitted module and runs each one with a recorded exit
code, one review pass checked against the host.  A program's suite
counts as passing on its reported result and its exit status, never on
its exit status alone: most suites exit 0 after a failed test.

## 4. Non-goals

The Stage-2 / Stage-3 crossing (Task 45).  The C ABI coercion of a class
or enum passed to or returned from an extern by value (`llvm_abi.cpp`):
the corpus has no such extern, and the backend refuses one rather than
miscompile it.

## 5. Acceptance

Every compiler program, lowered by the bootstrap, links with the runtime
and passes its suite; the audit's `llvm` column is measured through the
program pipeline and reads zero.

## 6. Delivered

Six slices, then what their review and the audit's run found: the
program driver with keyed tables (1); the host's type universe and
layouts (2); values, places, enum and pointer operations, intrinsics
and regions (3); operators (4); generators (5); the audit lowering each
program through the backend, linking and running it (6).  The audit's
run then exposed the fall-through a match leaves after arms that cover
every variant, which the backend had rejected as a function falling off
its end (7), and review brought the host's names — `module::name`, the
entry's `main`, a shared type name qualified by its module — extern
signature agreement, construction by `insertvalue`, and the types a
pointer or a yield reaches (8).  Every compiler program, compiled by the bootstrap, links and
passes its suite.
