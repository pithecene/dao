// string.c — String operation hooks.

#include "dao_abi.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Every string this file produces is allocated through __dao_mem_alloc,
// so it lands in the current allocation domain: process-lifetime in the
// root domain, reclaimed with the block inside `resource memory`.

struct dao_string __dao_str_concat(const struct dao_string *a,
                                   const struct dao_string *b) {
  int64_t a_len = (a != NULL) ? a->len : 0;
  int64_t b_len = (b != NULL) ? b->len : 0;
  int64_t total = a_len + b_len;

  if (total == 0) {
    return (struct dao_string){.ptr = NULL, .len = 0};
  }

  char *buf = (char *)__dao_mem_alloc(total, 1);

  if (a_len > 0 && a->ptr != NULL) {
    memcpy(buf, a->ptr, (size_t)a_len);
  }
  if (b_len > 0 && b->ptr != NULL) {
    memcpy(buf + a_len, b->ptr, (size_t)b_len);
  }

  return (struct dao_string){.ptr = buf, .len = total};
}

int64_t __dao_str_length(const struct dao_string *s) {
  if (s == NULL) {
    return 0;
  }
  return s->len;
}

// Return the byte value at the given index. Traps on out-of-range.
int32_t __dao_str_char_at(const struct dao_string *s, int64_t index) {
  if (s == NULL || index < 0 || index >= s->len) {
    fprintf(stderr, "dao: string index out of range: %lld (length %lld)\n",
            (long long)index, (long long)(s ? s->len : 0));
    abort();
  }
  return (int32_t)(unsigned char)s->ptr[index];
}

// Extract a substring starting at `start` with byte length `len`.
// Traps if the range is out of bounds. Returns a copy owned by the
// current domain.
struct dao_string __dao_str_substring(const struct dao_string *s,
                                      int64_t start, int64_t len) {
  int64_t s_len = (s != NULL) ? s->len : 0;
  // Use overflow-safe check: after ruling out negatives, compare
  // start > s_len - len instead of start + len > s_len (which can
  // overflow signed int64_t for large positive inputs, invoking UB).
  if (start < 0 || len < 0 || len > s_len || start > s_len - len) {
    fprintf(stderr,
            "dao: substring out of range: start=%lld len=%lld (string length %lld)\n",
            (long long)start, (long long)len, (long long)s_len);
    abort();
  }
  if (len == 0) {
    return (struct dao_string){.ptr = NULL, .len = 0};
  }
  char *buf = (char *)__dao_mem_alloc(len, 1);
  memcpy(buf, s->ptr + start, (size_t)len);
  return (struct dao_string){.ptr = buf, .len = len};
}

// Find the first occurrence of needle in s. Returns byte offset or -1.
int64_t __dao_str_index_of(const struct dao_string *s,
                            const struct dao_string *needle) {
  if (s == NULL || needle == NULL) return -1;
  if (needle->len == 0) return 0;
  if (needle->len > s->len) return -1;

  int64_t limit = s->len - needle->len;
  for (int64_t i = 0; i <= limit; i++) {
    if (memcmp(s->ptr + i, needle->ptr, (size_t)needle->len) == 0) {
      return i;
    }
  }
  return -1;
}

// Check if s starts with prefix.
bool __dao_str_starts_with(const struct dao_string *s,
                            const struct dao_string *prefix) {
  if (s == NULL || prefix == NULL) return false;
  if (prefix->len == 0) return true;
  if (prefix->len > s->len) return false;
  return memcmp(s->ptr, prefix->ptr, (size_t)prefix->len) == 0;
}

// Check if s ends with suffix.
bool __dao_str_ends_with(const struct dao_string *s,
                          const struct dao_string *suffix) {
  if (s == NULL || suffix == NULL) return false;
  if (suffix->len == 0) return true;
  if (suffix->len > s->len) return false;
  int64_t offset = s->len - suffix->len;
  return memcmp(s->ptr + offset, suffix->ptr, (size_t)suffix->len) == 0;
}

// FNV-1a 64-bit hash of a string's bytes. Returns a signed i64 whose
// bit pattern is the unsigned FNV-1a result.
int64_t __dao_str_hash(const struct dao_string *s) {
  uint64_t hash = 14695981039346656037ULL;
  int64_t len = (s != NULL) ? s->len : 0;
  const char *ptr = (s != NULL) ? s->ptr : NULL;
  for (int64_t i = 0; i < len; i++) {
    hash ^= (uint8_t)ptr[i];
    hash *= 1099511628211ULL;
  }
  int64_t result;
  memcpy(&result, &hash, sizeof result);
  return result;
}

// Lexicographic comparison. Returns -1 if a < b, 0 if equal, 1 if a > b.
int32_t __dao_str_compare(const struct dao_string *a,
                           const struct dao_string *b) {
  int64_t a_len = (a != NULL) ? a->len : 0;
  int64_t b_len = (b != NULL) ? b->len : 0;
  int64_t min_len = (a_len < b_len) ? a_len : b_len;

  if (min_len > 0 && a->ptr != NULL && b->ptr != NULL) {
    int cmp = memcmp(a->ptr, b->ptr, (size_t)min_len);
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
  }

  if (a_len < b_len) return -1;
  if (a_len > b_len) return 1;
  return 0;
}
