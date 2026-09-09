// dao_abi.h — Canonical Dao runtime ABI declarations.
//
// This header is the single authoritative source for runtime-facing
// type definitions and hook signatures. All runtime implementation
// files include this header. The LLVM backend maintains a parallel
// authoritative declaration layer; both must agree with
// docs/contracts/CONTRACT_RUNTIME_ABI.md.
//
// This file defines the C-side ABI surface. Dao owns the semantics;
// C is the initial implementation vehicle only.

#ifndef DAO_RUNTIME_ABI_H
#define DAO_RUNTIME_ABI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Canonical value representations
// ---------------------------------------------------------------------------

// Dao string: pointer to byte data + byte length.
// Matches LLVM IR: %dao.string = type { ptr, i64 }
//
// - Not required to be null-terminated.
// - Literals may point to static (read-only) storage.
// - Empty strings are valid (ptr may be null when len is 0).
struct dao_string {
  const char *ptr;
  int64_t len;
};

// ---------------------------------------------------------------------------
// Runtime hook declarations — IO domain
// ---------------------------------------------------------------------------

// Write a string to stdout followed by a newline.
// Borrows msg for the duration of the call; no ownership transfer.
void __dao_io_write_stdout(const struct dao_string *msg);

// Write a string to stderr followed by a newline.
void __dao_io_write_stderr(const struct dao_string *msg);

// Read an entire file into a string owned by the current domain.
// Traps on error.
struct dao_string __dao_io_read_file(const struct dao_string *path);

// Write a string to a file. Returns true on success, false on failure.
bool __dao_io_write_file(const struct dao_string *path,
                          const struct dao_string *content);

// Check if a file exists.
bool __dao_io_file_exists(const struct dao_string *path);

// ---------------------------------------------------------------------------
// Runtime hook declarations — Equality domain
// ---------------------------------------------------------------------------

bool __dao_eq_i8(int8_t a, int8_t b);
bool __dao_eq_i16(int16_t a, int16_t b);
bool __dao_eq_i32(int32_t a, int32_t b);
bool __dao_eq_i64(int64_t a, int64_t b);
bool __dao_eq_u8(uint8_t a, uint8_t b);
bool __dao_eq_u16(uint16_t a, uint16_t b);
bool __dao_eq_u32(uint32_t a, uint32_t b);
bool __dao_eq_u64(uint64_t a, uint64_t b);
bool __dao_eq_f32(float a, float b);
bool __dao_eq_f64(double a, double b);
bool __dao_eq_bool(bool a, bool b);
bool __dao_eq_string(const struct dao_string *a, const struct dao_string *b);

// ---------------------------------------------------------------------------
// Runtime hook declarations — Conversion domain (to_string)
// ---------------------------------------------------------------------------

// Scalar-to-string conversions return a dao_string by value.
// The returned ptr points to a fresh buffer allocated through
// __dao_mem_alloc -- owned by the current domain, like every other
// string-producing hook; successive calls return distinct buffers, so
// the result may be stored in long-lived data structures (e.g. HashMap
// keys) without copying.
struct dao_string __dao_conv_i8_to_string(int8_t x);
struct dao_string __dao_conv_i16_to_string(int16_t x);
struct dao_string __dao_conv_i32_to_string(int32_t x);
struct dao_string __dao_conv_i64_to_string(int64_t x);
struct dao_string __dao_conv_u8_to_string(uint8_t x);
struct dao_string __dao_conv_u16_to_string(uint16_t x);
struct dao_string __dao_conv_u32_to_string(uint32_t x);
struct dao_string __dao_conv_u64_to_string(uint64_t x);
struct dao_string __dao_conv_f32_to_string(float x);
struct dao_string __dao_conv_f64_to_string(double x);
struct dao_string __dao_conv_bool_to_string(bool x);

// Numeric type conversions.
// Lossless conversions are exact. Narrowing conversions trap (abort)
// on NaN, Inf, or out-of-range values.
double __dao_conv_i32_to_f64(int32_t x);
int64_t __dao_conv_i32_to_i64(int32_t x);
int32_t __dao_conv_f64_to_i32(double x);
int32_t __dao_conv_i64_to_i32(int64_t x);

