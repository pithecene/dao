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
  parameter passed by pointer; an extern's struct parameters and
  returns coerced to the C ABI (`llvm_abi.cpp`); runtime hooks declared
  with their canonical signatures (`llvm_runtime_hooks.cpp`).
- Values: string constants as private globals and a `%dao.string`
  aggregate; loads and stores through places (field GEPs, a pointer's
  deref); construction, enum construction, discriminant and payload;
  pointer operations; the intrinsics `size_of$T` / `align_of$T` /
  `ptr_offset` inlined.
- Operators: signed arithmetic checked (overflow intrinsics, a trap on
  overflow), unsigned, float; comparisons; `==` on strings through
  `__dao_eq_string`.
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
   operations, comparisons, string equality.
5. **Calls and the C boundary** — string parameters by pointer, extern
   struct coercion, runtime hooks with their canonical signatures.
6. **Regions** — resource and mode enter / exit.
7. **Generators** — frames, init / resume, iteration.
8. **The programs run** — each compiler program, compiled by the
   bootstrap, is linked with the runtime and passes its own suite.

Each slice: the host's IR for a probe (`daoc build` / `daoc llvm`) is the
parity target for what the IR means, `validate_ir.sh` proves LLVM
accepts every emitted module and that it runs, one review pass checked
against the host.

## 4. Non-goals

The Stage-2 / Stage-3 crossing (Task 45).

## 5. Acceptance

Every compiler program, lowered by the bootstrap, links with the runtime
and passes its suite; the audit's `llvm` column is measured through the
program pipeline and reads zero.
