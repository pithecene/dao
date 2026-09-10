# Bootstrap Compiler — Dao

Self-hosting compiler subsystems written in Dao.

This directory contains maintained bootstrap infrastructure — code that
is intended to evolve into the self-hosted Dao compiler.  It is **not**
a probe or experiment directory.

## Shared substrate

`shared/base.dao` is the single source of truth for the bootstrap
frontend pipeline (token model, lexer, AST, parser).  Subsystem
`.dao` files are assembled from `base.dao` + subsystem `.part.dao`
fragments via `bash bootstrap/assemble.sh`.  See `shared/README.md`.

## Subsystems

### `lexer/`

Indentation-aware lexer matching the host compiler's token surface
(`compiler/frontend/lexer/`).

**Status**: promoted from probe (Task 20, Phase 7).

**What it supports**:

- All token kinds from `compiler/frontend/lexer/token.h`
- Indentation-sensitive INDENT/DEDENT emission
- Newline suppression inside parentheses and brackets
- Error tokens for tabs, inconsistent indentation, unterminated strings,
  and unexpected characters
- Source span tracking (offset + length) for all tokens
- Diagnostics as data (`LexResult.diagnostics`) — no inline printing

**Parity with host lexer**:

The bootstrap lexer matches the C++ lexer on:

- Token kind sequences for well-formed source
- Error-token emission (TK::Error) matching `emit_error()` behavior
- INDENT/DEDENT spans (`line_begin` position, correct lengths)
- Comment handling (skipped as trivia, comment-only lines don't
  affect indentation)

**Known limitations**:

- Single-file only — no imports/modules (Dao limitation, not lexer)
- No hex/octal/binary integer literal support
- No multiline string literals
- No raw string literals
- Escape sequences are consumed but not validated beyond `\\`
- No Unicode identifier support
- `vec_pop` is O(n) — rebuilds the vector on every pop

**How to run tests**:

```sh
# From repository root:
bash bootstrap/assemble.sh && daoc build bootstrap/lexer/lexer.gen.dao && ./bootstrap/lexer/lexer.gen
```

Tests include golden token stream assertions, malformed-input tests,
a comprehensive Dao snippet regression, and a self-lex smoke test
(reads and tokenizes its own source file).

### `parser/`

Recursive-descent parser producing an arena-indexed AST for a Tier A
slice of Dao syntax.

**Status**: promoted from probe (Task 21, Phase 7).

**Tier A syntax** (supported):

- Declarations: `fn` (block + expression-bodied), `extern fn`, `class`
  (fields only), `enum` (with payloads), `type` alias
- Statements: `let`, assignment, `if`/`else`/`else if`, `while`,
  `for...in`, `return`, `break`, `match`, expression statements
- Expressions: full precedence tower (pipe through primary), call,
  field access, index, try (`?`), lambda, list literals, qualified
  names
- Types: named, generic instantiation (`Vector<Token>`), pointer
  (`*T`)

**Tier B-Bootstrap coverage (added)**: call-site type arguments —
`Type<Args>::member(args)` and `f<Args>(args)` read with the host's
rule, the type arguments carried on `CallE` (Task 36); `resource <kind>
<name> =>` blocks and their suites (Task 37); named call arguments
(`f(name = expr)`), the names carried on `CallE` beside the arguments

**Tier B deferrals** (explicitly not supported yet):

- `concept`, `derived concept`, `extend`
- Class methods and conformance blocks
- `mode` blocks
- `yield`
- Function types (`fn(T): R`)
- Generic parameter bounds and `where` clauses
- `import` declarations
- Pipe continuation across newlines

**Architecture**:

- Single arena `Vector<Node>` with payload enum for all node kinds
- Shared `Vector<i64>` index list for variable-length children
  (params, args, body statements, fields, variants)
- Immutable `ParserState` threaded through all parse functions
- Structured fail-fast with diagnostics as data

**How to run tests**:

```sh
# From repository root:
bash bootstrap/assemble.sh && daoc build bootstrap/parser/parser.gen.dao && ./bootstrap/parser/parser.gen
```

Tests include golden parse tree assertions, error recovery tests,
and a self-parse smoke test (lexes and parses a real Dao source file).

### `resolver/`

Two-pass name resolver producing scope chains, symbol tables, and a
uses map over the bootstrap parser's AST.

**Status**: implemented (Task 22, Phase 7).

**Tier A scope**:

