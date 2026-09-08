#include "frontend/types/type_identity.h"

#include "frontend/types/type_printer.h"

#include <format>
#include <vector>

namespace dao {

namespace {

/// Nominal types currently being keyed, innermost last.  A type that
/// reappears is written as a back-reference to its position, which
/// terminates the walk for a type that points at itself.
using InProgress = std::vector<const Type*>;

void append_key(std::string& out, const Type* type, InProgress& open);

void append_nominal(std::string& out, const Type* type, InProgress& open, char tag,
                    const Decl* decl_id, const std::vector<const Type*>& children) {
  for (size_t i = 0; i < open.size(); ++i) {
    if (open[i] == type) {
      out += std::format("^{}", i);
      return;
    }
  }
  open.push_back(type);
  out += std::format("{}{:x}(", tag, reinterpret_cast<uintptr_t>(decl_id));
  for (const auto* child : children) {
    append_key(out, child, open);
    out += ',';
  }
  out += ')';
  open.pop_back();
}

void append_key(std::string& out, const Type* type, InProgress& open) {
  if (type == nullptr) {
    out += '?';
    return;
  }
  switch (type->kind()) {
  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(type);
    std::vector<const Type*> fields;
    fields.reserve(st->fields().size());
    for (const auto& field : st->fields()) {
      fields.push_back(field.type);
    }
    append_nominal(out, type, open, 'S', st->decl_id(), fields);
    break;
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    std::vector<const Type*> payloads;
    for (const auto& variant : en->variants()) {
      for (const auto* payload : variant.payload_types) {
        payloads.push_back(payload);
      }
    }
    append_nominal(out, type, open, 'E', en->decl_id(), payloads);
    break;
  }
  case TypeKind::Pointer:
    out += '*';
    append_key(out, static_cast<const TypePointer*>(type)->pointee(), open);
    break;
  case TypeKind::Generator:
    out += 'G';
    append_key(out, static_cast<const TypeGenerator*>(type)->yield_type(), open);
    break;
  case TypeKind::Function: {
    const auto* fn = static_cast<const TypeFunction*>(type);
    out += "F(";
    for (const auto* param : fn->param_types()) {
      append_key(out, param, open);
      out += ',';
    }
    out += ")";
    append_key(out, fn->return_type(), open);
    break;
  }
  case TypeKind::GenericParam: {
    const auto* param = static_cast<const TypeGenericParam*>(type);
    out += std::format("P{:x}:{}", reinterpret_cast<uintptr_t>(param->binder()), param->index());
    break;
  }
  default:
    // Builtins, string, void: interned, so their printed form is all
    // the identity they have.
    out += std::format("B{}:{}", static_cast<int>(type->kind()), print_type(type));
    break;
  }
}

} // namespace

auto type_identity_key(const Type* type) -> std::string {
  std::string key;
  InProgress open;
  append_key(key, type, open);
  return key;
}

} // namespace dao
