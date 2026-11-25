# Value-Producing Control-Flow Expressions and Divergence

Status: **proposed** (promotes to `docs/contracts/` upon acceptance)
Date: 2026-11-25

## Decision

1. Allow `if` and `match` in **value position** alongside statement position, supporting both single-line inline arms and multiline indented block arms whose final expression yields the branch value without `return`.
2. Preserve `return` strictly as enclosing-function return everywhere. Inside a value-producing arm, `return`, `break`, loop `continue`, and `panic(...)` carry the uninhabited bottom type `never`, unifying with any expected branch type `T`.
3. Require exhaustiveness whenever `if` or `match` appears in value position: `if` requires an unconditional `else` branch, and `match` requires full variant coverage or a non-redundant trailing `_:` arm.
4. Track definite divergence across `if`, exhaustive `match`, `mode`, and `resource` suites so functions whose paths all return or trap require zero trailing dummy `return` or `panic("unreachable")` statements.

## Context

Across `bootstrap/`, `stdlib/`, and `examples/`, statement-only `if` and `match` force two recurring patterns of ceremony:

1. **Pre-declared scratch bindings mutated per branch:** authors write `let padding: string = ""` followed by a three-line `if m < 10:` mutation (`examples/forecast_plans.dao`), or `let rows: i32 = 0` before a `Result` `match` whose error arm returns early. When the target type lacks a cheap sentinel, authors must extract a small helper function solely to use `return` per arm.
2. **Dead trailing fallthroughs:** `stdlib/core/vector.dao` and `stdlib/core/hashmap.dao` end `mode unsafe =>` methods with unreachable `panic("unreachable")` lines because the compiler lacks definite-divergence propagation through `mode`, `resource`, and exhaustive `match` suites.

Expression `if` and `match` eliminate mutable scratch placeholders while preserving Dao's arrow taxonomy. Colon `:` introduces control-flow branches, `->` marks lambdas and expression functions, and `=>` marks `mode` and `resource` blocks.

## 1. Surface Syntax and Indentation Discipline

### 1.1 Inline and Block-Bodied `if`

An `if` construct takes either inline expressions or indented suites after `:`, chaining zero or more `else if` clauses and an optional (in statement position) or mandatory (in value position) `else:` clause:

```dao
let sign = if x < 0: -1 else: 1

let tier = if score >= 90:
  let bonus = compute_bonus(score)
  format_tier(name = "gold", total = score + bonus)
else if score >= 70:
  format_tier(name = "silver", total = score)
else:
  format_tier(name = "standard", total = score)
```

Lexer and parser rules stay context-free:

- **1-token branch dispatch:** immediately after `:`, `TokenKind::Newline` opens an indented suite (`NEWLINE INDENT ... DEDENT`). Any expression-start token parses an inline expression branch.
- **No mixing inline and suite branches in one `if` chain:** an inline `if cond: a else: b` keeps every branch inline, whereas a multiline `if` uses indented suites for every branch.
- **Parenthesis indentation boundary:** Dao's lexer suppresses `NEWLINE`, `INDENT`, and `DEDENT` whenever `paren_depth > 0` (`(...)` and `[...]`). Inline `if cond: a else: b` uses no indentation tokens and works inside call arguments and collection literals (`clamp(if fast: x * 2 else: x, min = 0, max = 100)`). Block-bodied `if` and `match` expressions require `paren_depth == 0` (`let` initializers, assignments, `return`, `yield`, expression-bodied `->` functions, and statement level), keeping the lexer free of parser feedback and preventing deep multi-suite nesting inside argument lists.

### 1.2 Inline and Block-Bodied `match`

A `match` construct opens an indented arm list after `match scrutinee:`. Each arm header (`pattern:` or `_:` or `Variant as binding:`) takes either a single-line expression followed by `NEWLINE` or an indented suite (`NEWLINE INDENT ... DEDENT`):

```dao
let tag = match visibility:
  Visibility::AsOfDecision: "as_of_decision"
  Visibility::LatestNow: "latest_now"

let device_id = match step:
  Step::Upload(input, buffer):
    let host_id = lookup_host(host_env, input)?
    bind_device(device_env, name = buffer, id = host_id)
  Step::Reply(host):
    lookup_host(host_env, host)?
  _:
    return Result::Err("unsupported step")
```

An individual `match` may combine single-line arms for simple variants with indented block suites for arms that bind local `let` variables or run side effects before yielding their final value.

### 1.3 Suite Tail Values vs Function `return`

Inside a block-bodied branch of a value-producing `if` or `match`:

1. Zero or more leading statements (`let`, assignment, `while`, `for`, statement-position `if`/`match`, `mode`, `resource`) execute in order.
2. Unless a statement diverges (`§2.3`), the suite must end with a **tail expression** (or a nested value-producing `if`, `match`, `mode`, or `resource` suite whose own tail yields a value). That tail expression supplies the branch value without a keyword.
3. Writing `return expr` inside an arm **always returns from the enclosing function**, never merely exiting the `if` or `match` expression. Function bodies themselves still require explicit `return` (`CONTRACT_SYNTAX_SURFACE.md`) unless declared with `->`.

## 2. Typing, Exhaustiveness, and Divergence

### 2.1 Statement Position vs Value Position

Context determines how the type checker checks an `if` or `match`:

- **Statement position** covers an `if` or `match` appearing as a non-final item in a suite, as the final item of a `void`-returning block body, or inside a loop body. Statement-position `if` may omit `else`, and arms need not share a result type.
- **Value position** covers `let` initializers, assignment right-hand sides, `return expr`, `yield expr`, `fn f(...): R -> expr`, binary/unary/call/index operands, pipeline operands, and tail positions of enclosing value-producing `if` or `match` suites.

