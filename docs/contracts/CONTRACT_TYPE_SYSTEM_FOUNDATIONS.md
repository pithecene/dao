# CONTRACT_TYPE_SYSTEM_FOUNDATIONS

Status: normative
Scope: Dao semantic type foundations
Authority: language and compiler boundary contract

## 1. Purpose

This contract freezes the foundational type-system direction for Dao.

It does not freeze final surface syntax for generics, conformance, or
pattern matching. It does freeze the semantic categories and
architectural boundaries that compiler work may rely on.

## 2. Type system stance

Dao is:

- statically typed
- nominal for named declarations
- algebraic for enums / sum types
- parametric for generics
- inference-assisted, but not inference-dominated

Dao is not a pure Hindley–Milner language and should not be designed
around whole-program inference as the primary organizing principle.

## 3. Foundational semantic type categories

The compiler semantic type layer must support, at minimum:

- builtin scalar types
- pointer types (spelled `Ptr<T>`; see `ADR_RAW_POINTER_SURFACE.md`)
- function types
- named types
- generic parameter types
- class types (named aggregate types)
- enum types
- alias-backed named references, whether aliases are preserved
  semantically or normalized later

These categories are semantic compiler concepts, not merely parser
syntax.

## 4. Builtin scalar surface

The language-level builtin scalar names are terse.

### Integers
- `i8`
- `i16`
- `i32`
- `i64`
- `u8`
- `u16`
- `u32`
- `u64`

### Floating-point
- `f32`
- `f64`

### Other
- `bool`

Additional builtins such as `isize` or `usize` may be introduced
later, but are not required by this contract.

## 5. Predeclared and compiler-supported types

Dao distinguishes builtin scalar types from compiler-known predeclared
named types.

- `string` is not a builtin scalar. It is a compiler-known predeclared
  named type. Its long-term home is the stdlib/core prelude, but for
  now it is predeclared by the compiler so that examples and early
  programs can use it without import.

- `void` is a compiler-supported return/internal type. It is used as
  the return type for functions that produce no meaningful value. The
  question of whether `void` stays surface-visible or becomes a
  unit-like type is deferred; for now it is available as a return type
  annotation.

These types are not part of the builtin scalar family and should not
be conflated with it.

## 6. Semantic builtin representation

The semantic type system must represent builtin scalar types
explicitly.

The semantic naming convention should follow the `Type*` family, for
example:

- `TypeBuiltin`
- `TypePointer`
- `TypeFunction`
- `TypeNamed`
- `TypeGenericParam`
- `TypeStruct`
- `TypeEnum`

The semantic representation may use enums such as `I32`, `U64`, `F32`,
`F64`, `Bool` internally.

## 7. Enums and algebraic data

Dao has two closed-type declarations:

- `enum` — closed set of fieldless atomic variants (classification)
- `enum class` — closed set of structured variants with named fields
  (branching with embodiment)

### 7.1 `enum` (fieldless)

`enum` variants carry no data.  They are pure classifications.

```dao
enum Visibility:
  Public
  Private
  Internal
```

Payload-bearing variants are not allowed in `enum`.  Use `enum class`
instead.

### 7.2 `enum class` (structured)

`enum class` variants carry named fields.  Each variant is a
structured alternative.

```dao
enum class Option<T>:
  Some(value: T)
  None
```

Rules:
- fields are always named — no anonymous positional payloads
- match is exhaustive with named destructuring
- `..` in match ignores remaining fields
- see `ADR_ENUM_CLASS.md` for full design rationale

### 7.3 Semantic representation

Both `enum` and `enum class` are represented as `TypeEnum` in the
compiler's semantic type layer.  `enum class` variants additionally
carry field name metadata.

### 7.4 Migration

Existing `enum` declarations with payload-bearing variants must
migrate to `enum class` with named fields.  This is a breaking change.
See `ADR_ENUM_CLASS.md` for the migration path.

## 8. Class semantics

A `class` in Dao is the canonical named aggregate type: a value-typed
product of named fields.

### 8.1 Value semantics

Classes are value types. Assignment copies and passing to a function
passes a copy unless an explicit pointer or reference mechanism is
used. Comparison and construction semantics are not yet frozen by
this contract.

There is no implicit heap allocation, no reference counting, and no
garbage-collected backing store for class values. Stack allocation is
the default; explicit allocation domains are expressed through the
`resource` construct.

### 8.2 No inheritance

Classes do not participate in inheritance hierarchies. There is no
`extends`, no superclass, no method-resolution order, and no
implicit vtable.

Composition, delegation, and conformance to concepts (section 10)
are the intended mechanisms for abstraction and polymorphism.

### 8.3 No implicit constructor magic

There are no synthesized default constructors, copy constructors, or
destructor chains. Construction syntax and its exact semantics are
not yet frozen; what is frozen is that implicit constructor magic is
not part of the class model.

### 8.4 No dynamic dispatch on class identity

Method dispatch on class values is static. Classes do not carry
type metadata or vtable pointers at runtime. Dynamic dispatch, if
needed, is expressed through concept objects or explicit indirection,
not through the class mechanism itself.

### 8.5 Field access and methods