// Float ↔ float
double __dao_conv_f32_to_f64(float x);
float __dao_conv_f64_to_f32(double x);

// Integer → float
float __dao_conv_i32_to_f32(int32_t x);
double __dao_conv_i64_to_f64(int64_t x);
float __dao_conv_i64_to_f32(int64_t x);

// Float → integer (trapping)
int64_t __dao_conv_f64_to_i64(double x);
int32_t __dao_conv_f32_to_i32(float x);
int64_t __dao_conv_f32_to_i64(float x);

// Integer widening (lossless)
int32_t __dao_conv_i8_to_i32(int8_t x);
int32_t __dao_conv_i16_to_i32(int16_t x);
int64_t __dao_conv_i8_to_i64(int8_t x);
int64_t __dao_conv_i16_to_i64(int16_t x);
uint32_t __dao_conv_u8_to_u32(uint8_t x);
uint32_t __dao_conv_u16_to_u32(uint16_t x);
uint64_t __dao_conv_u8_to_u64(uint8_t x);
uint64_t __dao_conv_u16_to_u64(uint16_t x);
uint64_t __dao_conv_u32_to_u64(uint32_t x);
int64_t __dao_conv_u32_to_i64(uint32_t x);

// Integer narrowing (trapping)
int8_t __dao_conv_i32_to_i8(int32_t x);
int16_t __dao_conv_i32_to_i16(int32_t x);
uint8_t __dao_conv_u32_to_u8(uint32_t x);
uint16_t __dao_conv_u32_to_u16(uint32_t x);

// Sign conversions (trapping)
uint32_t __dao_conv_i32_to_u32(int32_t x);
int32_t __dao_conv_u32_to_i32(uint32_t x);
uint64_t __dao_conv_i64_to_u64(int64_t x);
int64_t __dao_conv_u64_to_i64(uint64_t x);

// ---------------------------------------------------------------------------
// Runtime hook declarations — Overflow domain (explicit operations)
// ---------------------------------------------------------------------------

// Wrapping arithmetic: two's complement wrap, no trap.
int8_t __dao_wrapping_add_i8(int8_t a, int8_t b);
int8_t __dao_wrapping_sub_i8(int8_t a, int8_t b);
int8_t __dao_wrapping_mul_i8(int8_t a, int8_t b);
int16_t __dao_wrapping_add_i16(int16_t a, int16_t b);
int16_t __dao_wrapping_sub_i16(int16_t a, int16_t b);
int16_t __dao_wrapping_mul_i16(int16_t a, int16_t b);
int32_t __dao_wrapping_add_i32(int32_t a, int32_t b);
int32_t __dao_wrapping_sub_i32(int32_t a, int32_t b);
int32_t __dao_wrapping_mul_i32(int32_t a, int32_t b);
int64_t __dao_wrapping_add_i64(int64_t a, int64_t b);
int64_t __dao_wrapping_sub_i64(int64_t a, int64_t b);
int64_t __dao_wrapping_mul_i64(int64_t a, int64_t b);

// Saturating arithmetic: clamp to min/max representable value.
int8_t __dao_saturating_add_i8(int8_t a, int8_t b);
int8_t __dao_saturating_sub_i8(int8_t a, int8_t b);
int8_t __dao_saturating_mul_i8(int8_t a, int8_t b);
int16_t __dao_saturating_add_i16(int16_t a, int16_t b);
int16_t __dao_saturating_sub_i16(int16_t a, int16_t b);
int16_t __dao_saturating_mul_i16(int16_t a, int16_t b);
int32_t __dao_saturating_add_i32(int32_t a, int32_t b);
int32_t __dao_saturating_sub_i32(int32_t a, int32_t b);
int32_t __dao_saturating_mul_i32(int32_t a, int32_t b);
int64_t __dao_saturating_add_i64(int64_t a, int64_t b);
int64_t __dao_saturating_sub_i64(int64_t a, int64_t b);
int64_t __dao_saturating_mul_i64(int64_t a, int64_t b);

// ---------------------------------------------------------------------------
// Runtime hook declarations — Generator domain
// ---------------------------------------------------------------------------

