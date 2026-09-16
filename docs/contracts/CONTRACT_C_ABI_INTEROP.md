# CONTRACT_C_ABI_INTEROP

Status: normative
Scope: Dao foreign function interop via the C ABI
Authority: language and compiler boundary contract

## 1. Purpose

This contract defines the boundary between Dao programs and external
code via the platform C ABI. It specifies what `extern fn` means
semantically, which types are supported, what the compiler guarantees
at the boundary, and what is explicitly out of scope.

## 2. Core doctrine

C ABI interop is an **explicit foreign boundary**, not a general-
purpose extension mechanism. It complements, but does not redefine,
Dao's runtime, stdlib, or semantic model.

Rules:

- `extern fn` declares a function whose calling convention and data
  representation are governed by the target platform C ABI
- Dao semantics do not automatically project onto foreign code
- crossing the boundary is explicit and narrow
- C ABI interop is a capability, not a substitute for designing
  Dao's own runtime and stdlib properly

## 3. What `extern fn` means

An `extern fn` declaration states:

- this function exists in external object code
- its symbol name is exactly as written
- its calling convention is the platform C ABI
- the compiler emits an LLVM `declare` with the appropriate
  signature and linkage
- the compiler does not generate a body, verify the implementation,
  or enforce Dao semantics on the foreign side

The function must be provided at link time via an object file,
static archive, or shared/system library.

## 4. Supported types at the ABI boundary

### 4.1 Supported scalar types

| Dao type  | C ABI type       | Direction       |
|-----------|------------------|-----------------|
| `i8`      | `int8_t`         | arg + return    |
| `i16`     | `int16_t`        | arg + return    |
| `i32`     | `int32_t`        | arg + return    |
| `i64`     | `int64_t`        | arg + return    |
| `u8`      | `uint8_t`        | arg + return    |
| `u16`     | `uint16_t`       | arg + return    |
| `u32`     | `uint32_t`       | arg + return    |
| `u64`     | `uint64_t`       | arg + return    |
| `f32`     | `float`          | arg + return    |
| `f64`     | `double`         | arg + return    |
| `bool`    | `_Bool` (1 byte) | arg + return    |
| `Ptr<T>`  | `T*`             | arg + return    |
| `void`    | `void`           | return only     |

### 4.2 Pointer semantics at the boundary

- pointer values are foreign addresses — no ownership implied
- no aliasing or lifetime safety is promised across FFI
- reading, writing, or offsetting a foreign pointer (`get`, `set`,
  `offset`) requires `mode unsafe =>`
