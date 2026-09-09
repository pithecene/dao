// resource.c — Scoped resource domain hooks.
//
// `resource memory <name> =>` enters a domain and leaves it on every
// control-flow path; the domain is the arena every allocation inside
// the block lands in (runtime/memory/domain.h).  The handle is the
// domain itself, opaque to compiled code.

#include "dao_abi.h"

#include "../memory/domain.h"

void *__dao_mem_resource_enter(void) { return dao_domain_push(); }

void __dao_mem_resource_exit(void *domain) {
  dao_domain_pop((struct dao_domain *)domain);
}
