// generator.c — Generator frame allocation for the Dao runtime.
//
// Generator frames are allocated by the compiler-generated init
// function and freed when the for-loop iterator is destroyed.  Both go
// through the memory hooks, so a frame lives in the domain current when
// the generator was created and is reclaimed with it (freeing domain
// memory is a no-op).

#include "dao_abi.h"

#include <stdlib.h>
#include <string.h>

void *__dao_gen_alloc(int64_t size, int64_t align) {
  // At least pointer alignment: frames hold pointers, and the domain
  // allocator bumps to whatever alignment it is given.
  int64_t a = align < (int64_t)sizeof(void *) ? (int64_t)sizeof(void *) : align;
  void *ptr = __dao_mem_alloc(size, a);
  memset(ptr, 0, (size_t)(size < 0 ? 0 : size));
  return ptr;
}

void __dao_gen_free(void *ptr) { __dao_mem_free(ptr); }
