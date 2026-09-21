#ifndef DAO_FRONTEND_TYPES_TYPE_CONTEXT_H
#define DAO_FRONTEND_TYPES_TYPE_CONTEXT_H

#include "frontend/types/type.h"
#include "support/arena.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// TypeContext — arena owner and interning hub for semantic types.
// ---------------------------------------------------------------------------

class TypeContext {
public:
  TypeContext();
  ~TypeContext();

  TypeContext(const TypeContext&) = delete;
  auto operator=(const TypeContext&) -> TypeContext& = delete;
  TypeContext(TypeContext&&) noexcept;
  auto operator=(TypeContext&&) noexcept -> TypeContext&;

  // --- Builtin access (pre-populated, never null) ---

  auto builtin(BuiltinKind kind) -> const TypeBuiltin*;

  auto i8() -> const TypeBuiltin* {
    return builtin(BuiltinKind::I8);
  }
  auto i16() -> const TypeBuiltin* {
    return builtin(BuiltinKind::I16);
  }
  auto i32() -> const TypeBuiltin* {
    return builtin(BuiltinKind::I32);
  }
  auto i64() -> const TypeBuiltin* {
    return builtin(BuiltinKind::I64);
  }
  auto u8() -> const TypeBuiltin* {
    return builtin(BuiltinKind::U8);
  }
  auto u16() -> const TypeBuiltin* {
    return builtin(BuiltinKind::U16);
  }
  auto u32() -> const TypeBuiltin* {
    return builtin(BuiltinKind::U32);
  }
  auto u64() -> const TypeBuiltin* {
    return builtin(BuiltinKind::U64);
  }
  auto f32() -> const TypeBuiltin* {
    return builtin(BuiltinKind::F32);
  }
  auto f64() -> const TypeBuiltin* {
    return builtin(BuiltinKind::F64);
  }
  auto bool_type() -> const TypeBuiltin* {
    return builtin(BuiltinKind::Bool);
  }

  // Void is a compiler-internal return type, not a builtin scalar.
  // See CONTRACT_TYPE_SYSTEM_FOUNDATIONS.md §5.
  auto void_type() -> const TypeVoid*;

  // --- Interned constructors (return canonical pointer) ---

  auto pointer_to(const Type* pointee) -> const TypePointer*;

  auto function_type(std::vector<const Type*> params, const Type* ret) -> const TypeFunction*;

  auto named_type(const Decl* decl_id, std::string_view name, std::vector<const Type*> type_args)
      -> const TypeNamed*;

  auto generic_param(const Decl* binder, std::string_view name, uint32_t index)
      -> const TypeGenericParam*;

  auto generator_type(const Type* yield_type) -> const TypeGenerator*;

  // --- Nominal constructors (not interned — each call allocates) ---

  auto make_struct(const Decl* decl_id, std::string_view name, std::vector<StructField> fields,
                   std::vector<const Type*> type_args = {}) -> const TypeStruct*;

  // Allocate a struct type shell with empty fields. Returns a mutable
  // pointer so the caller can call set_fields() once all type shells
  // are registered (enabling forward references between classes).
  auto make_struct_shell(const Decl* decl_id, std::string_view name) -> TypeStruct*;

  auto make_enum(const Decl* decl_id, std::string_view name, std::vector<EnumVariant> variants,
                 std::vector<const Type*> type_args = {}) -> const TypeEnum*;

  // An enum type shell with no variants yet, for an instantiation whose
  // variants are filled once the walk that reaches back into it is done.
  auto make_enum_shell(const Decl* decl_id, std::string_view name) -> TypeEnum*;

private:
  Arena arena_;

  // --- Interning maps ---

  std::array<const TypeBuiltin*, kBuiltinKindCount> builtins_{};
  const TypeVoid* void_ = nullptr;

  std::unordered_map<const Type*, const TypePointer*> pointer_map_;

  // Function type key: hash of (param types..., return type).
  struct FnKey {
    std::vector<const Type*> params;
    const Type* ret;

    auto operator==(const FnKey& other) const -> bool = default;
  };

  struct FnKeyHash {
    auto operator()(const FnKey& key) const -> size_t;
  };

  std::unordered_map<FnKey, const TypeFunction*, FnKeyHash> function_map_;

  // Named type key: (decl_id, type_args...).
  struct NamedKey {
    const Decl* decl_id;
    std::vector<const Type*> type_args;

    auto operator==(const NamedKey& other) const -> bool = default;
  };

  struct NamedKeyHash {
    auto operator()(const NamedKey& key) const -> size_t;
  };

  std::unordered_map<NamedKey, const TypeNamed*, NamedKeyHash> named_map_;

  // Generic param key: (binder, index). The binder pointer
  // distinguishes T@0 from fn f<T> vs T@0 from fn g<T>.
  struct GenericParamKey {
    const Decl* binder;
    uint32_t index;

    auto operator==(const GenericParamKey& other) const -> bool = default;
  };

  struct GenericParamKeyHash {
    auto operator()(const GenericParamKey& key) const -> size_t;
  };

  std::unordered_map<GenericParamKey, const TypeGenericParam*, GenericParamKeyHash>
      generic_param_map_;

  std::unordered_map<const Type*, const TypeGenerator*> generator_map_;
};

} // namespace dao

#endif // DAO_FRONTEND_TYPES_TYPE_CONTEXT_H
