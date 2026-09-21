#include "frontend/typecheck/type_conversion.h"

#include <functional>
#include <unordered_set>
#include <utility>

namespace dao {

namespace {

/// Recursive helper for the repr-C-compatible struct predicate.
/// `visiting` tracks struct decl_ids currently being checked to detect
/// non-pointer self-reference cycles.
auto is_c_abi_compatible_impl(const Type* type,
                               std::unordered_set<const Decl*>& visiting)
    -> bool;

using TypePair = std::pair<const Type*, const Type*>;

struct TypePairHash {
  auto operator()(const TypePair& pair) const -> size_t {
    const std::hash<const void*> hash;
    return hash(pair.first) ^ (hash(pair.second) << 1U);
  }
};

/// Pairs the walk is already deciding.  A class may reach itself
/// (`class Node<T>: next: Ptr<Node<T>>`), so a pair that comes back
/// around is taken to match: whether the two types differ is settled by
/// the fields that are not the cycle.
using Deciding = std::unordered_set<TypePair, TypePairHash>;

auto binds(const Type* source, const Type* target, GenericBinding binding, Deciding& deciding) -> bool;

/// One field, payload, parameter, or pointee of a pair being decided.
auto binds_part(const Type* source, const Type* target, GenericBinding binding, Deciding& deciding)
    -> bool {
  if (source == target) {
    return true;
  }
  return binds(source, target, binding, deciding);
}

/// Whether `source` reaches `target` while binding the parameters the
/// call declares: the callee's own, and those of the class or enum its
/// signature is written with.  Such a parameter takes the type across
/// from it; every other type must mean what the target means.
auto binds(const Type* source, const Type* target, GenericBinding binding, Deciding& deciding)
    -> bool {
  if (source == nullptr || target == nullptr) {
    return source == target;
  }
  if (!deciding.insert(TypePair{source, target}).second) {
    return true;
  }

  // A parameter this call declares is what the comparison binds; one
  // declared elsewhere — an enclosing signature's own `T`, or the `T` of
  // a class whose instantiation is already fixed — names one type, its
  // own.  Parameters are interned by binder and position, so being that
  // one type is being the same object, which `binds_part` settled.
  if (target->kind() == TypeKind::GenericParam) {
    const auto* owner = static_cast<const TypeGenericParam*>(target)->binder();
    return owner != nullptr && (owner == binding.binder || owner == binding.owner);
  }

  if (source->kind() != target->kind()) {
    return false;
  }

  // Composite types carry the parameters inside them, so the walk
  // continues through each part.  Nothing here is covariant: a part
  // that binds nothing has to mean the same type.
  switch (target->kind()) {
  case TypeKind::Pointer:
    return binds_part(static_cast<const TypePointer*>(source)->pointee(),
                      static_cast<const TypePointer*>(target)->pointee(), binding, deciding);
  case TypeKind::Generator:
    return binds_part(static_cast<const TypeGenerator*>(source)->yield_type(),
                      static_cast<const TypeGenerator*>(target)->yield_type(), binding, deciding);
  case TypeKind::Function: {
    const auto* source_fn = static_cast<const TypeFunction*>(source);
    const auto* target_fn = static_cast<const TypeFunction*>(target);
    if (source_fn->param_types().size() != target_fn->param_types().size()) {
      return false;
    }
    for (size_t i = 0; i < source_fn->param_types().size(); ++i) {
      if (!binds_part(source_fn->param_types()[i], target_fn->param_types()[i], binding, deciding)) {
        return false;
      }
    }
    return binds_part(source_fn->return_type(), target_fn->return_type(), binding, deciding);
  }
  case TypeKind::Struct: {
    const auto* source_struct = static_cast<const TypeStruct*>(source);
    const auto* target_struct = static_cast<const TypeStruct*>(target);
    // Same class, then field by field: the fields are what an
    // instantiation differs in.
    if (source_struct->decl_id() != target_struct->decl_id() ||
        source_struct->name() != target_struct->name() ||
        source_struct->fields().size() != target_struct->fields().size() ||
        source_struct->type_args().size() != target_struct->type_args().size()) {
      return false;
    }
    // The instantiation first: a parameter no field mentions lives only
    // here, and two instantiations of one class are otherwise alike.
    for (size_t i = 0; i < source_struct->type_args().size(); ++i) {
      if (!binds_part(source_struct->type_args()[i], target_struct->type_args()[i], binding,
                      deciding)) {
        return false;
      }
    }
    for (size_t i = 0; i < source_struct->fields().size(); ++i) {
      if (!binds_part(source_struct->fields()[i].type, target_struct->fields()[i].type, binding,
                      deciding)) {
        return false;
      }
    }
    return true;
  }
  case TypeKind::Enum: {
    const auto* source_enum = static_cast<const TypeEnum*>(source);
    const auto* target_enum = static_cast<const TypeEnum*>(target);
    if (source_enum->decl_id() != target_enum->decl_id() ||
        source_enum->name() != target_enum->name() ||
        source_enum->variants().size() != target_enum->variants().size() ||
        source_enum->type_args().size() != target_enum->type_args().size()) {
      return false;
    }
    for (size_t i = 0; i < source_enum->type_args().size(); ++i) {
      if (!binds_part(source_enum->type_args()[i], target_enum->type_args()[i], binding, deciding)) {
        return false;
      }
    }
    for (size_t i = 0; i < source_enum->variants().size(); ++i) {
      const auto& source_variant = source_enum->variants()[i];
      const auto& target_variant = target_enum->variants()[i];
      if (source_variant.payload_types.size() != target_variant.payload_types.size()) {
        return false;
      }
      for (size_t j = 0; j < source_variant.payload_types.size(); ++j) {
        if (!binds_part(source_variant.payload_types[j], target_variant.payload_types[j], binding,
                        deciding)) {
          return false;
        }
      }
    }
    return true;
  }
  default:
    break;
  }
  return false;
}

} // namespace

auto is_assignable(const Type* source, const Type* target, GenericBinding binding) -> bool {
  if (source == target) {
    return true;
  }

  // One walk answers both: with a binder, a parameter that binder
  // declared takes the type across from it; with none, every parameter
  // is fixed and the walk is plain semantic equality — which is what
  // makes `Ptr<i32>` unassignable to `Ptr<T>`
  // (CONTRACT_TYPECHECKING_BASELINE §§4, 8).  It compares the types
  // themselves rather than a spelling of them: one meaning is many
  // objects, a class is not interned and substitution allocates at every
  // occurrence, and a pair already being decided is taken to match, so a
  // type that reaches itself terminates.
  Deciding deciding;
  return binds(source, target, binding, deciding);
}

auto is_numeric(const Type* type) -> bool {
  if (type == nullptr || type->kind() != TypeKind::Builtin) {
    return false;
  }
  auto kind = static_cast<const TypeBuiltin*>(type)->builtin();
  switch (kind) {
  case BuiltinKind::I8:
  case BuiltinKind::I16:
  case BuiltinKind::I32:
  case BuiltinKind::I64:
  case BuiltinKind::U8:
  case BuiltinKind::U16:
  case BuiltinKind::U32:
  case BuiltinKind::U64:
  case BuiltinKind::F32:
  case BuiltinKind::F64:
    return true;
  case BuiltinKind::Bool:
    return false;
  }
  return false;
}

auto is_integer(const Type* type) -> bool {
  if (type == nullptr || type->kind() != TypeKind::Builtin) {
    return false;
  }
  auto kind = static_cast<const TypeBuiltin*>(type)->builtin();
  switch (kind) {
  case BuiltinKind::I8:
  case BuiltinKind::I16:
  case BuiltinKind::I32:
  case BuiltinKind::I64:
  case BuiltinKind::U8:
  case BuiltinKind::U16:
  case BuiltinKind::U32:
  case BuiltinKind::U64:
    return true;
  default:
    return false;
  }
}

auto is_string(const Type* type) -> bool {
  if (type == nullptr || type->kind() != TypeKind::Named) {
    return false;
  }
  const auto* named = static_cast<const TypeNamed*>(type);
  // The predeclared string type has name "string" AND null decl_id,
  // distinguishing it from any user-defined nominal type named "string".
  // This must match the backend's authoritative predicate in
  // LlvmTypeLowering::is_string_type().
  return named->name() == "string" && named->decl_id() == nullptr;
}

auto is_float(const Type* type) -> bool {
  if (type == nullptr || type->kind() != TypeKind::Builtin) {
    return false;
  }
  auto kind = static_cast<const TypeBuiltin*>(type)->builtin();
  return kind == BuiltinKind::F32 || kind == BuiltinKind::F64;
}

auto is_c_abi_compatible(const Type* type) -> bool {
  std::unordered_set<const Decl*> visiting;
  return is_c_abi_compatible_impl(type, visiting);
}

namespace {

auto is_c_abi_compatible_impl(const Type* type,
                               std::unordered_set<const Decl*>& visiting)
    -> bool {
  if (type == nullptr) {
    return false;
  }
  switch (type->kind()) {
  case TypeKind::Builtin: {
    // All numeric builtins and bool are C ABI compatible — they map
    // directly to C scalar types (int8_t through uint64_t, float,
    // double, bool).
    auto kind = static_cast<const TypeBuiltin*>(type)->builtin();
    switch (kind) {
    case BuiltinKind::I8:
    case BuiltinKind::I16:
    case BuiltinKind::I32:
    case BuiltinKind::I64:
    case BuiltinKind::U8:
    case BuiltinKind::U16:
    case BuiltinKind::U32:
    case BuiltinKind::U64:
    case BuiltinKind::F32:
    case BuiltinKind::F64:
    case BuiltinKind::Bool:
      return true;
    }
    return false;
  }
  case TypeKind::Pointer:
    return true; // raw pointers — pointee type is not checked
  case TypeKind::Void:
    return true; // void return
  case TypeKind::Struct: {
    // Repr-C-compatible struct predicate (CONTRACT_C_ABI_INTEROP §4.3.1):
    //   1. at least one field (no empty structs)
    //   2. every field is recursively C-ABI-compatible
    //   3. no non-pointer self-reference cycle
    const auto* st = static_cast<const TypeStruct*>(type);
    if (st->fields().empty()) {
      return false; // empty struct rejected
    }
    // Cycle detection: if we're already visiting this struct, we have
    // a non-pointer self-reference (pointers don't recurse into this
    // branch — they return true in the Pointer case above).
    if (!visiting.insert(st->decl_id()).second) {
      return false; // recursive struct through non-pointer field
    }
    for (const auto& field : st->fields()) {
      if (!is_c_abi_compatible_impl(field.type, visiting)) {
        visiting.erase(st->decl_id());
        return false;
      }
    }
    visiting.erase(st->decl_id());
    return true;
  }
  case TypeKind::Function: {
    // Function pointer type: all param and return types must be
    // C-ABI-compatible (CONTRACT_C_ABI_INTEROP §4.4.2).
    const auto* fn_type = static_cast<const TypeFunction*>(type);
    for (const auto* param : fn_type->param_types()) {
      if (!is_c_abi_compatible_impl(param, visiting)) {
        return false;
      }
    }
    if (fn_type->return_type() != nullptr &&
        fn_type->return_type()->kind() != TypeKind::Void) {
      return is_c_abi_compatible_impl(fn_type->return_type(), visiting);
    }
    return true;
  }
  default:
    return false; // string, generator, named, enum, generic
  }
}

} // namespace

} // namespace dao
