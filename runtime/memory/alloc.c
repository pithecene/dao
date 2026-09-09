// alloc.c — Dao runtime allocation hooks and the domains behind them.
//
// Authority: docs/contracts/CONTRACT_RUNTIME_ABI.md
// Placement: runtime/memory/ per docs/ARCH_INDEX.md
//
// Outside every `resource memory` block the root domain is current and
// every hook is the system allocator, as it always was.  Inside a block
// the block's domain is current: allocations bump inside its chunks,
// reallocation copies within it and abandons the old block to it, and
// freeing domain memory is a no-op -- the whole domain is reclaimed at
// once when the block is left (domain.h).

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/dao_abi.h"
#include "domain.h"

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------

struct dao_chunk {
  struct dao_chunk *next;
  char *base;
  size_t capacity;
  size_t used;
};

struct dao_domain {
  struct dao_domain *parent;
  struct dao_chunk *chunks; // newest first; allocation bumps in the newest
  size_t next_chunk_size;   // doubles per chunk up to kChunkMax
};

enum {
  kChunkFirst = 64 * 1024,
  kChunkMax = 8 * 1024 * 1024,
};

static struct dao_domain *current_domain = NULL; // NULL: the root domain
static int64_t open_domains = 0;
static int64_t bytes_held = 0;

static void trap(const char *what, int64_t size, int64_t align) {
  fprintf(stderr, "dao panic: %s (size=%lld, align=%lld)\n", what, (long long)size,
          (long long)align);
  abort();
}

static size_t round_up(size_t size, size_t align) {
  return (size + align - 1) & ~(align - 1);
}

static struct dao_chunk *chunk_new(struct dao_domain *domain, size_t capacity) {
  struct dao_chunk *chunk = (struct dao_chunk *)malloc(sizeof(struct dao_chunk));
  char *base = (char *)malloc(capacity);
  if (chunk == NULL || base == NULL) {
    trap("allocation failed", (int64_t)capacity, 1);
  }
  chunk->base = base;
  chunk->capacity = capacity;
  chunk->used = 0;
  chunk->next = domain->chunks;
  domain->chunks = chunk;
  bytes_held += (int64_t)capacity;
  return chunk;
}

// Bump-allocate in `domain`.  A request larger than a chunk gets a
// chunk of its own; otherwise the newest chunk serves it, or a fresh
// one (growing geometrically) when it does not fit.
static void *domain_alloc(struct dao_domain *domain, size_t size, size_t align) {
  if (size == 0) {
    size = align;
  }
  struct dao_chunk *chunk = domain->chunks;
  if (chunk != NULL) {
    size_t start = round_up((size_t)(uintptr_t)(chunk->base + chunk->used), align) -
                   (size_t)(uintptr_t)chunk->base;
    if (start + size <= chunk->capacity) {
      chunk->used = start + size;
      return chunk->base + start;
    }
  }
  size_t needed = size + align;
  size_t capacity = domain->next_chunk_size;
  if (needed > capacity) {
    capacity = needed;
  }
  // Growth stays geometric whatever the request, so a domain's chunk
  // list is logarithmic in the bytes it holds (past kChunkMax each
  // request that large gets its own chunk: at 8 MiB apiece, a few
  // thousand at most on any machine).  That bounds every walk of it.
  if (domain->next_chunk_size < (size_t)kChunkMax) {
    domain->next_chunk_size *= 2;
  }
  chunk = chunk_new(domain, capacity);
  size_t start = round_up((size_t)(uintptr_t)chunk->base, align) - (size_t)(uintptr_t)chunk->base;
  chunk->used = start + size;
  return chunk->base + start;
}

// Whether `ptr` lies in a chunk of an open domain.  Addresses are
// compared as integers: relational comparison of pointers into
// different allocations is undefined in C.  The walk is bounded by the
// open domains times their chunk lists, which grow geometrically
// (domain_alloc); it runs only for a free or a reallocation while a
// block is open.
static int domain_owns(const void *ptr) {
  uintptr_t p = (uintptr_t)ptr;
  for (const struct dao_domain *d = current_domain; d != NULL; d = d->parent) {
    for (const struct dao_chunk *c = d->chunks; c != NULL; c = c->next) {
      uintptr_t base = (uintptr_t)c->base;
      if (p >= base && p - base < (uintptr_t)c->capacity) {
        return 1;
      }
    }
  }
  return 0;
}

static void domain_release(struct dao_domain *domain) {
  struct dao_chunk *chunk = domain->chunks;
  while (chunk != NULL) {
    struct dao_chunk *next = chunk->next;
    bytes_held -= (int64_t)chunk->capacity;
    free(chunk->base);
    free(chunk);
    chunk = next;
  }
  free(domain);
}

