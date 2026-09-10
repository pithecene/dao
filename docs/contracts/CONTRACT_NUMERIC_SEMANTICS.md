# CONTRACT_NUMERIC_SEMANTICS

Status: normative
Scope: Dao numeric type semantics, conversion rules, and backend obligations
Authority: language semantics contract

## 1. Purpose

This contract defines Dao's numeric semantics: the language-level
behavioral guarantees for integer and floating-point types, their
interactions, and the obligations imposed on compiler backends,
runtime, and stdlib implementations.

This contract specifies the intended semantics first and the
implementation rollout second. Sections marked **Deferred** are
normative for future implementation but non-operative in the current
compiler unless explicitly enabled.

## 2. Core doctrine

Dao's default numeric semantics prioritize predictability,
portability, and explicitness over implicit coercion and silent
optimization-driven weakening.

- default compilation assumes strict floating-point semantics
- optimizations must not silently change observable NaN, infinity,
  or signed-zero behavior
- any future fast/relaxed numeric mode must be opt-in and lexically
  visible
- relaxed or target-specific numeric behavior must not redefine the
  default numeric model implicitly

## 3. Numeric types in Dao

### 3.1 Integer types

| Type  | Width | Signed | Status              |
|-------|-------|--------|---------------------|
| `i8`  | 8     | yes    | Implemented         |
| `i16` | 16    | yes    | Implemented         |
| `i32` | 32    | yes    | Implemented         |
| `i64` | 64    | yes    | Implemented         |
| `u8`  | 8     | no     | Implemented         |
| `u16` | 16    | no     | Implemented         |
| `u32` | 32    | no     | Implemented         |
| `u64` | 64    | no     | Implemented         |

All integer types are fixed-width, two's-complement for signed types,
with no padding bits.

### 3.2 Floating-point types

| Type  | IEEE 754    | Status              |
|-------|-------------|---------------------|
| `f64` | binary64    | Implemented         |
| `f32` | binary32    | Implemented         |

Both types are part of the language design and are implemented. `f32`
shares the same IEEE 754 conformance obligations as `f64`. GPU-specific
numeric profiles for `f32` are deferred to Phase 8.

### 3.3 Decimal types

Dao does not currently define a built-in decimal floating type.

Dao intends to support decimal/exact-base-10 numerics as a distinct
semantic family, not as aliases of binary floats. Decimal operations
must not silently lower to binary float semantics.

Later decimal introduction must choose and standardize one of:

- fixed-point decimal
- IEEE 754 decimal floating-point (decimal64/decimal128)
- arbitrary-precision decimal

Until such a choice is made, decimal support is specified as deferred
design space, not implementation-defined ad hoc behavior.

If Dao adds a built-in decimal type, it must be specified as a distinct
decimal semantic domain with exact decimal-rounding rules, rather than
a thin formatting veneer over binary floating-point.

Status: **Deferred**

## 4. Integer semantics

### 4.1 Arithmetic

Integer arithmetic follows the mathematical result where the result
is representable in the type's range.

### 4.2 Overflow

Signed integer overflow is **checked by default**. Overflow in the
default arithmetic operators (`+`, `-`, `*`) causes a **trap**
(runtime abort). The compiler must not treat signed integer overflow
as undefined behavior and must not silently lower default signed
arithmetic to wrapping semantics.

Rules:

1. default `+`, `-`, `*` on signed integers are checked — overflow
   traps
2. the trap is a defined runtime abort, not undefined behavior
3. the compiler may not exploit overflow for optimization
4. `mode unsafe =>` does **not** change the semantics of integer
   arithmetic operators — `unsafe` governs pointer/memory/FFI
   safety, not arithmetic behavior
5. the operator means the same thing in every build mode unless
   the language explicitly says otherwise — no debug/release
   semantic split

Explicit overflow operations:

- `wrapping_add`, `wrapping_sub`, `wrapping_mul` — two's complement
  wrap, no trap — **implemented** for all signed types (i8–i64)
- `saturating_add`, `saturating_sub`, `saturating_mul` — clamp to
  min/max representable value — **implemented** for all signed types
  (i8–i64)
- `checked_add`, `checked_sub`, `checked_mul` — return
  `Option::None` on overflow, `Option::Some(value = result)` otherwise —
  **implemented** for all signed types (i8–i64); pure Dao
  implementations using widening (i8/i16/i32) or wrapping +
  overflow detection (i64)

If Dao later wants relaxed arithmetic for high-performance numerics,
it must be introduced as an explicit mode, operator family, or
intrinsic family — not by overloading `unsafe` or by making
semantics build-configuration-dependent.

Status: **Specified, implemented** — default signed arithmetic traps
on overflow for all signed types (i8–i64); explicit wrapping,
saturating, and checked operations available for all signed types

### 4.3 Division and remainder

Integer division truncates toward zero.

Remainder follows the truncation direction: `a % b` has the same
sign as `a`.

