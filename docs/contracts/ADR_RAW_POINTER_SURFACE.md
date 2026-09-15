# ADR: Raw Pointer Surface — Canonical `Ptr<T>`

Status: **accepted** (address formation, §9, remains open)
Date: 2026-09-15

## Decision

Dao has no pointer-specific sigil syntax.  There is one pointer type
spelling, `Ptr<T>`, and every pointer operation is an ordinary method
or static call on it:

```dao
Ptr<T>::new(): Ptr<T>             // the null pointer
fn get(self): T                   // read the pointee
fn set(self, value: T): void      // write the pointee
fn offset(self, elements: i64): Ptr<T>
fn cast<U>(self): Ptr<U>
fn is_null(self): bool
```

The following are rejected as Dao syntax:

```dao
*T          // pointer type
*p          // dereference
*p = v      // write through a pointer
&x          // address-of
```

Likewise rejected: intrinsic properties such as `p.value`, and public
operation names such as `load` / `store` or `read` / `write`.

## Context

The earlier surface borrowed C's punctuation: `*T` for the type, unary
`*` to read and to assign through, unary `&` to take an address, and
the free intrinsics `null_ptr<T>()`, `ptr_cast<T>(p)`, and
`ptr_offset(p, n)` for the rest.  That is two grammars for one type:
punctuation the parser, checker, tooling, and HIR each special-case,
and a set of free functions beside them.  Dao's surface is otherwise
parameterized types and method calls (`Vector<T>`, `Generator<T>`,
`Option<T>`), and excessive sigils are a non-goal.

## The rule

> Compiler-standard implementation is acceptable.  Compiler-special
> syntax and magic members are rejected.

`Ptr<T>` is a **compiler-standard type**:

- it is predeclared, and no module may declare or redefine `Ptr`;
- it has its own semantic type representation (a pointer to `T`);
- it lowers directly to a raw pointer in MIR and to `ptr` in LLVM;
- its methods have compiler-known identities, recognized by the
  receiver's semantic type being `Ptr<T>` — never by member spelling
  on an arbitrary value — and may be compiler-generated or
  intrinsic-backed.

In source it participates in ordinary syntax: `Ptr<i32>` is a normal
generic type application, `Ptr<i32>::new()` a normal static call,
`p.get()` a normal method call, `p.cast<u8>()` a normal method call
with an explicit type argument.

## 1. Type formation

`Ptr<T>` takes exactly one type argument.  `Ptr` alone, or with any
other argument count, is an error.  `Ptr<void>` is the opaque pointer.

## 2. Construction

`Ptr<T>::new()` is the null pointer of type `Ptr<T>`.  It follows
Dao's constructor/factory vocabulary; there is no separate
`null()` factory.

## 3. Read and write

```dao
mode unsafe =>
    let value: i32 = p.get()
    p.set(value + 1)
```

`get` yields the pointee; `set` stores into it.  There is no
assignable pointer place expression: memory access is always one of
these two calls.

## 4. Offset

`p.offset(n)` is `p` advanced by `n` elements of `T` — conceptually
`address + n * size_of<T>()` — and has type `Ptr<T>`.  `n` is `i64`.

## 5. Cast

`p.cast<U>()` converts the pointee type explicitly and has type
`Ptr<U>`.  It is the only pointee conversion.

## 6. Null test

`p.is_null()` is `true` exactly for the null pointer.

## 7. Unsafe boundary

| Operation | Requires `mode unsafe =>` |
|---|---|
| `p.get()` | yes |
| `p.set(v)` | yes |
| `p.offset(n)` | yes |
| `Ptr<T>::new()` | no |
| `p.is_null()` | no |
| `p.cast<U>()` | no |

`get`, `set`, and `offset` access or traverse untracked memory; the
rest operate on the pointer value itself.

## 8. Assignability and `Ptr<void>`

