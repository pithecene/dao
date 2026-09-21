#include "frontend/types/type_query.h"

#include <unordered_set>

namespace dao {

namespace {

/// Whether any parameter the caller is looking for is reachable in the
/// type.  One walk answers both questions asked of it — whether a type
/// mentions a parameter at all, and whether it mentions one a given
/// declaration declares.
auto mentions_walk(const Type* type,
                   const std::function<bool(const TypeGenericParam*)>& wanted,
                   std::unordered_set<const Type*>& visited, bool* cut) -> bool {
  if (type == nullptr) {
    return false;
  }
  if (!visited.insert(type).second) {
    // Already walked, or reached back into: nothing NEW is reachable
    // here, and what was cut is the caller's to remember or not.
    if (cut != nullptr) {
      *cut = true;
    }
    return false;
  }
  switch (type->kind()) {
  case TypeKind::GenericParam:
    return wanted(static_cast<const TypeGenericParam*>(type));
  case TypeKind::Pointer:
    return mentions_walk(static_cast<const TypePointer*>(type)->pointee(), wanted, visited, cut);
  case TypeKind::Generator:
    return mentions_walk(static_cast<const TypeGenerator*>(type)->yield_type(), wanted, visited, cut);
  case TypeKind::Function: {
    const auto* fn = static_cast<const TypeFunction*>(type);
    for (const auto* param : fn->param_types()) {
      if (mentions_walk(param, wanted, visited, cut)) {
        return true;
      }
    }
    return mentions_walk(fn->return_type(), wanted, visited, cut);
  }
  case TypeKind::Named: {
    const auto* named = static_cast<const TypeNamed*>(type);
    for (const auto* arg : named->type_args()) {
      if (mentions_walk(arg, wanted, visited, cut)) {
        return true;
      }
    }
    return false;
  }
  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(type);
    for (const auto* arg : st->type_args()) {
      if (mentions_walk(arg, wanted, visited, cut)) {
        return true;
      }
    }
    for (const auto& field : st->fields()) {
      if (mentions_walk(field.type, wanted, visited, cut)) {
        return true;
      }
    }
    return false;
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    for (const auto* arg : en->type_args()) {
      if (mentions_walk(arg, wanted, visited, cut)) {
        return true;
      }
    }
    for (const auto& variant : en->variants()) {
      for (const auto* payload : variant.payload_types) {
        if (mentions_walk(payload, wanted, visited, cut)) {
          return true;
        }
      }
    }
    return false;
  }
  default:
    return false;
  }
}

} // namespace

auto type_mentions_generic_param(const Type* type) -> bool {
  std::unordered_set<const Type*> visited;
  return mentions_walk(
      type, [](const TypeGenericParam* /*param*/) { return true; }, visited, nullptr);
}

auto GenericParamReach::mentions(const Type* type) -> bool {
  if (type == nullptr) {
    return false;
  }
  if (auto known = answered_.find(type); known != answered_.end()) {
    return known->second;
  }
  std::unordered_set<const Type*> visited;
  bool cut = false;
  const bool found = mentions_walk(
      type, [](const TypeGenericParam* /*param*/) { return true; }, visited, &cut);
  // A cut walk saw less than the whole type, so its "no" is only a "no
  // so far"; a "yes" stands however the walk went.
  if (found || !cut) {
    answered_.emplace(type, found);
  }
  return found;
}

auto type_mentions_param_of(const Type* type, const Decl* binder) -> bool {
  if (binder == nullptr) {
    return false;
  }
  std::unordered_set<const Type*> visited;
  return mentions_walk(
      type, [binder](const TypeGenericParam* param) { return param->binder() == binder; }, visited,
      nullptr);
}

} // namespace dao
