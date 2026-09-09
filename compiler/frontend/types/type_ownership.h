// type_ownership.h — Which types own heap memory.
//
// A value of an owning type allocated inside a `resource memory` block
// does not outlive the block (CONTRACT_EXECUTION_CONTEXTS.md §Resources,
// law 7): the type checker rejects its escape until copy-out, and the
// MIR builder copies it out at the block's exits from E1 on.  Both ask
// here, so the rule lives once.
#pragma once

#include "frontend/types/type.h"

namespace dao {

/// Whether a value of this type owns heap memory: a string, a generator,
/// or a class or enum holding one of those -- or a raw pointer field, the
/// way `Vector` and `HashMap` own their buffers -- by value.  A pointer
/// value itself does not: it is the author's responsibility.  A generic
/// parameter does, conservatively: it may be instantiated with an owner.
[[nodiscard]] auto owns_heap_memory(const Type* type) -> bool;

/// Whether a value of this type is, or holds by value, a generator: its
/// frame's layout is private to its function, so no copier can copy it
/// out of a domain, and its escape stays rejected.
[[nodiscard]] auto holds_generator(const Type* type) -> bool;

} // namespace dao