// Allocate a generator frame of the given size and alignment.
// Returns a zeroed block. The caller (compiler-generated init function)
// populates the frame after allocation.
void *__dao_gen_alloc(int64_t size, int64_t align);

// Free a generator frame previously allocated by __dao_gen_alloc.
void __dao_gen_free(void *ptr);

// ---------------------------------------------------------------------------
// Runtime hook declarations — String domain
// ---------------------------------------------------------------------------

// Concatenate two strings, returning the result by value.
// The returned ptr points to a fresh buffer allocated through
// __dao_mem_alloc: owned by the current domain, reclaimed with it
// inside a `resource memory` block, process-lifetime in the root
// domain (as every string-producing hook below).
struct dao_string __dao_str_concat(const struct dao_string *a,
                                   const struct dao_string *b);

// Return the byte length of a string as i64.
int64_t __dao_str_length(const struct dao_string *s);

// Return the byte value at the given index as i32. Traps on out-of-range.
int32_t __dao_str_char_at(const struct dao_string *s, int64_t index);

// Extract a substring starting at `start` with byte length `len`.
// Traps if the range is out of bounds. Returns a copy owned by the
// current domain.
struct dao_string __dao_str_substring(const struct dao_string *s,
                                      int64_t start, int64_t len);

// Find the first occurrence of needle in s. Returns byte offset or -1.
int64_t __dao_str_index_of(const struct dao_string *s,
                            const struct dao_string *needle);

// Check if s starts with prefix.
bool __dao_str_starts_with(const struct dao_string *s,
                            const struct dao_string *prefix);

// Check if s ends with suffix.
bool __dao_str_ends_with(const struct dao_string *s,
                          const struct dao_string *suffix);

// Lexicographic comparison. Returns -1 if a < b, 0 if equal, 1 if a > b.
int32_t __dao_str_compare(const struct dao_string *a,
                           const struct dao_string *b);

// FNV-1a 64-bit hash of a string's bytes.
int64_t __dao_str_hash(const struct dao_string *s);

// ---------------------------------------------------------------------------
// Runtime hook declarations — Memory/resource domain
// ---------------------------------------------------------------------------

// Enter a scoped resource domain: an arena that becomes the current
// allocation domain until the matching exit.  Returns an opaque handle.
void *__dao_mem_resource_enter(void);

// Exit a scoped resource domain, reclaiming every allocation it made
// (and every domain still open inside it).  Takes the handle returned
// by enter; the enclosing domain becomes current again.
void __dao_mem_resource_exit(void *domain);

// ---------------------------------------------------------------------------
// Runtime hook declarations — Allocation domain
// ---------------------------------------------------------------------------

// Allocate size bytes with the given alignment in the current domain.
// Traps on failure.  In the root domain (outside every `resource
// memory` block) this is the system allocator; inside a block the
// memory belongs to the block's domain and is reclaimed with it.
void *__dao_mem_alloc(int64_t size, int64_t align);

// Allocate in the parent of the current domain: the domain that
// becomes current when the current block is left.  For values that
// must outlive the block (copy-out).  In the root domain, the root.
void* __dao_mem_alloc_outer(int64_t size, int64_t align);

// Resize allocation. Traps on failure. ptr may be null (acts as alloc).
// old_size is the number of bytes to preserve from the old allocation.
// Preserves alignment for alignments within max_align_t; for stronger
// alignments, allocates a new aligned block and copies old_size bytes.
void *__dao_mem_realloc(void *ptr, int64_t old_size, int64_t new_size,
                        int64_t align);

// Free allocation. Null is a no-op, and so is domain memory: a domain
// is reclaimed whole when its block is left.
void __dao_mem_free(void *ptr);

// A string owned by the current domain holding a copy of `len` bytes.
struct dao_string __dao_str_from_bytes(const char* bytes, int64_t len);

// A copy of `s` owned by the parent of the current domain: the domain
// that becomes current when the current block is left (copy-out).
struct dao_string __dao_str_copy_outer(const struct dao_string* s);

// ---------------------------------------------------------------------------
// Runtime hook declarations — Panic domain
// ---------------------------------------------------------------------------

// Print message to stderr and abort. Does not return.
void __dao_panic(const struct dao_string *msg);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // DAO_RUNTIME_ABI_H
