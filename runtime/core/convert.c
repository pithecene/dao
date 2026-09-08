// convert.c — Dao runtime conversion hooks.
//
// Implements: scalar-to-string, numeric type conversions
// Authority:  docs/contracts/CONTRACT_RUNTIME_ABI.md
//
// String conversion results are heap-allocated via malloc.  The caller
// owns the memory; in the current runtime there is no automatic
// deallocation — these allocations leak until process exit.  Future
// arena/GC integration will reclaim them.  Matches the convention used
// by __dao_str_concat in string.c.
//
// Earlier versions returned pointers to thread-local static buffers,
// which silently corrupted any data structure (e.g. HashMap keys) that
// stored the returned string: the next conversion call overwrote the
// previous contents in place.
//
// Numeric conversions trap (abort) on out-of-range or invalid inputs.

#include "dao_abi.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper: format into a stack buffer of known size, then duplicate
// into an allocation of the current domain sized to the actual length.
static struct dao_string conv_str_dup(const char *buf, int len) {
  if (len <= 0) {
    return (struct dao_string){.ptr = NULL, .len = 0};
  }
  char *heap = (char *)__dao_mem_alloc(len, 1);
  memcpy(heap, buf, (size_t)len);
  return (struct dao_string){.ptr = heap, .len = len};
}

