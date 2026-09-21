#ifndef DAO_FRONTEND_TYPES_PTR_OPS_H
#define DAO_FRONTEND_TYPES_PTR_OPS_H

#include <cstdint>
#include <optional>
#include <string_view>

namespace dao {

// ---------------------------------------------------------------------------
// PtrOp — the operations of the compiler-standard `Ptr<T>`
// (docs/contracts/ADR_RAW_POINTER_SURFACE.md).
//
// Source reaches them as ordinary calls: `Ptr<T>::new()` and the methods
// `get`, `set`, `offset`, `cast<U>`, and `is_null`.  The method set is
// the compiler's, recognized once the receiver's semantic type is a
// pointer; HIR and MIR carry the operation itself, and only the backend
// speaks in load / store / getelementptr.
// ---------------------------------------------------------------------------

enum class PtrOp : std::uint8_t {
  New,    // Ptr<T>::new(): the null pointer
  Get,    // p.get(): T
  Set,    // p.set(value: T): void
  Offset, // p.offset(elements: i64): Ptr<T>
  Cast,   // p.cast<U>(): Ptr<U>
  IsNull, // p.is_null(): bool
};

/// The method of `Ptr<T>` a member name denotes, for a receiver already
/// known to be a pointer.  `new` is the static factory, never a method.
[[nodiscard]] inline auto ptr_method_op(std::string_view name) -> std::optional<PtrOp> {
  if (name == "get") {
    return PtrOp::Get;
  }
  if (name == "set") {
    return PtrOp::Set;
  }
  if (name == "offset") {
    return PtrOp::Offset;
  }
  if (name == "cast") {
    return PtrOp::Cast;
  }
  if (name == "is_null") {
    return PtrOp::IsNull;
  }
  return std::nullopt;
}

/// Whether the operation reads, writes, or traverses the memory the
/// pointer addresses: those need `mode unsafe =>` and a sized pointee.
[[nodiscard]] constexpr auto ptr_op_touches_memory(PtrOp operation) -> bool {
  return operation == PtrOp::Get || operation == PtrOp::Set || operation == PtrOp::Offset;
}

[[nodiscard]] constexpr auto ptr_op_name(PtrOp operation) -> std::string_view {
  switch (operation) {
  case PtrOp::New:
    return "new";
  case PtrOp::Get:
    return "get";
  case PtrOp::Set:
    return "set";
  case PtrOp::Offset:
    return "offset";
  case PtrOp::Cast:
    return "cast";
  case PtrOp::IsNull:
    return "is_null";
  }
  return "?";
}

} // namespace dao

#endif // DAO_FRONTEND_TYPES_PTR_OPS_H