- Two-pass: collect top-level declarations, then resolve bodies
- Scopes: file, function, block, struct, lambda
- Forward references at file level
- `IdentE` lookup with `unknown name` diagnostics
- `QualNameE` first-segment resolution
- Type name resolution (no diagnostic on unresolved)
- Builtin type pre-population (i8–u64, f32, f64, bool, string, void)
- Duplicate declaration diagnostics
- `let` / `for` / lambda / param / field declarations
- `resource` block bodies in a block scope (Task 37)

**Tier B deferrals**:

- Imports, overload resolution, method mangling
- Generic type parameters and where clauses
- Concept / extend resolution
- Mode block scoping
- Match arm destructuring bindings

**How to run tests**:

```sh
bash bootstrap/assemble.sh && daoc build bootstrap/resolver/resolver.gen.dao && ./bootstrap/resolver/resolver.gen
```

### `typecheck/`

Type checker assigning types to expressions and validating type
correctness for Tier A Dao syntax.

**Status**: implemented (Task 23, Phase 7).

**Tier A scope**:

- Two-pass: register declarations (aliases, structs, enums, functions),
  then check bodies
- Expression typing: literals, identifiers, binary/unary ops, calls,
  field access, pipe
- Statement checking: let, assign, if/while (bool condition), for,
  return, break, match, expression statements, `resource` block
  bodies (Task 37)
- Struct constructors and field access
- Enum variant constructors
- Assignability via structural type comparison

**Tier B deferrals**:

- Generics, inference, substitution
- Concepts, extend, derived conformance
- Method tables, lambda contextual typing, list literal inference
- Try operator, deref/addr-of, index expressions

**How to run tests**:

```sh
bash bootstrap/assemble.sh && daoc build bootstrap/typecheck/typecheck.gen.dao && ./bootstrap/typecheck/typecheck.gen
```

### `hir/`

HIR lowering pass producing compiler-owned typed IR from the
type-checked AST.

**Status**: implemented (Task 24, Phase 7).

**What it does**:

- Lowers typed AST → flat `HirNode` arena with resolved types
- Expression-bodied fns desugared to block + return
- Match desugared to if/else chain
- Every HIR expression carries its resolved type index
- Pipe preserved as first-class node (MIR lowers it)
- HirMatch reserved for Tier B pattern compilation

**Tier A coverage**: literals, identifiers, binary/unary ops, calls,
field access, pipe, qualified names, let, assign, if/else, while,
for, return, break, match (desugared), expression statements,
fn/extern fn/class/enum/type alias declarations, `resource` blocks
(`HirResource`, Task 37)

**How to run tests**:

```sh
bash bootstrap/assemble.sh && daoc build bootstrap/hir/hir.gen.dao && ./bootstrap/hir/hir.gen
```

### `mir/`

Tier A MIR lowering pass producing basic-block MIR from HIR, mirroring
the host compiler's structure (`compiler/ir/mir/mir.h`).

**Status**: implemented (Task 29).

**What it does**:

- Lowers typed HIR → flat `MirNode` arena with `MirModule` root
- `MirFunction` owns a list of `MirLocal` slots (params first) and a
  list of `MirBlock` nodes (entry first); per-function state is
  accumulated on `MS.fn_blocks` / `MS.locals` and flushed on seal
- `BlockR` threads block-local instruction buffers and a `sealed`
  flag through statement lowering; `block_seal` rewrites each
  `MirBlock` with its instruction list offset and count
- `ExprR { br, value }` threads expression-level state back to the
  caller because Dao classes are value-copied across function
  boundaries
- if/else lowers to `cond_br → then/else → br → merge` with
  early-return detection
- while lowers to `br → header (cond_br) → body (br header) / exit`

**Tier A coverage**: functions, extern functions, params/locals,
constants (int/float/bool/string), binary/unary ops, let/assign,
calls (with `MirFnRef` callees), returns, field-access reads,
if/else, while.

**Tier B deferrals**: generators, monomorphization, `resource` blocks
(rejected with the unsupported-statement diagnostic, fail-closed, until
the domain is represented in MIR — `CONTRACT_MIR_BOUNDARY.md` §4, Task
37 §5), `mode` blocks, enum construction/discriminant/payload, lambda
/ closures, try operator, for-over-iterable, index expressions,
break/continue.

**How to run tests**:

```sh
bash bootstrap/assemble.sh && daoc build bootstrap/mir/mir.gen.dao && ./bootstrap/mir/mir.gen
```

### `llvm/`

LLVM backend: lowers bootstrap MIR to deterministic textual LLVM IR
via a backend-private Dao-side LLVM mini-IR and text serializer.