struct dao_string __dao_conv_i8_to_string(int8_t x) {
  char buf[8];
  int len = snprintf(buf, sizeof(buf), "%d", (int)x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_i16_to_string(int16_t x) {
  char buf[8];
  int len = snprintf(buf, sizeof(buf), "%d", (int)x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_i32_to_string(int32_t x) {
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%d", x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_i64_to_string(int64_t x) {
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%lld", (long long)x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_u8_to_string(uint8_t x) {
  char buf[8];
  int len = snprintf(buf, sizeof(buf), "%u", (unsigned)x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_u16_to_string(uint16_t x) {
  char buf[8];
  int len = snprintf(buf, sizeof(buf), "%u", (unsigned)x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_u32_to_string(uint32_t x) {
  char buf[16];
  int len = snprintf(buf, sizeof(buf), "%u", x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_u64_to_string(uint64_t x) {
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_f32_to_string(float x) {
  char buf[64];
  int len = snprintf(buf, sizeof(buf), "%g", (double)x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_f64_to_string(double x) {
  char buf[64];
  int len = snprintf(buf, sizeof(buf), "%g", x);
  return conv_str_dup(buf, len);
}

struct dao_string __dao_conv_bool_to_string(bool x) {
  if (x) {
    return (struct dao_string){.ptr = "true", .len = 4};
  }
  return (struct dao_string){.ptr = "false", .len = 5};
}

// ---------------------------------------------------------------------------
// Numeric type conversions
// ---------------------------------------------------------------------------

// i32 -> f64: exact (all i32 values are representable in f64).
double __dao_conv_i32_to_f64(int32_t x) { return (double)x; }

// i32 -> i64: exact (lossless widening).
int64_t __dao_conv_i32_to_i64(int32_t x) { return (int64_t)x; }

// f64 -> i32: truncates toward zero. Traps on NaN, Inf, or out-of-range.
int32_t __dao_conv_f64_to_i32(double x) {
  if (isnan(x) || isinf(x) || x < -2147483648.0 || x > 2147483647.0) {
    fprintf(stderr, "dao: numeric conversion error: f64 value out of i32 range\n");
    abort();
  }
  return (int32_t)x;
}

// i64 -> i32: traps if value does not fit.
int32_t __dao_conv_i64_to_i32(int64_t x) {
  if (x < INT32_MIN || x > INT32_MAX) {
    fprintf(stderr, "dao: numeric conversion error: i64 value out of i32 range\n");
    abort();
  }
  return (int32_t)x;
}

// ---------------------------------------------------------------------------
// Float ↔ float conversions
// ---------------------------------------------------------------------------

// f32 -> f64: exact (IEEE widening).
double __dao_conv_f32_to_f64(float x) { return (double)x; }

// f64 -> f32: rounds to nearest (IEEE narrowing).
float __dao_conv_f64_to_f32(double x) { return (float)x; }

// ---------------------------------------------------------------------------
// Integer → float conversions
// ---------------------------------------------------------------------------

// i32 -> f32: may lose precision for large i32 values.
float __dao_conv_i32_to_f32(int32_t x) { return (float)x; }

// i64 -> f64: may lose precision for large i64 values.
double __dao_conv_i64_to_f64(int64_t x) { return (double)x; }

// i64 -> f32: may lose precision.
float __dao_conv_i64_to_f32(int64_t x) { return (float)x; }

// ---------------------------------------------------------------------------
// Float → integer conversions (trapping)
// ---------------------------------------------------------------------------

// f64 -> i64: truncates toward zero. Traps on NaN, Inf, or out-of-range.
int64_t __dao_conv_f64_to_i64(double x) {
  if (isnan(x) || isinf(x) || x < (double)INT64_MIN || x >= 9.223372036854776e+18) {
    fprintf(stderr, "dao: numeric conversion error: f64 value out of i64 range\n");
    abort();
  }
  return (int64_t)x;
}

// f32 -> i32: truncates toward zero. Traps on NaN, Inf, or out-of-range.
int32_t __dao_conv_f32_to_i32(float x) {
  if (isnan(x) || isinf(x) || x < -2147483648.0f || x > 2147483647.0f) {
    fprintf(stderr, "dao: numeric conversion error: f32 value out of i32 range\n");
    abort();
  }
  return (int32_t)x;
}

// f32 -> i64: truncates toward zero. Traps on NaN, Inf, or out-of-range.
int64_t __dao_conv_f32_to_i64(float x) {
  // INT64_MAX is 2^63-1; the nearest float above is 2^63 = 9.223372e+18f.
  // INT64_MIN is -2^63 = -9.223372e+18f (exactly representable in float).
  if (isnan(x) || isinf(x) || x < (float)INT64_MIN || x >= 9.223372036854776e+18f) {
    fprintf(stderr, "dao: numeric conversion error: f32 value out of i64 range\n");
    abort();
  }
  return (int64_t)x;
}

// ---------------------------------------------------------------------------
// Integer widening (lossless)
// ---------------------------------------------------------------------------

int32_t __dao_conv_i8_to_i32(int8_t x)   { return (int32_t)x; }
int32_t __dao_conv_i16_to_i32(int16_t x)  { return (int32_t)x; }
int64_t __dao_conv_i8_to_i64(int8_t x)    { return (int64_t)x; }
int64_t __dao_conv_i16_to_i64(int16_t x)  { return (int64_t)x; }
uint32_t __dao_conv_u8_to_u32(uint8_t x)  { return (uint32_t)x; }
uint32_t __dao_conv_u16_to_u32(uint16_t x) { return (uint32_t)x; }
uint64_t __dao_conv_u8_to_u64(uint8_t x)  { return (uint64_t)x; }
uint64_t __dao_conv_u16_to_u64(uint16_t x) { return (uint64_t)x; }
uint64_t __dao_conv_u32_to_u64(uint32_t x) { return (uint64_t)x; }
int64_t __dao_conv_u32_to_i64(uint32_t x)  { return (int64_t)x; }

// ---------------------------------------------------------------------------
// Integer narrowing (trapping)
// ---------------------------------------------------------------------------

int8_t __dao_conv_i32_to_i8(int32_t x) {
  if (x < INT8_MIN || x > INT8_MAX) {
    fprintf(stderr, "dao: numeric conversion error: i32 value out of i8 range\n");
    abort();
  }
  return (int8_t)x;
}

int16_t __dao_conv_i32_to_i16(int32_t x) {
  if (x < INT16_MIN || x > INT16_MAX) {
    fprintf(stderr, "dao: numeric conversion error: i32 value out of i16 range\n");
    abort();
  }
  return (int16_t)x;
}

uint8_t __dao_conv_u32_to_u8(uint32_t x) {
  if (x > UINT8_MAX) {
    fprintf(stderr, "dao: numeric conversion error: u32 value out of u8 range\n");
    abort();
  }
  return (uint8_t)x;
}

uint16_t __dao_conv_u32_to_u16(uint32_t x) {
  if (x > UINT16_MAX) {
    fprintf(stderr, "dao: numeric conversion error: u32 value out of u16 range\n");
    abort();
  }
  return (uint16_t)x;
}

// ---------------------------------------------------------------------------
// Sign conversions (trapping)
// ---------------------------------------------------------------------------

uint32_t __dao_conv_i32_to_u32(int32_t x) {
  if (x < 0) {
    fprintf(stderr, "dao: numeric conversion error: negative i32 to u32\n");
    abort();
  }
  return (uint32_t)x;
}

int32_t __dao_conv_u32_to_i32(uint32_t x) {
  if (x > INT32_MAX) {
    fprintf(stderr, "dao: numeric conversion error: u32 value out of i32 range\n");
    abort();
  }
  return (int32_t)x;
}

uint64_t __dao_conv_i64_to_u64(int64_t x) {
  if (x < 0) {
    fprintf(stderr, "dao: numeric conversion error: negative i64 to u64\n");
    abort();
  }
  return (uint64_t)x;
}

int64_t __dao_conv_u64_to_i64(uint64_t x) {
  if (x > INT64_MAX) {
    fprintf(stderr, "dao: numeric conversion error: u64 value out of i64 range\n");
    abort();
  }
  return (int64_t)x;
}
