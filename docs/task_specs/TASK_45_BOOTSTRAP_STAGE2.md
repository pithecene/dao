# Task 45 — The Bootstrap Crossing

Status: implementation spec
Phase: Tier B-Bootstrap, authorized after Task 44
Scope: make the bootstrap a compiler that compiles itself.  The host
builds the bootstrap compiler (stage 1); stage 1 compiles the bootstrap
compiler's own source (stage 2); stage 2 compiles it again (stage 3).
Stage 2 and stage 3 must emit the same module (the fixed point), and
every bootstrap suite, compiled by the stage-2 compiler, must pass.

## 1. What Task 44 leaves

Every stage of the pipeline runs over a whole program with the prelude.
The closure audit compiles each compiler program with the bootstrap and
links and runs it, and every suite passes.  What is still missing is a
compiler: something that takes files and writes a module.  Today the
pipeline is reachable only through a test runner's probe mode.

## 2. The driver

`bootstrap/driver/main.dao` runs the whole pipeline (lex, parse,
resolve, typecheck, HIR, MIR, monomorphization, LLVM) over the files a
manifest names, and writes one LLVM module.  The runtime has no command
line (`stdlib/io` reads and writes files), so its input is a file,
`bootstrap/driver/out/.manifest`, with one entry per line:

- `output <path>`: where the module is written;
- `prelude <path>`: a prelude file (CONTRACT_MODULE_SYSTEM.md §7);
- `source <path>`: a program file.

It reports every diagnostic, writes nothing when any is an error, and
exits nonzero when it writes nothing.  `assemble.sh` builds
`bootstrap/driver/driver.gen.dao` from every library fragment and the
driver, with no test runner.

## 3. The crossing

`bootstrap/stage2.sh <build dir>`:

1. stage 1: `daoc build bootstrap/driver/driver.gen.dao`, or a stage-1
   binary passed as `STAGE1=`;
2. stage 2: stage 1 compiles `driver.gen.dao` with the prelude, and
   clang links the module with the runtime;
3. stage 3: the stage-2 binary compiles the same source;
4. the fixed point: stage 2's and stage 3's modules are byte-identical;
5. every bootstrap suite (lexer through llvm), compiled by stage 2,
   linked and run, passes and exits 0; `validate_ir.sh` accepts and
   runs the IR the stage-2-compiled llvm suite writes.

The prelude is the host's: every module under `stdlib/core` and
`stdlib/io`, in path order.

## 4. Non-goals

Replacing the host; a command line for the runtime; optimization of the
bootstrap's output; the extern C ABI coercion the host performs for
classes passed by value (the backend refuses such an extern; the corpus
has none).

## 5. Acceptance

`bootstrap/stage2.sh` reports the fixed point and every suite passing
under stage 2.
