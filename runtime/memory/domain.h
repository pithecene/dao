// domain.h — Allocation domains behind `resource memory`.
//
// Runtime-internal: shared by the allocation hooks (alloc.c) and the
// resource hooks (resource.c).  Not part of the compiler-facing ABI;
// dao_abi.h declares what compiled code calls.
//
// A domain is an arena with a lifetime bounded by a `resource memory`
// block: a chunk list with a bump pointer, reclaimed wholesale when the
// block is left.  Domains nest; the innermost open one is current.  The
// root domain is the process itself and uses the system allocator, so
// programs without `resource memory` behave exactly as before.
#ifndef DAO_RUNTIME_DOMAIN_H
#define DAO_RUNTIME_DOMAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dao_domain;

// Open a new domain under the current one and make it current.
struct dao_domain* dao_domain_push(void);

// Close `domain` and every domain opened inside it that is still open,
// reclaiming all their memory, and make its parent current.  Traps if
// `domain` is not open: an exit without its enter is a compiler defect.
void dao_domain_pop(struct dao_domain* domain);

// Bytes currently held by open domains (chunk capacity, all domains).
// For tests and measurement; zero when only the root domain is open.
int64_t dao_domain_bytes_held(void);

// How many domains are open above the root.
int64_t dao_domain_depth(void);

#ifdef __cplusplus
}
#endif

#endif // DAO_RUNTIME_DOMAIN_H
