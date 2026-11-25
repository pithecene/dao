# ADR: Contextual Numeric Literals and Homogeneous Collection Literals

Status: **proposed**
Date: 2026-11-24

## Decision

1. Extend narrow contextual literal typing (`CONTRACT_TYPECHECKING_BASELINE.md` §3) to expression-bodied function returns, generator `yield` statements, and expected generic `enum class` variant payloads.
2. Define `[e_0, e_1, ..., e_{n-1}]` and `[]` as homogeneous `Vector<T>` value literals in `core::vector`, with left-to-right element evaluation in the active memory domain.

## Context

`DAO-H01` measured `1,825` `to_i64(<literal>)` calls and `510` `Vector<T>::new()` call chains across the repository. While both compilers already fit unsuffixed integer and float literals in `let`, assignment, binary, constructor, method-argument, and block `return` positions, three remaining contexts drop the expected type:

```dao
fn zero(): i64 -> 0                  // rejected in native daoc: defaults 0 to i32

fn naturals(): Generator<i64>
  yield 0                            // rejected in both compilers: defaults 0 to i32

let item: Option<i64> =
  Option::Some(value = 0)            // rejected in both compilers: infers Option<i32>
```

Likewise, both parsers already produce `ListLiteral` (`[1, 2, 3]` and `[]`), yet both type checkers reject the node because target container semantics were left unfrozen.

## 1. Contextual Literal Typing Rules

### 1.1 Unchanged invariants

- Already-typed expressions never undergo implicit width conversion (`CONTRACT_NUMERIC_SEMANTICS.md`). Explicit conversion functions (`to_i64`, `i64_to_i32`, `to_f64`) remain mandatory for non-literal expressions.
- Without an expected scalar type, unsuffixed integer literals default to `i32` and unsuffixed float literals default to `f64`.
- Unary negation or plus applied directly to an unsuffixed numeric literal (`-1`, `+1`) participates in contextual fitting as a literal form, including `INT64_MIN` (`-9223372036854775808`).

### 1.2 Authorized expected-type sinks

An expected concrete type `E` propagates downward into the following positions:

1. **Expression-bodied functions:** in `fn f(...): R -> body`, `body` is checked with expected type `R`, identical to `return body` in a block-bodied function (`CONTRACT_TYPECHECKING_BASELINE.md` §6).
2. **Generator `yield` statements:** inside a function returning `Generator<T>`, `yield expr` checks `expr` with expected type `T`.
3. **Generic `enum class` variant construction:** when `Enum::Variant(...)` is checked under an expected instantiated type `Enum<A_0, ..., A_{k-1}>` and carries no conflicting explicit type arguments, the expected type seeds the variant's unbound type parameters before checking payload expressions. Thus `Option::Some(value = 0)` under expected `Option<i64>` checks `0` against `i64` and produces `Option<i64>`.
4. **Parenthesized expressions:** `(expr)` forwards its enclosing expected type to `expr`.

If an explicit type argument or an earlier argument binds a parameter to `i32` (`Option::Some<i32>(value = 0)` under `Option<i64>`), the mismatch still diagnoses as an assignability error.

## 2. Homogeneous Collection Literals

### 2.1 Target type

Every list literal `[e_0, ..., e_{n-1}]` has semantic type `Vector<T>` (`core::vector::Vector<T>`). Dao introduces no separate array-literal or slice-literal syntax here, and heterogeneous element lists remain errors.

### 2.2 Element typing and contextual inference

1. **With expected type `Vector<E>`:**
   - For `[]`, the literal has type `Vector<E>`.
   - For `[e_0, ..., e_{n-1}]`, each `e_i` is checked in index order `0 .. n - 1` with expected type `E` (so `let levels: Vector<i64> = [0, 1]` types `0` and `1` as `i64` without `to_i64`), and each element must be assignable to `E`.
2. **Without an expected `Vector<E>` type:**
   - `[]` is rejected with a located diagnostic: `"cannot infer element type for empty collection literal '[]'; provide an explicit Vector<T> type"`.
   - For `[e_0, ..., e_{n-1}]` (`n >= 1`), `e_0` is checked with no expected type to establish element type `T`. If `T` retains open generic enum parameters, checking diagnoses an unresolved type. Each subsequent `e_i` (`i >= 1`) is checked with expected type `T` and must equal `T`.

### 2.3 Evaluation order, allocation, and value semantics

1. **Left-to-right evaluation:** `e_0, e_1, ..., e_{n-1}` evaluate strictly left to right before or as slots are populated. If an element traps or propagates via `?`, later elements do not evaluate.
2. **Single-allocation lowering:**
   - `[]` lowers identically to `Vector<T>::new()` (zero heap allocation until first `push`).
   - Non-empty `[e_0, ..., e_{n-1}]` allocates one contiguous backing buffer of capacity `max(8, n)` in the current `resource memory` domain (or default process domain), stores `e_0 .. e_{n-1}` at indices `0 .. n - 1`, sets `len = n`, and initializes `version = 0` and `gen = 0`.
3. **Value and domain semantics:** the resulting `Vector<T>` obeys the exact copy-on-write stamp and `copy_out` rules of `core::vector::Vector<T>`. Escaping a `resource memory` block copies the vector and any heap-owning elements outward once.

## Non-Goals

- No implicit numeric widening on variables, parameters, or call results.
- No map/dictionary literal syntax (`{k: v}`), tuple literal syntax, or fixed-size stack array syntax.
- No whole-program or cross-statement Hindley-Milner element inference (`let xs = []; xs = xs.push(1)` remains rejected).
