#include "frontend/types/type_context.h"

#include <cstdint>
#include <functional>

namespace dao {

// ---------------------------------------------------------------------------
// Hash helpers
// ---------------------------------------------------------------------------

namespace {

auto hash_combine(size_t seed, size_t value) -> size_t {
  // Boost-style hash combine.
  return seed ^ (value + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

auto hash_type_ptr(const Type* t) -> size_t {
  return std::hash<const Type*>{}(t);
}

} // namespace

auto TypeContext::FnKeyHash::operator()(const FnKey& key) const -> size_t {
  size_t h = hash_type_ptr(key.ret);
  for (const auto* p : key.params) {
    h = hash_combine(h, hash_type_ptr(p));
  }
  return h;
}

auto TypeContext::NamedKeyHash::operator()(const NamedKey& key) const -> size_t {
  size_t h = std::hash<const Decl*>{}(key.decl_id);
  for (const auto* a : key.type_args) {
    h = hash_combine(h, hash_type_ptr(a));
  }
  return h;
}

auto TypeContext::GenericParamKeyHash::operator()(const GenericParamKey& key) const -> size_t {
  size_t h = std::hash<const Decl*>{}(key.binder);
  return hash_combine(h, std::hash<uint32_t>{}(key.index));
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TypeContext::TypeContext() {
  // Pre-populate all builtin scalar types.
  for (uint8_t i = 0; i < kBuiltinKindCount; ++i) {
    auto kind = static_cast<BuiltinKind>(i);
    builtins_[i] = arena_.alloc<TypeBuiltin>(kind);
  }
  // Void is a compiler-internal singleton, not a builtin scalar.
  void_ = arena_.alloc<TypeVoid>();
}

TypeContext::~TypeContext() = default;

TypeContext::TypeContext(TypeContext&&) noexcept = default;
auto TypeContext::operator=(TypeContext&&) noexcept -> TypeContext& = default;

// ---------------------------------------------------------------------------
// Builtin access
// ---------------------------------------------------------------------------

auto TypeContext::builtin(BuiltinKind kind) -> const TypeBuiltin* {
  return builtins_[static_cast<uint8_t>(kind)];
}

auto TypeContext::void_type() -> const TypeVoid* {
  return void_;
}

// ---------------------------------------------------------------------------
// Interned constructors
// ---------------------------------------------------------------------------

auto TypeContext::pointer_to(const Type* pointee) -> const TypePointer* {
  auto [it, inserted] = pointer_map_.try_emplace(pointee, nullptr);
  if (inserted) {
    it->second = arena_.alloc<TypePointer>(pointee);
  }
  return it->second;
}

auto TypeContext::function_type(std::vector<const Type*> params, const Type* ret)
    -> const TypeFunction* {
  FnKey key{params, ret};
  auto [it, inserted] = function_map_.try_emplace(key, nullptr);
  if (inserted) {
    it->second = arena_.alloc<TypeFunction>(std::move(params), ret);
  }
  return it->second;
}

auto TypeContext::named_type(const Decl* decl_id,
                             std::string_view name,
                             std::vector<const Type*> type_args) -> const TypeNamed* {
  NamedKey key{decl_id, type_args};
  auto [it, inserted] = named_map_.try_emplace(key, nullptr);
  if (inserted) {
    it->second = arena_.alloc<TypeNamed>(decl_id, name, std::move(type_args));
  }
  return it->second;
}

auto TypeContext::generic_param(const Decl* binder, std::string_view name, uint32_t index)
    -> const TypeGenericParam* {
  GenericParamKey key{binder, index};
  auto [it, inserted] = generic_param_map_.try_emplace(key, nullptr);
  if (inserted) {
    it->second = arena_.alloc<TypeGenericParam>(binder, name, index);
  }
  return it->second;
}

auto TypeContext::generator_type(const Type* yield_type) -> const TypeGenerator* {
  auto [it, inserted] = generator_map_.try_emplace(yield_type, nullptr);
  if (inserted) {
    it->second = arena_.alloc<TypeGenerator>(yield_type);
  }
  return it->second;
}

// ---------------------------------------------------------------------------
// Nominal constructors
// ---------------------------------------------------------------------------

auto TypeContext::make_struct(const Decl* decl_id,
                              std::string_view name,
                              std::vector<StructField> fields,
                              std::vector<const Type*> type_args) -> const TypeStruct* {
  return arena_.alloc<TypeStruct>(decl_id, name, std::move(fields), std::move(type_args));
}

auto TypeContext::make_struct_shell(const Decl* decl_id, std::string_view name) -> TypeStruct* {
  return arena_.alloc<TypeStruct>(decl_id, name, std::vector<StructField>{});
}

auto TypeContext::make_enum_shell(const Decl* decl_id, std::string_view name) -> TypeEnum* {
  return arena_.alloc<TypeEnum>(decl_id, name, std::vector<EnumVariant>{});
}

auto TypeContext::make_enum(const Decl* decl_id,
                            std::string_view name,
                            std::vector<EnumVariant> variants,
                            std::vector<const Type*> type_args) -> const TypeEnum* {
  return arena_.alloc<TypeEnum>(decl_id, name, std::move(variants), std::move(type_args));
}

} // namespace dao