**Status**: Tier A implemented (Task 30); first Tier B slice
(struct field access + construction) implemented.

**What it does**:

- Lowers `MirResult` (Task 29 output) to textual LLVM IR as a string
- Backend-private mini-IR (`LlModule` / `LlFunction` / `LlBlock` /
  `LlInst`) separates semantic lowering from text serialization
- `MirBackendInput` bundles MIR + resolver symbols + type universe +
  type_info + tokens + source for the backend's needs
- Alloca-everything SSA strategy with mandatory param seeding in the
  entry block prologue
- Integer arithmetic (`add/sub/mul/sdiv/srem`) and float arithmetic
  (`fadd/fsub/fmul/fdiv`), integer/float comparisons
- Function calls with extern `declare` / non-extern `define`
- String literals as private module-global constants + GEP
  materialization at use site
- if/else → `cond_br` + then/else/merge blocks;
  while → header/body/exit blocks with back-edge
- Struct types: `%struct.Name = type { ... }` definitions emitted
  at module top; struct construction (`P(1, 2)`) via alloca +
  per-field GEP stores + load; struct field reads (`p.x`) via
  `extractvalue` on SSA struct values; struct-typed function
  parameters and return values serialized with correct
  `%struct.Name` type text at call and return edges
- Fail-closed: `MirErrorExpr`, anonymous symbols, unresolved
  `field_index`, and unsupported types produce explicit diagnostics

**Tier A coverage**: functions, extern functions, params/locals,
constants (int/float/bool/string), binary/unary ops, let/assign,
calls, returns, if/else, while.

**Tier B coverage (added)**: struct layout and field access
(construction, field reads, struct-typed params and returns).

**Tier B deferrals**: enum payloads, monomorphization, generators,
mode/resource hooks, lambdas, try, for-over-iterable, index
expressions, break/continue, LLVM C API integration,
`llc`/`clang` validation in CI.

**How to run tests**:

```sh
task bootstrap-test   # every suite, then validate_ir over bootstrap/llvm/out
```

or by hand (the suite writes its artifacts under `bootstrap/llvm/out`,
which is gitignored, so create it first):

```sh
rm -rf bootstrap/llvm/out && mkdir -p bootstrap/llvm/out
bash bootstrap/assemble.sh && daoc build bootstrap/llvm/llvm.gen.dao && ./bootstrap/llvm/llvm.gen
bash bootstrap/validate_ir.sh build/debug
```

The suite writes the IR of every fixture it lowers to
`bootstrap/llvm/out/<test>.ll` (and `<test>.exit` where the program's
result is known); `validate_ir.sh` compiles each with `clang`, links the
ones with an expectation against the runtime, runs them, and compares
exit codes (Task 30.5).  `task bootstrap-test` runs both steps.

## Module-system contract parity

`docs/contracts/CONTRACT_MODULE_SYSTEM.md` governs both compilers.
This table mirrors its §12 for the bootstrap side and must move in the
same change as the bootstrap work that closes a row.

| Contract section | Bootstrap status |
|---|---|
| §2 module identity | conforms (Task 25) — explicit file lists only; no path convention |
| §3 imports | conforms (Tasks 25–26) — one-segment binding, module-name-only exposure, cycle rejection |
| §4 exports, same-module access | conforms (Task 26) |
| §5 `extend` scoping | conforms (Task 26 §6.5) — module granularity, not imported |
| §6 qualified forms | `b::f`, `b::T`, `b::E::V` conform (Task 27 D4); `b::C` in conformance positions and `b::T::m` are rejected until the bootstrap has concepts and methods — Task 33 |
| §7 prelude | not implemented — the resolver declares compiler builtins only and loads no stdlib — Task 33 |
| §8 entry module | not implemented — no driver or entry concept; MIR flattens every function of every module — Task 33 |
| §9 determinism | graph construction conforms (Task 25 §8; graph tests 7–8); program-level output determinism is unverified — Task 33 |

## Relationship to probes

The `examples/bootstrap_probe/` directory contains earlier experimental
probes that informed the design of these subsystems.  Probes are
learning artifacts; bootstrap subsystems are maintained code.

After Task 20, `examples/bootstrap_probe/dao_lexer_v2.dao` is
superseded by `bootstrap/lexer/lexer.dao`.

After Task 21, the bootstrap parser supersedes the `expr_parser.dao`
probe. The probe is retained as a historical artifact.

## What comes next

- Tier B expansion (generics, concepts, extend, mode/resource)
- Diagnostic formatting integration
- Multi-file compilation (eliminate assembly workaround)
