#ifndef DAO_FRONTEND_TYPECHECK_TYPE_CONVERSION_H
#define DAO_FRONTEND_TYPECHECK_TYPE_CONVERSION_H

#include "frontend/types/type.h"

#include <cstdint>

namespace dao {

// ---------------------------------------------------------------------------
// Assignability and type comparison.
//
// Initial policy: exact semantic type equality.
// No implicit widening, no implicit promotion.
// See CONTRACT_TYPECHECKING_BASELINE.md §4.
// ---------------------------------------------------------------------------

/// Which type parameters in `target` this comparison is still binding.
///
/// A call site binds the parameters of the declaration it calls: the
/// argument decides what that declaration's `T` is, so such a parameter
/// position accepts the argument's type.  Every other parameter is
/// already fixed — one declared by an enclosing signature, or any
/// parameter seen from a return, a binding, an assignment, or an
/// operand — and names one type, its own; a value of another type is no
/// value of it.  Reading a fixed parameter as a wildcard would retype a
/// pointer for free, which §8 forbids: `fn disguise<T>(p: Ptr<i32>):
/// Ptr<T> -> p` is an error, and `cast<U>` is the one pointee
/// conversion (ADR_RAW_POINTER_SURFACE.md).
///
/// `binder` is the declaration whose parameters are being bound here,
/// or null where nothing is being bound.  A call through a function
/// value binds nothing: its signature belongs to whoever wrote it.
struct GenericBinding {
  const Decl* binder = nullptr;
  /// The class or enum whose parameters the callee's signature is
  /// written with, when the callee is one of its methods.
  const Decl* owner = nullptr;
};

/// The comparison that binds no parameter: every one of them is fixed.
inline constexpr GenericBinding kFixedGenerics{};

/// Returns true if `source` is assignable to `target`.
/// Current rule: exact semantic type equality (CONTRACT_TYPECHECKING_BASELINE §4).
/// Nominal and substituted types are compared by what they mean, since
/// one semantic type is many objects; see `type_identity_key`.
auto is_assignable(const Type* source, const Type* target,
                   GenericBinding binding = kFixedGenerics) -> bool;

/// Returns true if the type is a numeric builtin (integer or float).
auto is_numeric(const Type* type) -> bool;

/// Returns true if the type is an integer builtin.
auto is_integer(const Type* type) -> bool;

/// Returns true if the type is a float builtin.
auto is_float(const Type* type) -> bool;

/// Returns true if the type is the predeclared string type.
auto is_string(const Type* type) -> bool;

/// Returns true if the type is compatible with the C ABI boundary.
/// Supported: builtin scalars (i32, i64, f64, bool, etc.), pointers,
/// and repr-C-compatible structs (non-empty, all fields recursively
/// C-ABI-compatible, no non-pointer self-reference).
auto is_c_abi_compatible(const Type* type) -> bool;

} // namespace dao

#endif // DAO_FRONTEND_TYPECHECK_TYPE_CONVERSION_H
