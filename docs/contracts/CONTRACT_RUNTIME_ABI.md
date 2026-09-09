# CONTRACT_RUNTIME_ABI.md

## Purpose

Defines the Dao compiler/runtime ABI: the explicit boundary between
compiler-generated code and runtime support functions required for
program execution.

This contract is normative. It is the authoritative specification for
runtime-facing value shapes, hook naming, calling conventions, and
ownership rules.

## Scope

This contract covers the **hosted-native** execution path only. It
defines the minimum runtime surface required for compiler-generated
binaries to execute using the Dao prelude (`stdlib/core/`) without
user-written host-language glue.

Out of scope:

- parallel/GPU execution runtime
- scheduler/executor architecture
- allocator design
- filesystem/network/IO beyond stdout

## Strategic posture

Dao owns the semantics. The runtime ABI is Dao-defined.

The current runtime implementation language is C. This is a bootstrap
decision, not a permanent architectural commitment. The native
substrate must remain narrow enough that:

- the implementation language can change (C, Rust, Zig, C++, etc.)
- the foreign substrate shrinks over time as Dao matures
- runtime policy and stdlib logic move upward into Dao itself
- eventual self-hosting requires only a thin irreducible native layer

Nothing in this contract cements C as Dao's permanent runtime language.

## Runtime hook naming policy

All Dao runtime hooks use the prefix `__dao_` followed by a
domain-qualified name. This prefix is reserved for compiler/runtime
use and must not appear in user code.

Naming pattern: `__dao_<domain>_<operation>`

Domains:

