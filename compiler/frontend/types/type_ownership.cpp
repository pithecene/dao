#include "frontend/types/type_ownership.h"

#include <algorithm>
#include <unordered_set>

namespace dao {

namespace {

auto owns_heap_memory_impl(const Type* t, std::unordered_set<const Type*>& seen) -> bool {
  if (t == nullptr || !seen.insert(t).second) {
    return false;
  }
  switch (t->kind()) {
  case TypeKind::Named:
    return static_cast<const TypeNamed*>(t)->name() == "string";
  case TypeKind::Generator:
    return true;
  case TypeKind::GenericParam:
    return true;
  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(t);
    return std::ranges::any_of(st->fields(), [&](const StructField& f) {
      return f.type != nullptr &&
             (f.type->kind() == TypeKind::Pointer || owns_heap_memory_impl(f.type, seen));
    });
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(t);
    return std::ranges::any_of(en->variants(), [&](const EnumVariant& v) {
      return std::ranges::any_of(v.payload_types, [&](const Type* p) {
        return p != nullptr && (p->kind() == TypeKind::Pointer || owns_heap_memory_impl(p, seen));
      });
    });
  }
  default:
    return false;
  }
}

} // namespace

auto owns_heap_memory(const Type* type) -> bool {
  std::unordered_set<const Type*> seen;
  return owns_heap_memory_impl(type, seen);
}

namespace {

auto holds_generator_impl(const Type* t, std::unordered_set<const Type*>& seen) -> bool {
  if (t == nullptr || !seen.insert(t).second) {
    return false;
  }
  switch (t->kind()) {
  case TypeKind::Generator:
    return true;
  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(t);
    return std::ranges::any_of(
        st->fields(), [&](const StructField& f) { return holds_generator_impl(f.type, seen); });
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(t);
    return std::ranges::any_of(en->variants(), [&](const EnumVariant& v) {
      return std::ranges::any_of(v.payload_types,
                                 [&](const Type* p) { return holds_generator_impl(p, seen); });
    });
  }
  default:
    return false;
  }
}

} // namespace

auto holds_generator(const Type* type) -> bool {
  std::unordered_set<const Type*> seen;
  return holds_generator_impl(type, seen);
}

} // namespace dao
