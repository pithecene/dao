#include "analysis/completion.h"
#include "frontend/module/program.h"

#include "frontend/resolve/symbol.h"
#include "frontend/types/type_printer.h"

namespace dao {

namespace {

auto symbol_kind_label(SymbolKind kind) -> const char* {
  switch (kind) {
  case SymbolKind::Function:    return "function";
  case SymbolKind::Type:        return "type";
  case SymbolKind::Param:       return "parameter";
  case SymbolKind::Local:       return "variable";
  case SymbolKind::Field:       return "field";
  case SymbolKind::Module:      return "module";
  case SymbolKind::Builtin:     return "type";
  case SymbolKind::Predeclared: return "type";
  case SymbolKind::LambdaParam: return "parameter";
  case SymbolKind::GenericParam:return "type_parameter";
  case SymbolKind::Concept:     return "concept";
  }
  return "unknown";
}

auto resolve_symbol_type(const Symbol* sym, const TypeCheckResult& typed)
    -> std::string {
  // Functions and types — decl type.
  if (sym->decl != nullptr &&
      (sym->kind == SymbolKind::Function ||
       sym->kind == SymbolKind::Type ||
       sym->kind == SymbolKind::Concept)) {
    const auto* decl = sym->decl_as_decl();
    const auto* decl_type = typed.typed.decl_type(decl);
    if (decl_type != nullptr) {
      return print_type(decl_type);
    }
  }

  // Parameters — extract from enclosing function type.
  if (sym->kind == SymbolKind::Param && sym->decl != nullptr) {
    const auto* fn_decl = sym->decl_as_decl();
    const auto* fn_type = typed.typed.decl_type(fn_decl);
    if (fn_type != nullptr && fn_type->kind() == TypeKind::Function) {
      const auto* func_type = static_cast<const TypeFunction*>(fn_type);
      if (fn_decl->is<FunctionDecl>()) {
        const auto& func = fn_decl->as<FunctionDecl>();
        for (size_t idx = 0; idx < func.params.size(); ++idx) {
          if (func.params[idx].name == sym->name &&
              idx < func_type->param_types().size()) {
            return print_type(func_type->param_types()[idx]);
          }
        }
      }
    }
  }

  // Local variables — stmt type.
  if (sym->kind == SymbolKind::Local && sym->decl != nullptr) {
    const auto* stmt = sym->decl_as_stmt();
    const auto* local_type = typed.typed.local_type(stmt);
    if (local_type != nullptr) {
      return print_type(local_type);
    }
  }

  // Builtins and predeclared — name is the type.
  if (sym->kind == SymbolKind::Builtin ||
      sym->kind == SymbolKind::Predeclared) {
    return std::string(sym->name);
  }

  return {};
}

} // namespace

auto query_completions(uint32_t offset,
                        const ResolveResult& resolve,
                        const TypeCheckResult& typed)
    -> std::vector<CompletionItem> {
  // Find the innermost scope containing the cursor.
  auto* scope = resolve.context.scope_at_offset(offset);
  if (scope == nullptr) {
    return {};
  }

  // Collect all visible symbols from the scope chain.
  auto visible = scope->all_visible_symbols();

  std::vector<CompletionItem> items;
  items.reserve(visible.size());

  for (const auto* sym : visible) {
    // Skip symbols declared after the cursor position (not yet visible).
    if (sym->kind == SymbolKind::Local &&
        sym->decl_span.offset > offset) {
      continue;
    }

    // Skip internal runtime hooks — not user-facing.
    if (sym->name.starts_with("__")) {
      continue;
    }

    items.push_back({
        .label = std::string(sym->name),
        .kind = symbol_kind_label(sym->kind),
        .type = resolve_symbol_type(sym, typed),
    });
  }

  return items;
}

auto query_dot_completions(const Type* receiver_type,
                           const TypeCheckResult& typed,
                           const ModuleInfo* from_module) -> std::vector<CompletionItem> {
  std::vector<CompletionItem> items;

  if (receiver_type == nullptr) {
    return items;
  }

  // Unwrap pointer for auto-deref: (*Point).| → Point fields.
  const Type* base_type = receiver_type;
  if (base_type->kind() == TypeKind::Pointer) {
    base_type = static_cast<const TypePointer*>(base_type)->pointee();
  }

  // Struct fields.
  if (base_type->kind() == TypeKind::Struct) {
    const auto* struct_type = static_cast<const TypeStruct*>(base_type);
    for (const auto& field : struct_type->fields()) {
      items.push_back({
          .label = std::string(field.name),
          .kind = "field",
          .type = print_type(field.type),
      });
    }
  }

  // Methods from concept extends (exported method table), less those an
  // `extend` in another module introduced: offering one would advertise
  // a call the checker rejects.
  for (const auto& method : typed.methods) {
    if (method.receiver_type == base_type &&
        extend_visible_from(method.extend_module, from_module)) {
      items.push_back({
          .label = std::string(method.method_name),
          .kind = "method",
          .type = print_type(method.method_type),
      });
    }
  }

  return items;
}

} // namespace dao