- opaque pointers (`Ptr<void>`, C's `void*`) are the primary interop
  mechanism for foreign aggregate data

### 4.3 Struct-by-value at the boundary

Dao structs may be passed by value across the C ABI boundary as both
parameters and return values, provided they are **repr-C-compatible**.

#### 4.3.1 Repr-C-compatible struct predicate

A Dao struct is repr-C-compatible if and only if all of the following
hold:

1. the struct has at least one field (empty structs are rejected)
2. every field type is itself C-ABI-compatible (scalar, pointer, or
   a recursively repr-C-compatible struct)
3. the struct is not self-referential or mutually recursive through
   non-pointer fields (pointer-to-self is allowed, since pointers
   are C-ABI-compatible independently)

The compiler must reject extern fn signatures that use structs
failing this predicate with a clear diagnostic.

#### 4.3.2 Layout rules

Repr-C-compatible Dao structs follow these layout rules at the ABI
boundary:

- declared field order is preserved (no field reordering)
- no packed layout
- no explicit alignment overrides
- alignment follows the target's natural alignment for each field
  type

The current implementation relies on the target LLVM DataLayout and
non-packed LLVM struct types to realize this layout. This matches
C struct layout for the supported native targets under the default
alignment rules.

#### 4.3.3 Target scope

Struct-by-value C ABI interop is supported on hosted native targets
covered by the current LLVM backend and CI/test matrix. Additional
targets may require further validation or target-specific ABI
lowering.

#### 4.3.4 Nested structs

Nested structs are allowed if every level satisfies the repr-C-
compatible predicate recursively. A struct containing another struct
is valid at the boundary if and only if the inner struct is also
repr-C-compatible.

### 4.4 Function pointer types at the boundary

Dao function types may appear as parameters and return values in
`extern fn` declarations, representing C function pointers.

#### 4.4.1 Syntax

Function types are written as `fn(T, U, ...): R` in type positions,
mirroring function declaration syntax. Parameter names are omitted —
only types are listed.

#### 4.4.2 ABI-compatible function type predicate

A Dao function type is C-ABI-compatible if and only if:

1. every parameter type is itself C-ABI-compatible (scalar, pointer,
   repr-C-compatible struct, or recursively ABI-compatible function
   type)
2. the return type is C-ABI-compatible or void

The compiler must reject extern fn signatures that use function types
failing this predicate with a clear diagnostic.

#### 4.4.3 Representation

Function types at the C ABI boundary are lowered to opaque pointers
(`ptr` in LLVM IR). This matches the C calling convention where
function pointer parameters are pointer-sized values.

#### 4.4.4 Passing functions as callbacks

A named Dao function may be passed as an argument where a function
type is expected in an extern fn call. The compiler emits the
function's address as the argument value.

#### 4.4.5 Restrictions

- **No closures**: only named function references may be passed as
  function-pointer arguments to extern fn calls. Lambda expressions
  in function-pointer argument positions of extern fn calls must be
  rejected by the type checker with a clear diagnostic. Closures
  cannot be represented as C function pointers.
- **Indirect calls (Dao-to-Dao)**: calling through a function-typed
  value is supported for Dao-to-Dao higher-order functions. The
  indirect call path applies the same parameter adjustments as
  direct calls (string → pointer, function type → pointer).
- **Indirect calls (C-origin)**: calling through a function pointer
  received from C works for scalar-only signatures. C-origin
  function pointers with struct-by-value parameters or returns
  would require C ABI coercion in the indirect call path, which
  is not yet implemented.
- **Calling convention**: function pointers follow the platform C
  calling convention. No other conventions are supported.

### 4.5 Types explicitly not supported

- Dao `string` as C `char*` (requires explicit conversion)
- variadic functions (`printf`-style)
- C enums
- C unions
- arrays / slices by value
- capturing lambdas / closures at the C boundary

If a user-declared `extern fn` uses an unsupported type, the compiler
must reject it with a clear diagnostic during type checking or
backend lowering.

**Exception**: functions with reserved-prefix names (`__`) are exempt
from these restrictions. This covers `__dao_*` runtime hooks, which
use Dao-defined types (e.g. `string`) with Dao-defined ABI
conventions specified in `CONTRACT_RUNTIME_ABI.md`. The `__` prefix
aligns with C's reserved identifier convention; user code should not
declare `__`-prefixed names.

### 4.6 Future type expansions

The following may be added in later versions:

- `c_string` or explicit null-terminated byte string type
- packed structs / explicit alignment control
- indirect calls through function-typed values (C → Dao callbacks
  returning function pointers)

Each expansion requires updating this contract before implementation.

## 5. Name and linkage model

- the symbol name in `extern fn foo(...)` is `foo` — no mangling,
  no prefix
- Dao runtime hooks use the `__dao_` prefix to avoid collisions
  with user-declared foreign symbols
- C++ mangled names are not supported; use `extern "C"` on the
  C++ side
- weak linkage, visibility attributes, and linker scripts are
  not part of the initial model

## 6. Ownership and safety rules

- no ownership transfer is implied by `extern fn` calls
- the caller (Dao side) retains ownership of any data it passes
  unless the foreign function's documentation states otherwise
- the compiler does not insert cleanup, reference counting, or
  lifetime tracking for values passed to or received from foreign
  code
- memory access through a foreign pointer (`get`, `set`, `offset`) is
  an unsafe operation
- integer overflow checking is **not** applied to values received
  from foreign code (they enter Dao's value space as-is)

## 7. Compiler obligations

### 7.1 Frontend

- `extern fn` declarations are parsed and type-checked like other
  function declarations
- the body must be absent (syntax error if present)
- parameter and return types must be in the supported set
- unsupported types must be rejected with a diagnostic

### 7.2 Backend

- `extern fn` is lowered to an LLVM `declare` with
  `ExternalLinkage`
- calling convention follows the platform C ABI (LLVM default)
- argument and return types are lowered per the existing type
  lowering rules
- no body is emitted

### 7.3 Driver

- the driver must support linking additional external inputs:
  - object files (`.o`)
  - static archives (`.a`)
  - system/shared libraries (`-l<name>`)
  - library search paths (`-L<path>`)
- the mechanism for specifying external link inputs is a driver
  concern, not a language concern
- the initial implementation may use simple CLI passthrough to the
  system linker

## 8. What is explicitly out of scope

- header parsing or `#include` integration
- automatic bindgen / declaration generation
- C preprocessor awareness
- C type introspection at compile time
- C++ name mangling or template instantiation
- dynamic loading (`dlopen`-style)
- platform-specific calling conventions beyond the default C ABI
- inline assembly
- ABI stability guarantees for Dao-defined symbols exposed to C
  (reserved for a future "export" contract)

## 9. Relationship to existing contracts

- `CONTRACT_BOOTSTRAP_AND_INTEROP.md` establishes that the C ABI
  is the initial interop target; this contract operationalizes that
- `CONTRACT_RUNTIME_ABI.md` defines Dao's own runtime hooks, which
  are already `extern fn` declarations consumed through this same
  mechanism
- `CONTRACT_SYNTAX_SURFACE.md` freezes `extern fn` as the
  declaration form for externally-provided functions
- `CONTRACT_NUMERIC_SEMANTICS.md` defines the type mappings that
  apply at the ABI boundary

## 10. Implementation status

| Feature                          | Status          |
|----------------------------------|-----------------|
| `extern fn` syntax + parsing     | Implemented     |
| `extern fn` LLVM lowering        | Implemented     |
| Scalar types at boundary         | Implemented     |
| Pointer types at boundary        | Implemented     |
| Struct-by-value arguments        | Implemented     |
| Struct-by-value returns          | Implemented     |
| Repr-C-compatible predicate      | Implemented     |
| Driver: link `.o` files          | Implemented     |
| Driver: link `-l` libraries      | Implemented     |
| Driver: `-L` search paths        | Implemented     |
| Unsupported type rejection       | Implemented     |
| E2E foreign call example         | Implemented     |
| E2E struct-by-value example      | Implemented     |
| Function pointer type syntax     | Implemented     |
| Function pointer params          | Implemented     |
| Function pointer return type     | Implemented     |
| Indirect call (Dao-to-Dao)       | Implemented     |
| Indirect call (C-origin struct)  | Not implemented |
| Named function as callback       | Implemented     |
| Lambda rejection at ABI boundary | Implemented     |
| E2E callback example             | Implemented     |
