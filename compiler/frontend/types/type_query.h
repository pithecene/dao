#ifndef DAO_FRONTEND_TYPES_TYPE_QUERY_H
#define DAO_FRONTEND_TYPES_TYPE_QUERY_H

#include "frontend/types/type.h"

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace dao {

/// A type parameter: the declaration that declares it and its position
/// there.  Two declarations both have a first parameter — a method's
/// `U` and its class's `T` — so anything that binds parameters keys
/// them this way rather than by position, in the checker and in MIR
/// alike.
struct ParamKey {
  const Decl* binder = nullptr;
  std::uint32_t index = 0;
  auto operator==(const ParamKey& other) const -> bool = default;
};

struct ParamKeyHash {
  auto operator()(const ParamKey& key) const -> size_t {
    return std::hash<const void*>{}(key.binder) ^ (std::hash<std::uint32_t>{}(key.index) << 1U);
  }
};

/// The key of the parameter a type is.
[[nodiscard]] inline auto param_key(const TypeGenericParam* param) -> ParamKey {
  return ParamKey{param->binder(), param->index()};
}

/// Whether a type parameter is reachable anywhere in the type: the type
/// itself, a pointee, a yield type, a parameter or return type, a
/// field, a payload, or the arguments a class or enum was instantiated
/// with — a parameter no field mentions (`class Tag<T>: text: string`)
/// is reachable only there.
///
/// This is what tells a template from a lowered function, so MIR keeps
/// one answer: a function whose signature mentions a parameter is a
/// template to specialize, and anything left over after monomorphization
/// is a bug rather than code to lower.  A type that reaches itself is
/// walked once.
[[nodiscard]] auto type_mentions_generic_param(const Type* type) -> bool;

/// The same question, asked of many types that hold one another.
///
/// A substitution asks it at every class and enum it reaches, and the
/// types it asks about are the same ones over and over: `Box<Box<…<X>>>`
/// would be walked from each level down, once per level.  Remembering
/// what was answered makes the whole walk one pass.
///
/// An answer is remembered only when the walk that produced it saw the
/// whole of what the type holds.  A type that reaches BACK into one
/// still being walked is cut there, and what was cut may yet hold a
/// parameter, so nothing is remembered for it.  The answers hold while
/// the types do: a walk owns its own, and a type whose fields are
/// filled in later is asked again.
class GenericParamReach {
public:
  [[nodiscard]] auto mentions(const Type* type) -> bool;

  /// Forget everything answered.  A type's fields are filled in after it
  /// is made — a class registered before its own body is read, a shell
  /// completed once the walk that reaches back into it is done — so an
  /// answer holds for one walk and is dropped with it.
  void forget() {
    answered_.clear();
  }

private:
  std::unordered_map<const Type*, bool> answered_;
};

/// Whether a parameter that `binder` declares is reachable in the type.
/// A type argument written at a call may be written WITH the enclosing
/// class's parameters (`Box::identity<Ptr<T>>(p)`), and such a `T` names
/// that one type: it is no slot the call may fill.
[[nodiscard]] auto type_mentions_param_of(const Type* type, const Decl* binder) -> bool;

} // namespace dao

#endif // DAO_FRONTEND_TYPES_TYPE_QUERY_H