Assignability stays exact (`CONTRACT_TYPECHECKING_BASELINE.md` §4).
`Ptr<A>` is assignable to `Ptr<B>` only when `A` and `B` are the same
semantic type.  There is no implicit `Ptr<T>` ↔ `Ptr<void>` conversion
and no covariance through pointer or generic arguments:

```dao
let p: Ptr<i32> = Ptr<i32>::new()
let q: Ptr<void> = p              // error: Ptr<i32> differs from Ptr<void>
let r: Ptr<void> = p.cast<void>() // accepted
```

`Generator<Ptr<i32>>` is therefore unassignable to
`Generator<Ptr<void>>`; a generator of opaque pointers yields
`p.cast<void>()`.

`Ptr<void>` has no readable or writable value and no element size, so
`get`, `set`, and `offset` on it are errors.  It supports `new`,
`is_null`, and `cast`; to touch memory, cast to a sized pointee first.

## 9. Address formation (open)

Obtaining a `Ptr<T>` from an existing addressable place is left open
by this ADR.  It is no ordinary method: an ordinary call evaluates
its argument as a value, and an address needs the argument as a place.
Unary `&` stays removed, and no intrinsic property stands in for it.
No stdlib or runtime source needs address formation today; the
remaining uses are examples and tests, which are migrated without it.

Candidate directions, for a separate short decision:

1. **A compiler-standard place operation.**  One named operation whose
   single argument must be an addressable place (a local, a field path
   rooted at one, or memory reached through a `Ptr`), yielding
   `Ptr<T>`.  Semantics: the argument is evaluated as a place, never as
   a value; the operation is legal only in `mode unsafe`, and taking the
   address forces the place into addressable storage.  Implications: the
   one call form whose argument is a place instead of a value, carried
   as a place node in HIR and lowered to the place's slot address in
   MIR; the checker
   needs an addressability rule; resource law 7 must say whether such a
   pointer may leave its domain.
2. **Explicit storage objects.**  Locals are never addressable.  A
   compiler-standard storage type (a cell holding one `T`) is declared
   like any value and exposes its address through an ordinary method
   returning `Ptr<T>`.  Semantics: only storage deliberately declared as
   addressable has an address; the address lives as long as the cell's
   scope.  Implications: no place-argument call form; one more
   compiler-standard type with its own lowering to a stack slot; FFI
   out-parameters are written through a cell.
3. **No address formation.**  Pointers come only from allocation, the
   runtime, foreign code, `new`, `offset`, and `cast`.  Semantics:
   nothing on the stack is ever pointed at; a foreign out-parameter is
   an allocation in the current domain, read back with `get`.
   Implications: the smallest surface and no place semantics; each such
   call pays an allocation, reclaimed with its domain.

## Layers

| Layer | Vocabulary |
|---|---|
| Dao source | `Ptr<T>`, `new`, `get`, `set`, `offset`, `cast`, `is_null` |
| HIR | semantic pointer operations (read, write, offset, cast) |
| MIR / LLVM | `load`, `store`, `getelementptr`, pointer casts, `ptr` |

The naming applies to Dao source.  Implementation layers keep systems
vocabulary; no source-level punctuation semantics reach the contract.

## C ABI

`Ptr<T>` maps to `T*` and `Ptr<void>` to `void*`.  A pointer crossing
the boundary is a raw address with no ownership, lifetime, or aliasing
guarantee, and memory access through it requires `mode unsafe =>`.

## Migration

One surface only: after migration `*T`, unary `*`, and unary `&` are
rejected, with no compatibility mode.  The old free intrinsics
`null_ptr`, `ptr_cast`, and `ptr_offset` are no longer user API; an
implementation may keep them as internal lowering primitives that no
source name reaches (`CONTRACT_MODULE_SYSTEM.md` §7.8).  The
change lands as internal Task 41 slices: this ADR and the contracts,
then the host compiler, then the bootstrap compiler, then the
remaining stdlib, runtime, and example cleanup.