Division by zero is a runtime error (trap or defined panic), not
undefined behavior.

Status: **Specified, partially implemented**

## 5. Floating-point semantics

### 5.1 IEEE 754 conformance

Dao `f64` follows IEEE 754 binary64 as the language-level semantic
baseline. Dao `f32` follows IEEE 754 binary32.

Default Dao numeric semantics assume IEEE-conformant behavior for:

- arithmetic operations
- infinities
- NaNs
- signed zero
- comparisons
- conversions where defined by IEEE 754

Hosted native targets are required to provide semantics compatible
with IEEE 754 binary floating-point for `f64`, and eventually `f32`,
unless a target profile explicitly documents a restricted numeric
environment.

### 5.2 NaN behavior

- `NaN == x` is `false` for all `x`, including `NaN`
- `NaN != x` is `true` for all `x`, including `NaN`
- `<`, `<=`, `>`, `>=` involving `NaN` are `false`
- ordered comparisons follow IEEE partial-order behavior, not a
  total ordering
- NaN propagates through arithmetic operations according to IEEE 754
- quiet NaN is the default NaN produced by invalid operations
  (e.g. `0.0 / 0.0`)

### 5.3 Infinity behavior

- `+Inf` and `-Inf` propagate through arithmetic per IEEE 754
- `1.0 / +0.0` = `+Inf`
- `1.0 / -0.0` = `-Inf`
- `Inf - Inf` = `NaN`
- `0.0 * Inf` = `NaN`
- comparisons with infinities follow IEEE ordering

### 5.4 Signed zero

- `+0.0 == -0.0` is `true`
- the sign of zero is preserved by operations where IEEE 754
  preserves it
- negation: `-(+0.0)` = `-0.0`, `-(-0.0)` = `+0.0`

### 5.5 Equality and ordering for concepts

Primitive float equality uses IEEE equality semantics, not total-order
equality.

If total ordering, hashable wrappers, or approximate comparison are
later needed, those must be explicit library abstractions, not
redefinitions of the default `==` operator.

## 6. Numeric conversions and casts

### 6.1 General doctrine

Numeric conversions are explicit unless proven lossless by rule.
Mixed-type arithmetic is not silently coerced by broad promotion
rules in the initial language model.

### 6.2 Integer to integer

- lossless widening (e.g. `i8` → `i32`) may be implicit if Dao
  adopts that policy globally; this decision is deferred
- narrowing conversions are explicit and checked
- sign-changing conversions (signed ↔ unsigned) are explicit

Status: **Implemented** — widening, narrowing, and sign-changing
conversions available for all surfaced integer types (i8–i64,
u8–u64) via explicit conversion functions; narrowing and sign
conversions trap on out-of-range

### 6.3 Integer to float

- explicit if precision loss is possible
- semantics follow the nearest representable IEEE value under the
  default rounding environment
- `i32` → `f64` is exact (all i32 values are representable in f64)
- `i64` → `f64` may lose precision and requires explicit conversion

Status: **Implemented** — `i32_to_f32`, `i32_to_f64` (exact),
`i64_to_f64`, `i64_to_f32` (may lose precision)

### 6.4 Float to integer

- explicit
- truncates toward zero
- conversion from NaN is a runtime error (trap or defined panic)
- conversion from infinity is a runtime error
- conversion from a finite value outside the target integer range is
  a runtime error
- float-to-integer conversion must not be silent or undefined on
  out-of-range inputs

Status: **Implemented** — `f64_to_i32`, `f64_to_i64`, `f32_to_i32`,
`f32_to_i64` (all truncating, trapping on NaN/Inf/out-of-range)

### 6.5 f64 to f32 / f32 to f64

- `f32` → `f64`: exact, follows IEEE widening
- `f64` → `f32`: explicit, rounds to nearest according to the
  default rounding rule

Status: **Implemented** — `f32_to_f64` (exact widening),
`f64_to_f32` (narrowing, rounds to nearest)

### 6.6 Mixed-type binary operators

Operands must have the same concrete numeric type unless an explicit
conversion is written.

The initial language model does not include an implicit numeric
promotion lattice.

Status: **Implemented** — type checker requires same-type operands

## 7. Rounding-mode policy

### 7.1 Default rounding rule

Dao default floating semantics assume the platform's default IEEE
rounding mode: round-to-nearest, ties-to-even.

### 7.2 Dynamic rounding control

The core language does not expose dynamic rounding-mode mutation.

User code may not assume that runtime manipulation of the processor
floating rounding mode is reflected in Dao semantics unless and until
explicitly standardized.

### 7.3 Future extension

If Dao later exposes rounding control, it must do so explicitly and
in a way that constrains optimizer and backend assumptions. Ambient
mutable rounding-mode state must not infect default semantics.

Status: **Specified as unsupported in initial language**

## 8. Fast-math and relaxed numeric modes

### 8.1 Default

Dao default numeric semantics forbid optimization flags or backend
transforms that change observable IEEE behavior, including:

- reassociation of floating-point operations
- contraction that changes results (e.g. fused multiply-add where
  not semantically equivalent)
- assuming no NaNs
- assuming no infinities
- ignoring signed zero
- reciprocal approximations that are not semantically exact

### 8.2 Relaxed mode

Dao may later expose relaxed/fast numeric semantics as an explicit
mode, attribute, or target profile.

Such a mode must be:

- opt-in
- lexically or semantically visible at the use site
- unable to silently become the default interpretation of ordinary
  Dao numeric code

### 8.3 GPU interaction

Core language numeric semantics are defined independently of GPU
execution. Staged GPU targets may later introduce constrained or
relaxed numeric profiles, but those must be explicit target/mode
semantics, not retroactive weakening of core CPU semantics.

Status: **Specified as opt-in-only, not implemented**

## 9. String conversion of numeric values

`to_string` for numeric types is currently implementation-defined but
stable for debugging and output. The current implementation uses
`snprintf` formatting.

Future hardening may adopt a shortest-roundtrip decimal representation
target (e.g. Ryu or Dragonbox for floats), but this is not yet frozen.

String conversion of numeric values must preserve semantic distinctions
where intended:

- `NaN` must convert to a recognizable NaN representation
- infinities must convert to a recognizable infinity representation
- `-0.0` representation is implementation-defined (may display as
  `0` or `-0`)

Status: **Partially implemented** — `__dao_conv_f64_to_string` exists

## 10. Compiler and backend obligations

### 10.1 Must not

The compiler and backend must not:

- assume no NaNs unless a relaxed mode explicitly allows it
- fold `x == x` to `true` for floats
- erase signed-zero distinctions where observable
- reassociate floating expressions under default semantics
- treat signed integer overflow as undefined if Dao defines it
  otherwise
- silently lower decimal operations to binary-float operations if
  decimal types are later introduced

### 10.2 Must

The compiler and backend must:

- preserve default IEEE-observable behavior for ordinary floating code
- lower float comparisons according to partial-order semantics:
  ordered equality for `==` (false if either operand is NaN),
  unordered not-equal for `!=` (true if either operand is NaN),
  ordered comparisons for relational ops `<`/`<=`/`>`/`>=`
  (false if either operand is NaN)
- ensure explicit conversions trap or error where the language
  specifies
- distinguish semantic defaults from future relaxed modes

## 11. Runtime and stdlib obligations

- helper APIs (`abs`, `min`, `max`, approximate-equality,
  total-ordering) must not redefine primitive operator semantics
- if approximate or total-order float utilities are added, they must
  be clearly named and distinct from default `==` / `<`
- conversion failures must follow the contract, not host-language
  accident
- decimal, if later implemented, must use its specified decimal
  semantics rather than binary-float approximation masquerading as
  decimal

## 12. Target-specific obligations

- hosted native targets must document any numeric restrictions
- GPU/staged targets may define alternate numeric profiles only
  explicitly, never by silently weakening the default contract
- any target that cannot provide IEEE 754 conformance must declare
  a restricted numeric profile

## 13. Implementation status matrix

| Feature                          | Specified | Implemented       |
|----------------------------------|-----------|-------------------|
| `i32` arithmetic                 | Yes       | Yes               |
| `i32` checked overflow (trap)    | Yes       | Yes               |
| `f64` IEEE 754 binary64          | Yes       | Implemented       |
| `f64` NaN/Inf/−0.0 semantics    | Yes       | Implemented       |
| `f64` comparison partial-order   | Yes       | Implemented       |
| `f32` IEEE 754 binary32          | Yes       | Implemented       |
| `i64` arithmetic + overflow      | Yes       | Yes               |
| Integer widths (i8, i16)         | Yes       | Implemented       |
| Unsigned integers (u8–u64)       | Yes       | Implemented       |
| Numeric conversions (full matrix) | Yes       | Implemented       |
| Float-to-int trapping (all)      | Yes       | Implemented       |
| Decimal types                    | Posture   | Deferred          |
| Rounding-mode control            | Forbidden | N/A               |
| Fast-math / relaxed mode         | Opt-in    | Deferred          |
| Integer overflow policy freeze   | Yes       | Implemented       |
| Wrapping operations (i32, i64)   | Yes       | Implemented       |
| Saturating operations (i32, i64) | Yes       | Implemented       |
| Checked operations               | Yes       | Deferred          |
| String conversion (f64)          | Partial   | Implemented       |
| Mixed-type operator rejection    | Yes       | Implemented       |

## 14. Open future extensions

The following are explicitly reserved for future specification:

- `isize` / `usize` pointer-sized integer types
- SIMD / vector numeric types
- complex number types
- fixed-point types
- numeric literal polymorphism (e.g. `0` usable as `i32` or `f64`
  by context)
- associated constants for numeric concepts (e.g. `T.zero()`,
  `T.max_value()`)
- overflow-mode selection (checked, wrapping, saturating) as
  language-level controls