Fields are accessed via `.` (dot notation). There are no synthesized
getters, setters, or property wrappers at the class level.

Methods may be declared inside the class body after all field
specifiers. A method with a bare `self` parameter receives the
enclosing class type as the receiver. Methods defined inside the
class body are semantically equivalent to methods attached via
`extend` blocks — they are ordinary functions with static dispatch.

### 8.6 Why "class"

The keyword `class` was chosen deliberately despite its OOP
connotations in other languages. In Dao, `class` means "a named
classification of data" — the taxonomic sense, not the object-oriented
sense. This reflects Dao's position that the word's overloaded history
in programming does not disqualify its precise meaning.

### 8.7 Non-goals

The following are explicitly not part of class semantics:

- inheritance or subtyping between classes
- implicit heap allocation or reference semantics
- synthesized special members (constructors, destructors, copy/move)
- runtime type identity or reflection on class values
- field visibility tiers (deferred; not ruled out, but not yet frozen)

## 9. Generics

Dao supports parametric generics.

The type system must be able to represent:

- generic type parameters
- generic arguments on named types
- instantiated generic named types

The semantic type layer must be designed so later generic substitution
and instantiation are possible without replacing the foundational
representation.

## 10. Concepts

Dao's abstraction and conformance mechanism is the **concept**.

A concept declares a set of method requirements that a type must
satisfy. Concepts are:

- statically resolved by default (monomorphization, no implicit vtable)
- capable of constraining generic type parameters
- less surface-heavy than Rust traits — conformance lives with the
  type via `as Concept:` blocks, not scattered in separate impl blocks
- derivable: a `derived concept` has automatic structural conformance
  for types whose fields all satisfy the concept, with explicit opt-out
  via `deny`

The keyword is `concept`. The detailed design is specified in
`docs/task_specs/TASK_12_TRAITS_AND_GENERICS.md`.

## 11. Nominal identity

Named declarations such as classes, enums, and concepts are nominal.

Two distinct named declarations are not equivalent merely because they
have structurally identical fields or variants.

## 12. Separation of layers

The compiler must preserve a strict separation between:

- parser syntax-level type nodes
- semantic compiler type representations
- type-checking and inference logic

Specifically:

- AST type syntax belongs in `compiler/frontend/ast/`
- semantic types belong in `compiler/frontend/types/`
- type checking belongs in `compiler/frontend/typecheck/`

## 13. Operator semantics

### 13.1 Equality operators (`==`, `!=`)

The equality operators have two dispatch paths:

1. **Builtin scalars** (all integer types, float types, bool): the
   compiler emits intrinsic comparison instructions directly (LLVM
   `icmp`/`fcmp`). This is the fast path with no function call
   overhead.
2. **Non-builtin types**: the compiler dispatches through the
   `Equatable` concept's runtime equality hook for the operand type.
   Currently this covers `string` (via `__dao_eq_string`). When
   struct equality is implemented, the same path will apply to any
   type conforming to `Equatable`.

`!=` is derived from `==` via logical negation. There is no separate
not-equal hook or concept method.

Both operands must have the same type. The result type is always
`bool`.

### 13.2 Arithmetic operators (`+`, `-`, `*`, `/`, `%`)

Arithmetic operators require numeric operands with the following
exception:

- **`+` on strings**: `string + string` dispatches to the runtime
  string concatenation hook (`__dao_str_concat`) and returns
  `string`. This is a special case, not a general operator
  overloading mechanism.

All other arithmetic operators (`-`, `*`, `/`, `%`) reject non-numeric
operands with a diagnostic.

Both operands must have the same type. The result type matches the
operand type.

### 13.3 Comparison operators (`<`, `<=`, `>`, `>=`)

Comparison operators currently require numeric operands. Extending
them to non-numeric types via `Comparable` is a future task.

### 13.4 General operator overloading

Dao does not currently support user-defined operator overloading.
The operator-to-concept dispatch for `==` and the string `+` special
case are the only non-builtin operator behaviors. A general operator
overloading mechanism is deferred and would require updating this
contract and `CONTRACT_SYNTAX_SURFACE.md`.

## 14. Non-goals of this contract

This contract does not freeze:

- generic syntax beyond the core `<T>` and `<T: Concept>` forms
  frozen in `CONTRACT_SYNTAX_SURFACE.md`
- conformance syntax beyond the core `as`, `extend`, `deny`, and
  `derived concept` forms frozen in `CONTRACT_SYNTAX_SURFACE.md`
- overload resolution semantics
- inference algorithm details
- pattern matching syntax
- exhaustiveness diagnostics policy
- effect typing
- higher-kinded types
- dependent typing

## 15. Implementation consequences

Compiler work may rely on the following architectural assumptions:

- semantic types are canonical compiler-owned objects
- semantic types are distinct from AST syntax
- semantic type identity is stable enough for later HIR typing and
  tooling
- `types/` must not depend on `typecheck/`

## 16. Stability rule

Any change that would alter Dao from:

- nominal + algebraic + parametric

into:

- purely structural
- purely HM-style
- or trait-surface-dominated

requires explicit reconsideration of this contract.