struct dao_domain *dao_domain_push(void) {
  struct dao_domain *domain = (struct dao_domain *)malloc(sizeof(struct dao_domain));
  if (domain == NULL) {
    trap("allocation failed", (int64_t)sizeof(struct dao_domain), 1);
  }
  domain->parent = current_domain;
  domain->chunks = NULL;
  domain->next_chunk_size = kChunkFirst;
  current_domain = domain;
  open_domains++;
  return domain;
}

void dao_domain_pop(struct dao_domain *domain) {
  const struct dao_domain *open = current_domain;
  while (open != NULL && open != domain) {
    open = open->parent;
  }
  if (open == NULL) {
    fprintf(stderr, "dao panic: resource exit of a domain that is not open\n");
    abort();
  }
  // Leave every domain opened inside `domain` too: a `return` through
  // nested blocks exits each in order, but the runtime does not depend
  // on it.
  while (current_domain != NULL) {
    struct dao_domain *closing = current_domain;
    current_domain = closing->parent;
    open_domains--;
    domain_release(closing);
    if (closing == domain) {
      break;
    }
  }
}

int64_t dao_domain_bytes_held(void) { return bytes_held; }

int64_t dao_domain_depth(void) { return open_domains; }

// ---------------------------------------------------------------------------
// Root-domain allocation: the system allocator
// ---------------------------------------------------------------------------

static void *root_alloc(int64_t size, int64_t align) {
  if (size <= 0) {
    size = align; // Allocate at least one unit.
  }
  // aligned_alloc requires size to be a multiple of alignment.
  size_t rounded = round_up((size_t)size, (size_t)align);
  void *ptr = aligned_alloc((size_t)align, rounded);
  if (ptr == NULL) {
    trap("allocation failed", size, align);
  }
  return ptr;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

void *__dao_mem_alloc(int64_t size, int64_t align) {
  if (current_domain == NULL) {
    return root_alloc(size, align);
  }
  return domain_alloc(current_domain, (size_t)(size < 0 ? 0 : size), (size_t)align);
}

void *__dao_mem_alloc_outer(int64_t size, int64_t align) {
  if (current_domain == NULL || current_domain->parent == NULL) {
    return root_alloc(size, align);
  }
  return domain_alloc(current_domain->parent, (size_t)(size < 0 ? 0 : size), (size_t)align);
}

void *__dao_mem_realloc(void *ptr, int64_t old_size, int64_t new_size,
                        int64_t align) {
  if (ptr == NULL) {
    return __dao_mem_alloc(new_size, align);
  }
  if (new_size <= 0) {
    __dao_mem_free(ptr);
    return NULL;
  }
  if (current_domain != NULL) {
    // Copy into the domain and abandon the old block: to the domain if
    // it is domain memory, or back to the system if it was the root's.
    void *fresh = domain_alloc(current_domain, (size_t)new_size, (size_t)align);
    int64_t copy_size = old_size < new_size ? old_size : new_size;
    if (copy_size > 0) {
      memcpy(fresh, ptr, (size_t)copy_size);
    }
    if (!domain_owns(ptr)) {
      free(ptr);
    }
    return fresh;
  }
  // Standard realloc does not preserve alignment beyond max_align_t.
  // For alignments within max_align_t, plain realloc is sufficient.
  // For stronger alignments, allocate new + memcpy + free.
  if (align <= (int64_t)_Alignof(max_align_t)) {
    void *result = realloc(ptr, (size_t)new_size);
    if (result == NULL) {
      trap("reallocation failed", new_size, align);
    }
    return result;
  }
  // Strong alignment: allocate fresh aligned block and copy.
  void *fresh = root_alloc(new_size, align);
  int64_t copy_size = old_size < new_size ? old_size : new_size;
  if (copy_size > 0) {
    memcpy(fresh, ptr, (size_t)copy_size);
  }
  free(ptr);
  return fresh;
}

void __dao_mem_free(void *ptr) {
  if (ptr == NULL || domain_owns(ptr)) {
    return; // domain memory is reclaimed with its domain, not one block at a time
  }
  free(ptr);
}

struct dao_string __dao_str_copy_outer(const struct dao_string* s) {
  if (s == NULL || s->len <= 0 || s->ptr == NULL) {
    return (struct dao_string){.ptr = NULL, .len = 0};
  }
  char* buf = (char*)__dao_mem_alloc_outer(s->len, 1);
  memcpy(buf, s->ptr, (size_t)s->len);
  return (struct dao_string){.ptr = buf, .len = s->len};
}

struct dao_string __dao_str_from_bytes(const char *bytes, int64_t len) {
  if (len <= 0 || bytes == NULL) {
    return (struct dao_string){.ptr = NULL, .len = 0};
  }
  char *buf = (char *)__dao_mem_alloc(len, 1);
  memcpy(buf, bytes, (size_t)len);
  return (struct dao_string){.ptr = buf, .len = len};
}