| Domain     | Scope                                      |
|------------|--------------------------------------------|
| `io`       | Output, input (currently stdout only)      |
| `eq`       | Equality comparison                        |
| `conv`     | Value-to-value conversion (e.g. to_string) |
| `gen`      | Generator frame allocation and lifetime    |
| `mem`      | Resource domain scope and lifetime         |
| `str`      | String operations                          |
| `wrapping` | Wrapping (two's complement) arithmetic     |
| `saturating`| Saturating arithmetic                     |

Examples:

| Hook                      | Signature (Dao surface)              |
|---------------------------|--------------------------------------|
| `__dao_io_write_stdout`   | `(msg: string): void`                |
| `__dao_io_write_stderr`   | `(msg: string): void`                |
| `__dao_io_read_file`      | `(path: string): string`             |
| `__dao_io_write_file`     | `(path: string, content: string): bool` |
| `__dao_io_file_exists`    | `(path: string): bool`               |
| `__dao_eq_i8`             | `(a: i8, b: i8): bool`              |
| `__dao_eq_i16`            | `(a: i16, b: i16): bool`            |
| `__dao_eq_i32`            | `(a: i32, b: i32): bool`            |
| `__dao_eq_i64`            | `(a: i64, b: i64): bool`            |
| `__dao_eq_u8`             | `(a: u8, b: u8): bool`              |
| `__dao_eq_u16`            | `(a: u16, b: u16): bool`            |
| `__dao_eq_u32`            | `(a: u32, b: u32): bool`            |
| `__dao_eq_u64`            | `(a: u64, b: u64): bool`            |
| `__dao_eq_f32`            | `(a: f32, b: f32): bool`            |
| `__dao_eq_f64`            | `(a: f64, b: f64): bool`            |
| `__dao_eq_bool`           | `(a: bool, b: bool): bool`          |
| `__dao_eq_string`         | `(a: string, b: string): bool`      |
| `__dao_conv_i8_to_string` | `(x: i8): string`                   |
| `__dao_conv_i16_to_string`| `(x: i16): string`                   |
| `__dao_conv_i32_to_string`| `(x: i32): string`                   |
| `__dao_conv_i64_to_string`| `(x: i64): string`                   |
| `__dao_conv_u8_to_string` | `(x: u8): string`                   |
| `__dao_conv_u16_to_string`| `(x: u16): string`                   |
| `__dao_conv_u32_to_string`| `(x: u32): string`                   |
| `__dao_conv_u64_to_string`| `(x: u64): string`                   |
| `__dao_conv_f32_to_string`| `(x: f32): string`                   |
| `__dao_conv_f64_to_string`| `(x: f64): string`                   |
| `__dao_conv_bool_to_string`| `(x: bool): string`                 |
| `__dao_gen_alloc`         | `(size: i64, align: i64): *void`     |
| `__dao_gen_free`          | `(ptr: *void): void`                 |
| `__dao_mem_resource_enter`| `(): *void`                           |
| `__dao_mem_resource_exit` | `(domain: *void): void`              |
| `__dao_mem_alloc`         | `(size: i64, align: i64): *void`     |
| `__dao_mem_realloc`       | `(ptr: *void, old_size: i64, new_size: i64, align: i64): *void` |
| `__dao_mem_free`          | `(ptr: *void): void`                 |
| `__dao_mem_alloc_outer`   | `(size: i64, align: i64): *void`     |
| `__dao_str_from_bytes`    | `(bytes: *u8, len: i64): string`     |
| `__dao_str_copy_outer`    | `(s: string): string`                |
| `__dao_conv_i32_to_f64`  | `(x: i32): f64`                       |
| `__dao_conv_i32_to_i64`  | `(x: i32): i64`                       |
| `__dao_conv_f64_to_i32`  | `(x: f64): i32`                       |
| `__dao_conv_i64_to_i32`  | `(x: i64): i32`                       |
| `__dao_conv_f32_to_f64`  | `(x: f32): f64`                       |
| `__dao_conv_f64_to_f32`  | `(x: f64): f32`                       |
| `__dao_conv_i32_to_f32`  | `(x: i32): f32`                       |
| `__dao_conv_i64_to_f64`  | `(x: i64): f64`                       |
| `__dao_conv_i64_to_f32`  | `(x: i64): f32`                       |
| `__dao_conv_f64_to_i64`  | `(x: f64): i64`                       |
| `__dao_conv_f32_to_i32`  | `(x: f32): i32`                       |
| `__dao_conv_f32_to_i64`  | `(x: f32): i64`                       |
| `__dao_conv_i8_to_i32`   | `(x: i8): i32`                        |
| `__dao_conv_i16_to_i32`  | `(x: i16): i32`                       |
| `__dao_conv_i8_to_i64`   | `(x: i8): i64`                        |
| `__dao_conv_i16_to_i64`  | `(x: i16): i64`                       |
| `__dao_conv_u8_to_u32`   | `(x: u8): u32`                        |
| `__dao_conv_u16_to_u32`  | `(x: u16): u32`                       |
| `__dao_conv_u8_to_u64`   | `(x: u8): u64`                        |
| `__dao_conv_u16_to_u64`  | `(x: u16): u64`                       |
| `__dao_conv_u32_to_u64`  | `(x: u32): u64`                       |
| `__dao_conv_u32_to_i64`  | `(x: u32): i64`                       |
| `__dao_conv_i32_to_i8`   | `(x: i32): i8`                        |
| `__dao_conv_i32_to_i16`  | `(x: i32): i16`                       |
| `__dao_conv_u32_to_u8`   | `(x: u32): u8`                        |
| `__dao_conv_u32_to_u16`  | `(x: u32): u16`                       |
| `__dao_conv_i32_to_u32`  | `(x: i32): u32`                       |
| `__dao_conv_u32_to_i32`  | `(x: u32): i32`                       |
| `__dao_conv_i64_to_u64`  | `(x: i64): u64`                       |
| `__dao_conv_u64_to_i64`  | `(x: u64): i64`                       |
| `__dao_str_concat`       | `(a: string, b: string): string`      |
| `__dao_str_length`       | `(s: string): i64`                    |
| `__dao_str_char_at`      | `(s: string, index: i64): i32`        |
| `__dao_str_substring`    | `(s: string, start: i64, len: i64): string` |
| `__dao_str_index_of`     | `(s: string, needle: string): i64`    |
| `__dao_str_starts_with`  | `(s: string, prefix: string): bool`   |
| `__dao_str_ends_with`    | `(s: string, suffix: string): bool`   |
| `__dao_str_compare`      | `(a: string, b: string): i32`         |
| `__dao_str_hash`         | `(s: string): i64`                    |
| `__dao_wrapping_add_i8`  | `(a: i8, b: i8): i8`                 |
| `__dao_wrapping_sub_i8`  | `(a: i8, b: i8): i8`                 |
| `__dao_wrapping_mul_i8`  | `(a: i8, b: i8): i8`                 |
| `__dao_wrapping_add_i16` | `(a: i16, b: i16): i16`              |
| `__dao_wrapping_sub_i16` | `(a: i16, b: i16): i16`              |
| `__dao_wrapping_mul_i16` | `(a: i16, b: i16): i16`              |
| `__dao_wrapping_add_i32` | `(a: i32, b: i32): i32`              |
| `__dao_wrapping_sub_i32` | `(a: i32, b: i32): i32`              |
| `__dao_wrapping_mul_i32` | `(a: i32, b: i32): i32`              |
| `__dao_wrapping_add_i64` | `(a: i64, b: i64): i64`              |
| `__dao_wrapping_sub_i64` | `(a: i64, b: i64): i64`              |
| `__dao_wrapping_mul_i64` | `(a: i64, b: i64): i64`              |
| `__dao_saturating_add_i8` | `(a: i8, b: i8): i8`                |
| `__dao_saturating_sub_i8` | `(a: i8, b: i8): i8`                |
| `__dao_saturating_mul_i8` | `(a: i8, b: i8): i8`                |
| `__dao_saturating_add_i16`| `(a: i16, b: i16): i16`             |
| `__dao_saturating_sub_i16`| `(a: i16, b: i16): i16`             |
| `__dao_saturating_mul_i16`| `(a: i16, b: i16): i16`             |
| `__dao_saturating_add_i32`| `(a: i32, b: i32): i32`             |
| `__dao_saturating_sub_i32`| `(a: i32, b: i32): i32`             |
| `__dao_saturating_mul_i32`| `(a: i32, b: i32): i32`             |
| `__dao_saturating_add_i64`| `(a: i64, b: i64): i64`             |
| `__dao_saturating_sub_i64`| `(a: i64, b: i64): i64`             |
| `__dao_saturating_mul_i64`| `(a: i64, b: i64): i64`             |

These are the **only** runtime hooks in the current supported slice.
New hooks require updating this contract before implementation.

## Canonical value representations

### Scalar types

| Dao type | LLVM IR type | C type     | Passing convention |
|----------|-------------|------------|--------------------|
| `i32`    | `i32`       | `int32_t`  | by value           |
| `f64`    | `double`    | `double`   | by value           |
| `bool`   | `i1`        | `bool`     | by value           |
| `void`   | `void`      | `void`     | N/A                |

### String type

The Dao string is represented at the ABI boundary as a struct:

```
%dao.string = type { ptr, i64 }
```

C-equivalent:

```c
struct dao_string {
    const char *ptr;
    int64_t     len;
};
```

Properties:

- `ptr` points to a contiguous byte sequence of `len` bytes
- not required to be null-terminated at the ABI boundary
- `len` is the byte count, not a character or codepoint count
- empty strings are valid (`ptr` may be null when `len` is 0)
- string literals may point to static (read-only) storage

### String passing convention

- **Parameters**: string arguments are passed **by pointer** to the
  struct (`const struct dao_string *`). The compiler emits a temporary
  alloca and passes its address.
- **Return values**: string-returning hooks return the struct **by
  value** (`struct dao_string`). The caller receives a copy.

### Generator type representation

`Generator<T>` is represented at the ABI boundary as a fat pair:

```
%dao.generator = type { ptr, ptr }
```

C-equivalent:

```c
struct dao_generator {
    void *frame;
    void (*resume)(void *frame);
};
```

Properties:

- `frame` points to a compiler-generated frame struct allocated by
  `__dao_gen_alloc`. The frame layout is private to the backend and
  may vary per generator function.
- `resume` is a pointer to the generator's resume function, which
  advances the generator to its next yield point when called with
  the frame pointer.
- Consumer code accesses the generator exclusively through the
  `__dao_gen_*` hooks and the resume function pointer.

### Resource domain handle

Resource domains are represented at the ABI boundary as an opaque
pointer:

```
ptr  ; opaque domain handle
```

C-equivalent:

```c
void *domain;
```

Properties:

- the handle is opaque to compiler-generated code; its internal
  structure is private to the runtime
- `__dao_mem_resource_enter` returns a fresh handle; the compiler
  passes the same handle to the corresponding `__dao_mem_resource_exit`
- handles are scope-paired: every enter has exactly one exit on every
  control-flow path — fall-through, `break`, and `return`; a `break`
  exits only the domains entered inside the loop it leaves
- nesting is supported: exiting a handle also closes every domain
  still open inside it

Allocation semantics (Task 35):

- a domain is an **arena**.  Between its enter and its exit it is the
  **current domain**: every allocation made through the memory hooks
  (`__dao_mem_alloc`, `__dao_mem_realloc`, and every string-, frame-,
  and file-producing hook, which allocate through them) lands in it
- exit **reclaims the domain wholesale**: no per-object frees, no
  destructors, no finalization order
- `__dao_mem_free` on domain memory is a no-op; `__dao_mem_realloc`
  inside a domain copies into the domain and abandons the old block
  (to the domain if it was the domain's, to the system if it was the
  root's)
- outside every block the **root domain** is current: the system
  allocator, process lifetime, `__dao_mem_free` frees — unchanged
- `__dao_mem_alloc_outer` allocates in the parent of the current
  domain (the root, when the current block is the outermost), for
  values that must outlive the block; which domain that is only the
  runtime's domain stack knows
- a value allocated in a domain is invalid after the domain's exit.
  The compiler copies what leaves a block into the enclosing domain
  at the exit (`CONTRACT_EXECUTION_CONTEXTS.md`, law 7): a string
  through `__dao_str_copy_outer`, which copies its bytes into the
  parent of the current domain, and an aggregate through its own
  `copy_out` method or field by field, allocating through
  `__dao_mem_alloc_outer`.  A generator may not leave its block, and
  a block may not contain `yield`, since a domain cannot stay current
  across a suspension.

## Ownership and lifetime rules

For the current supported hook slice:

1. **No ownership transfer.** Runtime hooks that receive strings
   (printing, equality) borrow the data for the duration of the call.
   The caller retains ownership.

2. **Literals are static.** String literals emitted by the compiler
   reside in static storage and are valid for the lifetime of the
   process.

3. **Conversion results are owned by the current domain.**
   Scalar-to-string conversion hooks (`__dao_conv_*_to_string`)
   return a `dao_string` whose `ptr` points to a buffer allocated
   through `__dao_mem_alloc`: in the root domain, a heap buffer with
   process lifetime; inside a `resource memory` block, the block's
   memory, reclaimed at its exit.  Callers may store the result in
   long-lived data structures (e.g. as a HashMap key) without copying
   — see rule 6 below.

4. **Generator frames belong to the domain current at creation.**
   Frames are allocated by `__dao_gen_alloc` (through the memory
   hooks) and released by `__dao_gen_free` when the iterator is no
   longer needed; the compiler inserts that call at for-loop exit.
   In the root domain the release frees; inside a block it is a
   no-op, the frame going with the block.

5. **Resource domain handles are scope-paired.** Every handle
   returned by `__dao_mem_resource_enter` must be passed to exactly
   one `__dao_mem_resource_exit` call. The compiler inserts exit
   calls on both normal and early-return paths.

6. **String-producing runtime hooks return buffers owned by the
   current domain.**  `__dao_str_concat`, `__dao_str_substring`,
   `__dao_str_from_bytes`, `__dao_io_read_file`, and all
   `__dao_conv_*_to_string` hooks return a `dao_string` whose `ptr`
   points to a fresh buffer allocated through `__dao_mem_alloc` —
   never to a static or thread-local buffer, which an earlier
   runtime used and which silently corrupted any data structure that
   stored the result (observable via `HashMap<V>.set(i64_to_string(i),
   v)` in a loop, where keys collided because every returned string
   pointed at the same buffer).  In the root domain such a buffer has
   process lifetime; inside a `resource memory` block it is the
   block's, reclaimed at its exit.

## Stability

### Stable (frozen for current slice)

- `__dao_` naming prefix and domain-qualified pattern
- `dao_string` struct layout (`{ ptr, i64 }`)
- scalar type mappings (i8, i16, i32, i64, u8, u16, u32, u64,
  f32, f64, bool, void)
- string passing convention (by-pointer in, by-value out)
- all hooks listed in the table above

### Provisional (may evolve)

- additional string manipulation hooks (beyond concat)
- the internal layout of a domain (chunk sizes, growth); the hooks
  and their semantics above are stable
- mode runtime hooks (parallel, GPU)

## Authoritative sources

The single authoritative home for runtime hook names and signatures
is the contract table above.

Implementation must agree:

| Layer                  | Must match contract |
|------------------------|---------------------|
| `stdlib/core/*.dao`    | extern declarations |
| `runtime/core/`        | C implementations   |
| LLVM backend hook layer| LLVM declarations   |
| backend tests          | ABI assertions      |

Disagreement between any layer and this contract is a bug.

## Self-hosting posture

This ABI contract describes what the compiler expects from its
runtime support layer. It does not prescribe the implementation
language.

Future migration path:

- **Near term**: Dao-defined ABI, C reference implementation
- **Mid term**: same ABI, implementation language may change
- **Long term**: Dao-hosted runtime/stdlib with only a thin
  irreducible native substrate (e.g. syscall wrappers)

The native substrate should shrink over time as Dao gains the
capability to implement more of its own runtime.