### 2.2 Exhaustiveness in Value Position

Every runtime path through a value-position `if` or `match` must either yield a value of target type `T` or diverge:

1. **Mandatory `else` on value `if`:** omitting `else:` on a value-position `if` diagnoses `"if expression in value position requires an 'else' branch"`.
2. **Closed sums and open types in `match`:**
   - Over an `enum` or `enum class`, arms must cover all declared variants or end with a non-redundant `_:` catch-all (`spec/semantics/aggregate_initialization.md` §2.2). Missing variants diagnose `"non-exhaustive match on '<Type>': missing '<Variant>'"`.
   - Over scalar or `string` scrutinees (`i32`, `string`, `bool` literals), value-position `match` must end with `_:` (or cover both `true` and `false` for `bool`), diagnosing `"match expression on '<Type>' requires a trailing '_' wildcard arm"` otherwise.

### 2.3 Bottom Type (`never`) and Diverging Arms

Dao introduces the uninhabited bottom type `never` (internal `TypeKind::Never`, unspellable as a struct field or variable storage type) for computations that never fall through to their continuation:

- `return` statements, `break` statements, loop `continue` statements (when loop `continue` lands alongside `break`), and `panic(_ msg: string): never` produce `never`.
- In `stdlib/core/panic.dao`, `__dao_panic` and `panic` change return type from `void` to `never`.
- **Assignability:** `never` is assignable to **every** semantic type `T` without runtime conversion. No concrete value type is assignable to `never`.
- **Unification across arms:**
  1. When the enclosing context supplies an expected type `E` (`let count: i64 = if ok: 0 else: panic("bad")`), every non-diverging arm checks against expected type `E` (propagating contextual numeric literal expectations into arm tails). Diverging arms (`never`) satisfy `E` automatically.
  2. Without an enclosing expected type (`let count = match res: ...`), the first arm whose tail type is not `never` establishes result type `T`, and all later non-diverging arms must match `T` (`CONTRACT_TYPECHECKING_BASELINE.md` §4). If every arm has type `never`, the entire `if` or `match` expression has type `never`.
  3. If a value-position suite ends with a non-expression statement (`let`, assignment, `while`, `for`) that does not diverge, type checking diagnoses `"branch of value-producing '<if|match>' must end with an expression or diverge"`.

### 2.4 Definite Divergence in Functions and Resource Suites

Definite-divergence analysis eliminates dead fallthrough boilerplate in block-bodied functions:

- A suite **diverges** if any statement executes `return`, `break`, `continue`, or a `never`-typed expression. A suite also diverges when its final `if ... else:`, exhaustive `match`, or `mode` / `resource` block diverges on all paths.
- Any statement appearing after a diverging statement in the same suite diagnoses `"unreachable statement"`.
- A non-`void` block-bodied function whose body diverges satisfies definite-return checking without a trailing `return` or `panic("unreachable")`.
- Inside `resource memory Name =>`, yielding a value from a nested `if` or `match` allocates in `Name` while inside the block and runs the standard single-exit `copy_out` promotion (`CONTRACT_EXECUTION_CONTEXTS.md` Law 7) when the value escapes `Name`.

## 3. Interaction with `?` and `|>`

1. **Try propagation (`?`):**
   - Postfix `?` binds at postfix precedence, tighter than inline `if` and `match`.
   - Inside an arm (`if cached: read_cache(k)? else: fetch_live(k)?`), `?` unwraps `Result<T, E>` or `Option<T>` in whichever branch runs, propagating `Err` or `None` from the enclosing function and yielding `T` as the arm value.
   - Parenthesizing `(if cached: read_cache(k) else: fetch_live(k))?` checks both arms as `Result<T, E>` (or `Option<T>`) first and unwraps the chosen result once.
2. **Pipelines (`|>`):**
   - `if` and `match` expressions sit **above** `pipe_expression` in grammar precedence (`expression := if_expression | match_expression | pipe_expression`), so `if normalize_input: raw |> trim else: raw` parses `raw |> trim` inside the `then` branch without parentheses.
   - Feeding a conditional value into a pipeline parenthesizes inline `if` (`(if use_backup: backup else: primary) |> validate`) or binds a multiline `match` to a local `let` before the pipeline.

## 4. Formatting

Canonical formatting aligns every structural keyword with Dao's `indent_stack_` lexer (`2` spaces per indentation level, zero tabs):

1. **Inline `if`:** place on a single line only when neither branch contains another `if`, `match`, or suite, and total width stays within `100` columns:
   ```dao
   let pad = if minutes < 10: "0" else: ""
   ```
2. **Multiline `if` / `else if` / `else`:** place `if cond:` after `=` or `return` and indent each branch body `2` spaces from the statement's base column. Align `else if` and `else:` with that base column rather than `if`:
   ```dao
   let verdict = if stored.model != wanted.model:
     "rejected: model mismatch"
   else if stored.archive != wanted.archive:
     "rejected: archive mismatch"
   else:
     "reused"
   ```
3. **Value-producing `match`:** indent arms `2` spaces from the statement's starting column. Short pure expressions may sit on the arm line after `: `, while multi-statement or long arms break onto a `2`-space indented suite:
   ```dao
   let op_cost = match step:
     Step::Upload(..): 4
     Step::Readback(..): 4
     Step::Dispatch(kernel, ..):
       let base = kernel_weight(kernel)
       base + 1
     _: 0
   ```
