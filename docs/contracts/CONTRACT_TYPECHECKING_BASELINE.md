# CONTRACT_TYPECHECKING_BASELINE

Status: normative
Scope: Dao initial type-checking behavior
Authority: semantic frontend baseline

## 1. Purpose

This contract freezes the initial type-checking stance for Dao so that
frontend, tooling, and later IR work can rely on a stable baseline.

## 2. Type-checking stance

Dao type checking is:

- explicit
- local
- predictable
- inference-assisted, not inference-dominated

The language must not drift into whole-program HM-style inference as
the default semantic model.

## 3. Initial inference scope

The initial inference scope is narrow.

Allowed baseline inference:

- `let x = expr` infers from initializer
- expression-bodied functions are checked against declared return type
- contextual expression typing may be used narrowly where explicitly
  implemented

Not part of the baseline:

- whole-function return inference
- global inference
- unconstrained lambda inference
- cross-binding inference chains as the primary typing model

## 4. Assignability baseline

Initial assignability is exact semantic type equality unless a narrower
exception is explicitly documented.

The baseline must not assume broad implicit numeric promotion.

## 5. Control-flow conditions

Conditions in `if` and `while` must type-check as `bool`.

## 6. Function checking baseline

Functions must have explicit return types in the current baseline.

Expression-bodied and block-bodied functions must type-check against
the declared return type.

## 7. Builtin and predeclared type distinction

Dao distinguishes:

- builtin scalar types
- predeclared named types
- compiler-supported special/internal types

### Builtin scalar types
- `i8`
- `i16`
- `i32`
- `i64`
- `u8`
- `u16`
- `u32`
- `u64`
- `f32`
- `f64`
- `bool`

### Predeclared named types
- `string`

`string` is not a builtin scalar.

### Compiler-standard types
- `Generator<T>`
- `Ptr<T>`

Their meaning is fixed by the compiler, and no module may declare
either name.

### Compiler-supported special/internal types
- `void`, if currently retained by implementation and examples

## 8. Pointer baseline

Pointers are the compiler-standard `Ptr<T>` (`ADR_RAW_POINTER_SURFACE.md`).

### Type formation
`Ptr<T>` requires exactly one type argument.

### Operations

| Operation | Type | Requires `mode unsafe =>` |
|---|---|---|
| `Ptr<T>::new()` | `Ptr<T>`, the null pointer | no |
| `p.get()` | `T` | yes |
| `p.set(value: T)` | `void` | yes |
| `p.offset(elements: i64)` | `Ptr<T>` | yes |
| `p.cast<U>()` | `Ptr<U>` | no |
| `p.is_null()` | `bool` | no |

`get`, `set`, and `offset` are errors on `Ptr<void>`, which has no
readable or writable value and no element size.  `offset` counts
elements of `T`.

### Assignability
Pointer types are invariant under §4: `Ptr<A>` is assignable to
`Ptr<B>` only when `A` and `B` are the same semantic type.  There is
no implicit conversion between `Ptr<T>` and `Ptr<void>` and no
covariance through a pointer or generic argument
(`Generator<Ptr<i32>>` is unassignable to `Generator<Ptr<void>>`).
`cast<U>()` is the only pointee conversion.

## 9. Pipe baseline

Pipe expressions are first-class and must be type-checked as such.

They must not be reduced to generic binary arithmetic-style checking.

## 10. Lambda baseline

Lambdas are expression-bodied.

Initial lambda typing may rely on contextual expected types. The
implementation must not silently commit Dao to broad unconstrained
lambda inference.

## 11. Diagnostics

The type checker must produce source-span-based diagnostics suitable
for CLI, playground, and later IDE/LSP surfaces.

## 12. Boundary rule

`typecheck/` consumes:

- AST
- resolution results
- semantic types

It must not define the semantic type universe itself.
