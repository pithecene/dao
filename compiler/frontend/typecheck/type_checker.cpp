#include "frontend/typecheck/type_checker.h"

#include "frontend/module/module_graph.h"

#include "frontend/typecheck/type_conversion.h"

namespace dao {

namespace {
/// The program offset of segment `i` of a qualified path, from the spans
/// the parser recorded; a path built by hand carries none, in which
/// case the segments are assumed to abut their `::` separators.
auto segment_offset(const std::vector<std::string_view>& segments,
                    const std::vector<Span>& spans,
                    Span whole,
                    size_t i) -> uint32_t {
  if (i < spans.size()) {
    return spans[i].offset;
  }
  uint32_t offset = whole.offset;
  for (size_t k = 0; k < i; ++k) {
    offset += static_cast<uint32_t>(segments[k].size()) + 2;
  }
  return offset;
}
} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TypeChecker::TypeChecker(TypeContext& types, const ResolveResult& resolve)
    : types_(types), resolve_(resolve) {
  // Build decl_span.offset -> Symbol* map for declaration-site lookups.
  for (const auto& sym : resolve_.context.symbols()) {
    if (sym->decl_span.length > 0) {
      decl_symbols_[sym->decl_span.offset] = sym.get();
    }
  }
}

namespace {

/// `a::b::c` for diagnostics.
auto qualified_path_text(const std::vector<std::string_view>& segments) -> std::string {
  std::string text;
  for (auto segment : segments) {
    text += (text.empty() ? "" : "::") + std::string(segment);
  }
  return text;
}

} // namespace

// ---------------------------------------------------------------------------
// Top-level entry
// ---------------------------------------------------------------------------

auto TypeChecker::check(std::span<const FileNode* const> files) -> TypeCheckResult {
  all_decls_.clear();
  decl_module_.clear();
  for (const auto* file : files) {
    auto module_it = file_modules_.find(file);
    const auto* module = module_it == file_modules_.end() ? nullptr : module_it->second;
    for (const auto* decl : file->declarations) {
      all_decls_.push_back(decl);
      if (module != nullptr) {
        decl_module_.emplace(decl, module);
      }
    }
  }

  // Pass 1: register all top-level declaration types.
  register_declarations();

  // Pass 1c: compute derived conformances for all classes.
  compute_derived_conformances();

  // Pass 1d: build pre-computed method lookup table.
  build_method_table();

  // Pass 2: check all declaration bodies.
  for (const auto* decl : all_decls_) {
    check_declaration(decl);
  }

  // Export method table for tooling (completion, hover).
  std::vector<MethodInfo> methods;
  for (const auto& [key, entries] : method_table_) {
    for (const auto& entry : entries) {
      methods.push_back({key.type, key.name, entry.fn_type, entry.extend_module});
    }
  }

  return {.typed = std::move(typed_),
          .diagnostics = std::move(diagnostics_),
          .methods = std::move(methods)};
}

// ---------------------------------------------------------------------------
// TypeNode -> Type* bridge
// ---------------------------------------------------------------------------

auto TypeChecker::instantiate_generic(const Type* base_type, std::string_view name,
                                      const std::vector<GenericParam>& type_params,
                                      const std::vector<TypeNode*>& type_args, Span span)
    -> const Type* {
  if (type_params.empty()) {
    if (!type_args.empty()) {
      error(span, "'" + std::string(name) + "' is not generic but was given type arguments");
      return nullptr;
    }
    return base_type;
  }

  if (type_args.empty()) {
    error(span, "generic type '" + std::string(name) + "' requires " +
                    std::to_string(type_params.size()) + " type argument(s)");
    return nullptr;
  }
  if (type_args.size() != type_params.size()) {
    error(span, "'" + std::string(name) + "' expects " + std::to_string(type_params.size()) +
                    " type argument(s), got " + std::to_string(type_args.size()));
    return nullptr;
  }
  std::unordered_map<uint32_t, const Type*> bindings;
  bool valid = true;
  for (size_t i = 0; i < type_args.size(); ++i) {
    const auto* arg_type = resolve_type_node(type_args[i]);
    if (arg_type == nullptr) {
      valid = false;
    } else {
      bindings[static_cast<uint32_t>(i)] = arg_type;
    }
  }
  if (valid) {
    return substitute_generics(base_type, bindings);
  }
  return nullptr;
}

/// Null when a symbol of this kind may name a type; otherwise what to
/// call it in a diagnostic.  Named types, aliases, builtins, generic
/// parameters, and concepts are all legitimate in a type position;
/// these kinds never are.
auto not_a_type(SymbolKind kind) -> const char* {
  switch (kind) {
  case SymbolKind::Function:
    return "a function";
  case SymbolKind::Param:
  case SymbolKind::Local:
  case SymbolKind::LambdaParam:
    return "a value";
  case SymbolKind::Field:
    return "a field";
  case SymbolKind::Module:
    return "a module";
  default:
    return nullptr;
  }
}

auto TypeChecker::resolve_type_node(const TypeNode* node) -> const Type* {
  if (node == nullptr) {
    return nullptr;
  }

  switch (node->kind()) {
  case NodeKind::NamedType: {
    const auto& named = node->as<NamedType>();
    const auto& path = named.name;
    if (path.segments.size() > 2) {
      // Through an import binding the resolver has already rejected the
      // path; only a head that is not a binding is the checker's to say.
      if (!resolver_owns_path(node)) {
        error(node->span,
              "'" + qualified_path_text(path.segments) +
                  "': a type path through an import binding has one more "
                  "segment (imports bind one segment)");
      }
      return nullptr;
    }
    auto name = path.segments.back();

    if (path.segments.size() == 1) {
      // Check builtin scalars.
      auto builtin = builtin_kind_from_name(name);
      if (builtin.has_value()) {
        return types_.builtin(*builtin);
      }

      // Check predeclared types.
      if (name == "void") {
        return types_.void_type();
      }
      if (name == "string") {
        // string is a predeclared named type. For now, use a sentinel
        // named type with a null decl_id.
        return types_.named_type(nullptr, "string", {});
      }

      // Generator<T> — compiler-provided coroutine type.
      if (name == "Generator") {
        if (named.type_args.size() != 1) {
          error(node->span, "Generator requires exactly one type argument");
          return nullptr;
        }
        const auto* yield_type = resolve_type_node(named.type_args[0]);
        if (yield_type == nullptr) {
          return nullptr;
        }
        return types_.generator_type(yield_type);
      }
    }

    // Look up user-defined types via resolver symbols: a plain name at
    // its own offset, `b::T` at T's offset where the resolver recorded
    // the export.
    auto symbol_offset = path.segments.size() == 1
                             ? node->span.offset
                             : segment_offset(path.segments, path.segment_spans, path.span, 1);
    auto it = resolve_.uses.find(symbol_offset);
    if (it != resolve_.uses.end()) {
      const auto* sym = it->second;
      // Generic type parameters resolve to TypeGenericParam.
      if (sym->kind == SymbolKind::GenericParam) {
        // Find the parameter index from the enclosing declaration.
        uint32_t index = find_generic_param_index(sym);
        return types_.generic_param(sym->decl_as_decl(), sym->name, index);
      }
      // Concept name in type position: substitute the conforming type
      // when inside a context that has set concept_self_map_ (§3.2).
      if (sym->kind == SymbolKind::Concept && sym->decl != nullptr) {
        auto csm = concept_self_map_.find(sym->decl_as_decl());
        if (csm != concept_self_map_.end()) {
          return csm->second;
        }
        // Anywhere else a concept is not a type: it constrains a type
        // parameter (`<T: Reveal>`), it does not stand for one.
        error(node->span,
              "'" + qualified_path_text(path.segments) +
                  "' is a concept, not a type; use it as a bound (<T: " + std::string(name) + ">)");
        return nullptr;
      }
      // A type position takes a type.  Without this, a function symbol
      // yields its own function type and a value symbol its value type,
      // so `p: helper` or `p: lib::helper` typechecks silently as
      // whatever the name happens to denote
      // (CONTRACT_TYPE_SYSTEM_FOUNDATIONS.md §11).
      if (const auto* what = not_a_type(sym->kind)) {
        error(node->span, "'" + module_display(path.segments) + "' is " + what + ", not a type");
        return nullptr;
      }
      const auto* base_type = resolve_symbol_type(sym);

      // If the AST node carries type arguments (e.g. Vector<i32>),
      // resolve them and substitute into the base type so that
      // function parameters and return types are fully instantiated.
      if (base_type != nullptr && base_type->kind() == TypeKind::Struct) {
        const auto* decl_node = sym->decl_as_decl();
        const std::vector<GenericParam>* type_params = nullptr;
        if (decl_node->is<ClassDecl>()) {
          type_params = &decl_node->as<ClassDecl>().type_params;
        }

        if (type_params != nullptr) {
          return instantiate_generic(base_type, name, *type_params, named.type_args, node->span);
        }
      }

      // Generic enum instantiation: Option<i32>, Result<i64, string>, etc.
      if (base_type != nullptr && base_type->kind() == TypeKind::Enum) {
        const auto* decl_node = sym->decl_as_decl();
        const std::vector<GenericParam>* type_params = nullptr;
        if (decl_node->is<EnumDeclNode>()) {
          type_params = &decl_node->as<EnumDeclNode>().type_params;
        }

        if (type_params != nullptr) {
          return instantiate_generic(base_type, name, *type_params, named.type_args, node->span);
        }
      }

      return base_type;
    }

    // `b::T` through an import binding: the resolver owns that path.  If
    // `b` is an import the graph reported missing, there is no module to
    // look in; if it resolved and `T` is not among its exports, the
    // resolver has already said so by name.  Either way a second
    // diagnostic here would only restate the first.
    if (path.segments.size() > 1) {
      auto head = resolve_.uses.find(path.span.offset);
      if (head != resolve_.uses.end() && head->second->kind == SymbolKind::Module) {
        return nullptr;
      }
    }

    error(node->span, "unknown type '" + std::string(name) + "'");
    return nullptr;
  }

  case NodeKind::PointerType: {
    const auto& ptr = node->as<PointerType>();
    const auto* pointee = resolve_type_node(ptr.pointee);
    if (pointee == nullptr) {
      return nullptr;
    }
    return types_.pointer_to(pointee);
  }

  case NodeKind::FunctionType: {
    const auto& ftn = node->as<FunctionTypeNode>();
    std::vector<const Type*> param_types;
    param_types.reserve(ftn.param_types.size());
    for (const auto* pt : ftn.param_types) {
      const auto* resolved = resolve_type_node(pt);
      if (resolved == nullptr) {
        return nullptr;
      }
      param_types.push_back(resolved);
    }
    const auto* ret_type = resolve_type_node(ftn.return_type);
    if (ret_type == nullptr) {
      return nullptr;
    }
    return types_.function_type(std::move(param_types), ret_type);
  }

  default:
    error(node->span, "unsupported type syntax");
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Symbol -> Type* bridge
// ---------------------------------------------------------------------------

auto TypeChecker::resolve_symbol_type(const Symbol* sym) -> const Type* {
  if (sym == nullptr) {
    return nullptr;
  }

  // Check cache first.
  auto it = symbol_types_.find(sym);
  if (it != symbol_types_.end()) {
    return it->second;
  }

  const Type* result = nullptr;

  switch (sym->kind) {
  case SymbolKind::Function: {
    // Function symbol -> derive TypeFunction from declaration.
    if (sym->decl == nullptr) {
      // Compiler builtin functions with no AST declaration.
      result = resolve_builtin_function_type(sym->name);
      break;
    }
    const auto& fn = sym->decl_as_decl()->as<FunctionDecl>();
    std::vector<const Type*> param_types;
    bool valid = true;
    for (const auto& param : fn.params) {
      const auto* pt = resolve_type_node(param.type);
      if (pt == nullptr) {
        valid = false;
      }
      param_types.push_back(pt);
    }
    const auto* ret =
        fn.return_type != nullptr ? resolve_type_node(fn.return_type) : types_.void_type();
    if (!valid || ret == nullptr) {
      break;
    }
    result = types_.function_type(std::move(param_types), ret);
    break;
  }

  case SymbolKind::Param: {
    // Parameter symbol -> type from its TypeNode.
    if (sym->decl == nullptr) {
      break;
    }
    // The decl for a param points to the Decl (FunctionDecl payload).
    // We need to find the matching param by name.
    const auto& fn = sym->decl_as_decl()->as<FunctionDecl>();
    for (const auto& p : fn.params) {
      if (p.name == sym->name) {
        result = resolve_type_node(p.type);
        break;
      }
    }
    break;
  }

  case SymbolKind::Local: {
    // Local type is set during let/for checking. If already cached, we
    // would have returned above. Return nullptr for now — it will be
    // populated during statement checking.
    break;
  }

  case SymbolKind::Type: {
    // Struct type — look up via decl_id.
    if (sym->decl != nullptr) {
      result = resolve_symbol_type_for_type_decl(sym);
    }
    break;
  }

  case SymbolKind::Builtin: {
    auto bk = builtin_kind_from_name(sym->name);
    if (bk.has_value()) {
      result = types_.builtin(*bk);
    }
    break;
  }

  case SymbolKind::Predeclared: {
    if (sym->name == "void") {
      result = types_.void_type();
    } else if (sym->name == "string") {
      result = types_.named_type(nullptr, "string", {});
    }
    break;
  }

  case SymbolKind::LambdaParam:
    // Lambda params are typed contextually; handled in check_lambda.
    break;

  case SymbolKind::GenericParam:
    // Generic type parameters resolve to TypeGenericParam. The index
    // is derived from the parameter's position in the declaration.
    // For now, return nullptr — generic params aren't yet type-checkable
    // as values (they are types, not values).
    break;

  case SymbolKind::Concept:
    // Concept symbols are type-level; not values.
    break;

  case SymbolKind::Field:
  case SymbolKind::Module:
    // Not yet handled.
    break;
  }

  if (result != nullptr) {
    symbol_types_[sym] = result;
  }
  return result;
}

// Helper for Type-kind symbols (structs).
auto TypeChecker::resolve_symbol_type_for_type_decl(const Symbol* sym) -> const Type* {
  // Check if it was already registered during pass 1.
  auto it = symbol_types_.find(sym);
  if (it != symbol_types_.end()) {
    return it->second;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Pass 1: register declaration types
// ---------------------------------------------------------------------------

void TypeChecker::register_declarations() {
  pending_classes_.clear(); // Reset pass-local state for this file.
  fields_registered_ = false;
  register_type_names();
  // Aliases come after the class shells they may name, and repeat until
  // a pass registers nothing new: one alias may name another to any
  // depth, in any order.  Enums are registered in between because their
  // payloads may name an alias, and aliases may in turn name an enum.
  while (register_type_aliases(/*report_failures=*/false) > 0) {
  }
  register_enum_variants();
  while (register_type_aliases(/*report_failures=*/false) > 0) {
  }
  // No failure is final yet: an alias may name one that waits for
  // fields (below), and reporting it here would reject a program that
  // resolves a pass later.
  register_struct_fields();
  fields_registered_ = true;
  // Aliases of generic instantiations were held back until the classes
  // they instantiate had fields; they resolve now, to a fixpoint as
  // before, and only now are their failures final.
  while (register_type_aliases(/*report_failures=*/false) > 0) {
  }
  register_type_aliases(/*report_failures=*/true);
  register_signatures();
}

auto TypeChecker::aliases_generic_shell(const TypeNode* node) const -> bool {
  if (fields_registered_ || node == nullptr) {
    return false;
  }
  // `*Box<i32>`, `fn(Box<i32>): i32`, `Vec<Box<i32>>`: the shell may sit
  // anywhere inside the node.
  if (node->is<PointerType>()) {
    return aliases_generic_shell(node->as<PointerType>().pointee);
  }
  if (node->is<FunctionTypeNode>()) {
    const auto& ftn = node->as<FunctionTypeNode>();
    return aliases_generic_shell(ftn.return_type) ||
           std::ranges::any_of(ftn.param_types,
                               [&](const TypeNode* pt) { return aliases_generic_shell(pt); });
  }
  if (!node->is<NamedType>()) {
    return false;
  }
  const auto& named = node->as<NamedType>();
  if (std::ranges::any_of(named.type_args,
                          [&](const TypeNode* arg) { return aliases_generic_shell(arg); })) {
    return true;
  }
  if (named.type_args.empty()) {
    return false;
  }
  const auto& path = named.name;
  auto symbol_offset = path.segments.size() == 1
                           ? node->span.offset
                           : segment_offset(path.segments, path.segment_spans, path.span, 1);
  auto it = resolve_.uses.find(symbol_offset);
  if (it == resolve_.uses.end() || it->second->kind != SymbolKind::Type) {
    return false;
  }
  const auto* decl = it->second->decl_as_decl();
  return std::ranges::any_of(pending_classes_,
                             [decl](const PendingClass& pc) { return pc.decl == decl; });
}

auto TypeChecker::resolver_owns_path(const TypeNode* node) const -> bool {
  if (node == nullptr) {
    return false;
  }
  switch (node->kind()) {
  case NodeKind::NamedType: {
    const auto& named = node->as<NamedType>();
    const auto& path = named.name;
    if (path.segments.size() >= 2) {
      auto head = resolve_.uses.find(path.span.offset);
      if (head != resolve_.uses.end() && head->second->kind == SymbolKind::Module) {
        return true;
      }
    }
    // `Vec<lib::Missing>`: the failure sits in a type argument.
    return std::ranges::any_of(named.type_args,
                               [&](const TypeNode* arg) { return resolver_owns_path(arg); });
  }
  case NodeKind::PointerType:
    return resolver_owns_path(node->as<PointerType>().pointee);
  case NodeKind::FunctionType: {
    const auto& ftn = node->as<FunctionTypeNode>();
    return resolver_owns_path(ftn.return_type) ||
           std::ranges::any_of(ftn.param_types,
                               [&](const TypeNode* pt) { return resolver_owns_path(pt); });
  }
  default:
    return false;
  }
}

auto TypeChecker::register_type_aliases(bool report_failures) -> size_t {
  size_t registered = 0;
  for (const auto* decl : all_decls_) {
    if (decl->kind() != NodeKind::AliasDecl) {
      continue;
    }
    const auto& alias = decl->as<AliasDecl>();
    auto decl_it = decl_symbols_.find(alias.name_span.offset);
    if (decl_it == decl_symbols_.end()) {
      continue;
    }
    const auto* sym = decl_it->second;
    if (symbol_types_.contains(sym)) {
      continue; // registered by an earlier run
    }

    // An alias of a generic instantiation (`type IntBox = lib::Box<i32>`)
    // cannot be resolved before `Box` has its fields: instantiating the
    // shell would cache a `Box<i32>` with no fields at all, and the
    // alias would then accept anything.  Such an alias waits for the
    // pass that runs after fields are registered.
    if (aliases_generic_shell(alias.type)) {
      continue;
    }
    // Resolve the aliased type and cache it so later lookups of the
    // alias name transparently return the underlying type.
    auto before = diagnostics_.size();
    const auto* aliased_type = resolve_type_node(alias.type);
    if (aliased_type != nullptr) {
      symbol_types_[sym] = aliased_type;
      typed_.set_decl_type(decl, aliased_type);
      ++registered;
      continue;
    }
    if (!report_failures) {
      // The target may simply not be registered yet; anything said now
      // would be said again by the final run.
      diagnostics_.resize(before);
      continue;
    }
    if (diagnostics_.size() == before && !resolver_owns_path(alias.type)) {
      // The name resolved to a symbol whose type never materialized —
      // one alias naming another that names it back, say.  Left
      // unsaid, the alias is silently unusable everywhere it appears.
      // A path through an import binding is not that case: the
      // resolver has already said what is wrong with it.
      error(alias.name_span,
            "cannot resolve the type aliased by '" + std::string(alias.name) + "'");
    }
  }
  return registered;
}

void TypeChecker::register_type_names() {
  // Pass 1b: register type shells (enums and class structs) so that
  // function signatures processed in pass 1c can reference them
  // regardless of source order.
  //
  // Sub-pass 1b-i: register class struct shells with empty fields.
  // This ensures all class names are available in symbol_types_
  // before any field types are resolved, enabling forward references
  // between classes (e.g. class Diagnostic with a Span field where
  // Span is defined later in the source).
  for (const auto* decl : all_decls_) {
    if (decl->kind() == NodeKind::ClassDecl) {
      const auto& st = decl->as<ClassDecl>();
      auto decl_it = decl_symbols_.find(st.name_span.offset);
      if (decl_it == decl_symbols_.end()) {
        continue;
      }
      const auto* sym = decl_it->second;

      auto* shell = types_.make_struct_shell(decl, st.name);
      symbol_types_[sym] = shell;
      typed_.set_decl_type(decl, shell);
      pending_classes_.push_back({&st, decl, shell});
    }
  }
}

void TypeChecker::register_enum_variants() {
  // Register enum types with resolved variant payload types.
  // Unresolved types are kept as nullptr to preserve arity — the
  // primary diagnostic comes from resolve_type_node; dropping the
  // slot would silently mutate the variant shape and produce
  // misleading secondary errors.
  for (const auto* decl : all_decls_) {
    if (decl->kind() != NodeKind::EnumDecl) {
      continue;
    }
    const auto& en = decl->as<EnumDeclNode>();
    auto decl_it = decl_symbols_.find(en.name_span.offset);
    if (decl_it == decl_symbols_.end()) {
      continue;
    }
    const auto* sym = decl_it->second;

    std::vector<EnumVariant> variants;
    for (const auto& variant : en.variants) {
      std::vector<const Type*> payload_types;
      for (size_t i = 0; i < variant.payload_types.size(); ++i) {
        const auto* resolved = resolve_type_node(variant.payload_types[i]);
        payload_types.push_back(resolved);
        if (resolved == nullptr) {
          if (variant.payload_types[i]->is<NamedType>()) {
            const auto& named = variant.payload_types[i]->as<NamedType>();
            if (named.name.segments.size() == 1 && named.name.segments[0] == en.name) {
              error(variant.payload_types[i]->span,
                    "enum '" + std::string(en.name) +
                        "' cannot contain itself by value in variant '" +
                        std::string(variant.name) + "'; use a pointer (*" + std::string(en.name) +
                        ") for recursive types");
            }
          }
        }
      }
      variants.push_back({variant.name, std::move(payload_types), variant.field_names});
    }
    const auto* enum_type = types_.make_enum(decl, en.name, std::move(variants));
    symbol_types_[sym] = enum_type;
    typed_.set_decl_type(decl, enum_type);
  }
}

void TypeChecker::register_struct_fields() {
  // Sub-pass 1b-ii: resolve class field types now that all type
  // shells (classes and enums) are registered in symbol_types_.
  // Unresolved types are kept as nullptr to preserve arity — same
  // rationale as enum variant payloads: dropping the slot silently
  // mutates the struct shape and produces misleading secondary
  // constructor-arity errors instead of the real type-resolution
  // failure.
  for (auto& pending : pending_classes_) {
    std::vector<StructField> fields;
    for (const auto* field : pending.class_decl->fields) {
      const auto* field_type = resolve_type_node(field->type);
      fields.push_back({field->name, field_type});
    }
    pending.shell->set_fields(std::move(fields));
  }
}

void TypeChecker::register_signatures() {
  // Pass 1c: register function signatures, class method signatures,
  // and extend method signatures. All type shells from pass 1b are
  // available, so return types like Option<V> resolve correctly.
  for (const auto* decl : all_decls_) {
    switch (decl->kind()) {
    case NodeKind::FunctionDecl: {
      const auto& fn = decl->as<FunctionDecl>();
      auto decl_it = decl_symbols_.find(fn.name_span.offset);
      if (decl_it == decl_symbols_.end()) {
        break;
      }
      const auto* sym = decl_it->second;

      std::vector<const Type*> param_types;
      bool valid = true;
      for (const auto& param : fn.params) {
        const auto* pt = resolve_type_node(param.type);
        if (pt == nullptr) {
          valid = false;
        }
        param_types.push_back(pt);
      }
      const auto* ret =
          fn.return_type != nullptr ? resolve_type_node(fn.return_type) : types_.void_type();
      if (valid && ret != nullptr) {
        const auto* fn_type = types_.function_type(std::move(param_types), ret);
        symbol_types_[sym] = fn_type;
        typed_.set_decl_type(decl, fn_type);
      }
      break;
    }

    case NodeKind::ClassDecl: {
      const auto& st = decl->as<ClassDecl>();
      auto decl_it = decl_symbols_.find(st.name_span.offset);
      if (decl_it == decl_symbols_.end()) {
        break;
      }
      const auto* struct_type = static_cast<const TypeStruct*>(symbol_types_[decl_it->second]);

      // Register class body methods.
      for (const auto* method : st.methods) {
        const auto& fn = method->as<FunctionDecl>();
        auto mdecl_it = decl_symbols_.find(fn.name_span.offset);
        if (mdecl_it == decl_symbols_.end()) {
          continue;
        }
        const auto* method_sym = mdecl_it->second;
        std::vector<const Type*> param_types;
        for (const auto& param : fn.params) {
          if (param.name == "self") {
            param_types.push_back(struct_type);
          } else {
            const auto* param_type = resolve_type_node(param.type);
            param_types.push_back(param_type);
          }
        }
        const auto* ret =
            fn.return_type != nullptr ? resolve_type_node(fn.return_type) : types_.void_type();
        const auto* fn_type = types_.function_type(std::move(param_types), ret);
        symbol_types_[method_sym] = fn_type;
        typed_.set_decl_type(method, fn_type);
      }
      break;
    }

    case NodeKind::ExtendDecl: {
      const auto& ext = decl->as<ExtendDecl>();
      const auto* target_type = resolve_type_node(ext.target_type);
      for (const auto* method : ext.methods) {
        const auto& method_fn = method->as<FunctionDecl>();
        std::vector<const Type*> param_types;
        bool valid = true;
        for (const auto& param : method_fn.params) {
          if (param.name == "self" && param.type == nullptr) {
            if (target_type != nullptr) {
              param_types.push_back(target_type);
            } else {
              valid = false;
              param_types.push_back(nullptr);
            }
          } else {
            const auto* param_type = resolve_type_node(param.type);
            if (param_type == nullptr) {
              valid = false;
            }
            param_types.push_back(param_type);
          }
        }
        const auto* ret = method_fn.return_type != nullptr
                              ? resolve_type_node(method_fn.return_type)
                              : types_.void_type();
        if (valid && ret != nullptr) {
          const auto* fn_type = types_.function_type(std::move(param_types), ret);
          auto fn_decl_it = decl_symbols_.find(method_fn.name_span.offset);
          if (fn_decl_it != decl_symbols_.end()) {
            symbol_types_[fn_decl_it->second] = fn_type;
          }
          typed_.set_decl_type(method, fn_type);
        }
      }
      break;
    }

    default:
      break;
    }
  }

  // Pass 1c-prep: collect derived concept declarations.
  for (const auto* decl : all_decls_) {
    if (decl->kind() == NodeKind::ConceptDecl) {
      const auto& cpt = decl->as<ConceptDecl>();
      if (cpt.is_derived) {
        derived_concepts_.push_back(decl);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Derived conformance computation
// ---------------------------------------------------------------------------

auto TypeChecker::type_conforms_to(const Type* type, const Decl* concept_decl) -> bool {
  // Check explicit conformance: inline `as` blocks on structs.
  if (type->kind() == TypeKind::Struct) {
    const auto* struct_type = static_cast<const TypeStruct*>(type);
    const auto* decl_node = struct_type->decl_id();
    if (decl_node != nullptr) {
      if (decl_node->is<ClassDecl>()) {
        const auto& cls = decl_node->as<ClassDecl>();

        // deny supersedes everything — if present, the type does not
        // conform regardless of explicit `as` blocks. (Having both
        // is a compile error diagnosed in check_class.)
        for (const auto& deny : cls.denials) {
          if (concept_named_at(deny.target.concept_span) == concept_decl) {
            return false;
          }
        }

        // Check explicit conformance.
        for (const auto& conf : cls.conformances) {
          if (concept_named_at(conf.target.concept_span) == concept_decl) {
            return true;
          }
        }
      }
    }
  }

  // Check extend declarations.
  if (!all_decls_.empty()) {
    for (const auto* decl : all_decls_) {
      if (decl->kind() != NodeKind::ExtendDecl) {
        continue;
      }
      const auto& ext = decl->as<ExtendDecl>();
      // Conformance introduced by `extend` is scoped like the methods it
      // introduces (CONTRACT_MODULE_SYSTEM.md §5): a type does not
      // satisfy a bound on the strength of an extension another module
      // declared.
      if (!extend_is_visible(declaring_module(decl))) {
        continue;
      }
      const auto* target = resolve_type_node(ext.target_type);
      if (target == type && concept_named_at(ext.target.concept_span) == concept_decl) {
        return true;
      }
    }
  }

  // Check derived conformance (already computed).
  auto it = derived_conformances_.find(type);
  if (it != derived_conformances_.end()) {
    for (const auto* derived : it->second) {
      if (derived == concept_decl) {
        return true;
      }
    }
  }

  return false;
}

void TypeChecker::compute_derived_conformances() {
  if (derived_concepts_.empty()) {
    return;
  }

  // Collect class declarations once for iteration.
  struct ClassEntry {
    const Decl* decl;
    const ClassDecl* cls;
    const Type* struct_type;
  };
  std::vector<ClassEntry> classes;
  for (const auto* decl : all_decls_) {
    if (decl->kind() != NodeKind::ClassDecl) {
      continue;
    }
    const auto& cls = decl->as<ClassDecl>();
    auto decl_it = decl_symbols_.find(cls.name_span.offset);
    if (decl_it == decl_symbols_.end()) {
      continue;
    }
    const auto* struct_type = resolve_symbol_type(decl_it->second);
    if (struct_type == nullptr || struct_type->kind() != TypeKind::Struct) {
      continue;
    }
    classes.push_back({decl, &cls, struct_type});
  }

  // Fixpoint loop: repeat until no new conformances are discovered.
  // This ensures transitive derivation is independent of declaration order.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& entry : classes) {
      // Derivation asks whether the fields conform, and that question is
      // asked FROM the class's module: an `extend` of its own module is
      // in scope, another module's is not (§5).  Without this the whole
      // pass ran with no current module, which made every non-prelude
      // extension invisible — including the class's own.
      current_module_ = declaring_module(entry.decl);
      for (const auto* concept_decl : derived_concepts_) {
        // Explicit conformance or denial of THIS concept — by identity,
        // since two modules may each declare one named the same.
        bool has_explicit = false;
        for (const auto& conf : entry.cls->conformances) {
          if (concept_named_at(conf.target.concept_span) == concept_decl) {
            has_explicit = true;
            break;
          }
        }
        if (has_explicit) {
          continue;
        }

        bool denied = false;
        for (const auto& deny : entry.cls->denials) {
          if (concept_named_at(deny.target.concept_span) == concept_decl) {
            denied = true;
            break;
          }
        }
        if (denied) {
          continue;
        }

        // Skip if already derived.
        auto existing_it = derived_conformances_.find(entry.struct_type);
        if (existing_it != derived_conformances_.end()) {
          bool already_derived = false;
          for (const auto* existing : existing_it->second) {
            if (existing == concept_decl) {
              already_derived = true;
              break;
            }
          }
          if (already_derived) {
            continue;
          }
        }

        // Structural check: all fields must conform to this concept.
        const auto* stype = static_cast<const TypeStruct*>(entry.struct_type);
        bool all_fields_conform = true;
        for (const auto& field : stype->fields()) {
          if (field.type == nullptr || !type_conforms_to(field.type, concept_decl)) {
            all_fields_conform = false;
            break;
          }
        }

        if (all_fields_conform) {
          derived_conformances_[entry.struct_type].push_back(concept_decl);
          changed = true;
        }
      }
    }
  }
  current_module_ = nullptr;
}

// ---------------------------------------------------------------------------
// Declaration checking (pass 2)
// ---------------------------------------------------------------------------

void TypeChecker::check_declaration(const Decl* decl) {
  current_module_ = declaring_module(decl);
  switch (decl->kind()) {
  case NodeKind::FunctionDecl:
    check_function(decl);
    break;
  case NodeKind::ClassDecl:
    check_class(decl);
    break;
  case NodeKind::ConceptDecl:
    // Concept default method bodies are abstract over `self`'s type.
    // Full checking requires concept-level type reasoning (deferred).
    break;
  case NodeKind::ExtendDecl: {
    // Check bodies of conformance methods with self_type set to the
    // target type of the extend declaration.
    const auto& ext = decl->as<ExtendDecl>();
    auto saved_ctx = ctx_;
    ctx_.self_type = resolve_type_node(ext.target_type);

    // Diagnose extend targeting a type that denies the concept.
    if (ctx_.self_type != nullptr && ctx_.self_type->kind() == TypeKind::Struct) {
      const auto* st = static_cast<const TypeStruct*>(ctx_.self_type);
      const auto* dnode = st->decl_id();
      if (dnode != nullptr) {
        if (dnode->is<ClassDecl>()) {
          for (const auto& deny : dnode->as<ClassDecl>().denials) {
            const auto* denied_concept = concept_named_at(deny.target.concept_span);
            if (denied_concept != nullptr &&
                denied_concept == concept_named_at(ext.target.concept_span)) {
              error(ext.target.concept_span,
                    "cannot extend '" + std::string(st->name()) + "' as '" +
                        std::string(ext.target.concept_name) + "' because the type denies it");
            }
          }
        }
      }
    }
    for (const auto* method : ext.methods) {
      validate_receiver(method, ext.target.concept_span);
      const auto& fn = method->as<FunctionDecl>();
      if (!fn.body.empty() || fn.expr_body != nullptr) {
        check_function(method);
      }
    }
    ctx_ = saved_ctx;
    break;
  }
  default:
    // AliasDecl — no body checking needed yet.
    break;
  }
}

void TypeChecker::check_function(const Decl* decl) {
  const auto& fn = decl->as<FunctionDecl>();

  // Determine return type.
  const auto* ret_type =
      fn.return_type != nullptr ? resolve_type_node(fn.return_type) : types_.void_type();

  // Validate extern fn ABI types (user declarations only, not __dao_* hooks).
  // Validate extern fn ABI types for user declarations.
  // Reserved-prefix names (__) are exempt — this covers __dao_*
  // runtime hooks (Dao-defined ABI per CONTRACT_RUNTIME_ABI) and
  // aligns with C's reserved identifier convention.
  if (fn.is_extern && !fn.name.starts_with("__")) {
    for (const auto& param : fn.params) {
      if (param.type != nullptr) {
        const auto* param_type = resolve_type_node(param.type);
        if (param_type != nullptr && !is_c_abi_compatible(param_type)) {
          error(param.type->span,
                "extern fn parameter type '" + print_type(param_type) +
                    "' is not supported at the C ABI boundary");
        }
      }
    }
    if (ret_type != nullptr && ret_type->kind() != TypeKind::Void &&
        !is_c_abi_compatible(ret_type)) {
      error(fn.return_type->span,
            "extern fn return type '" + print_type(ret_type) +
                "' is not supported at the C ABI boundary");
    }
  }

  // Function types are allowed in all function signatures. The backend
  // supports indirect calls through function-typed values.

  // Set up param types in symbol cache.
  for (const auto& param : fn.params) {
    auto decl_it = decl_symbols_.find(param.name_span.offset);
    if (decl_it == decl_symbols_.end()) {
      continue;
    }
    if (param.type != nullptr) {
      const auto* pt = resolve_type_node(param.type);
      if (pt != nullptr) {
        symbol_types_[decl_it->second] = pt;
      }
    } else if (param.name == "self" && ctx_.self_type != nullptr) {
      // Bare `self` receiver — type comes from the enclosing class/extend.
      symbol_types_[decl_it->second] = ctx_.self_type;
    }
  }

  // Save and set context.
  auto saved_ctx = ctx_;
  ctx_.return_type = ret_type;

  if (fn.is_expr_bodied()) {
    // Expression-bodied: -> expr
    const auto* expr_type = check_expr(fn.expr_body);
    if (expr_type != nullptr && ret_type != nullptr && !is_assignable(expr_type, ret_type)) {
      error(fn.expr_body->span,
            "expression body type '" + print_type(expr_type) + "' does not match return type '" +
                print_type(ret_type) + "'");
    }
  } else {
    // Block-bodied: check statements.
    check_body(fn.body);
  }

  ctx_ = saved_ctx;
}

void TypeChecker::check_class(const Decl* decl) {
  const auto& cls = decl->as<ClassDecl>();

  // Look up the struct type registered in pass 1.
  auto saved_ctx = ctx_;
  auto decl_it = decl_symbols_.find(cls.name_span.offset);
  if (decl_it != decl_symbols_.end()) {
    ctx_.self_type = resolve_symbol_type(decl_it->second);
  }

  // Diagnose conflicting as + deny for the same concept — the same one,
  // by declaration identity: `as a::C` alongside `deny b::C` names two
  // concepts and is not a contradiction.
  for (const auto& deny : cls.denials) {
    const auto* denied_concept = concept_named_at(deny.target.concept_span);
    for (const auto& conf : cls.conformances) {
      if (denied_concept != nullptr &&
          denied_concept == concept_named_at(conf.target.concept_span)) {
        error(deny.target.concept_span,
              "'" + std::string(cls.name) + "' both conforms to and denies '" +
                  std::string(deny.target.concept_name) + "'");
      }
    }
  }

  // Validate and check direct class methods.
  // Static methods (no self parameter) are allowed in class bodies.
  for (const auto* method : cls.methods) {
    const auto& fn = method->as<FunctionDecl>();
    if (!fn.params.empty() && fn.params[0].name == "self") {
      validate_receiver(method, cls.name_span);
    }
    if (!fn.body.empty() || fn.expr_body != nullptr) {
      check_function(method);
    }
  }

  // Validate and check conformance-block methods.
  for (const auto& conf : cls.conformances) {
    for (const auto* method : conf.methods) {
      validate_receiver(method, conf.target.concept_span);
      const auto& fn = method->as<FunctionDecl>();
      if (!fn.body.empty() || fn.expr_body != nullptr) {
        check_function(method);
      }
    }
  }

  ctx_ = saved_ctx;
}

// ---------------------------------------------------------------------------
// Statement checking
// ---------------------------------------------------------------------------

void TypeChecker::check_body(const std::vector<Stmt*>& body) {
  for (const auto* stmt : body) {
    check_statement(stmt);
  }
}

void TypeChecker::check_statement(const Stmt* stmt) {
  switch (stmt->kind()) {
  case NodeKind::LetStatement:
    check_let(stmt);
    break;
  case NodeKind::Assignment:
    check_assignment(stmt);
    break;
  case NodeKind::IfStatement:
    check_if(stmt);
    break;
  case NodeKind::WhileStatement:
    check_while(stmt);
    break;
  case NodeKind::ForStatement:
    check_for(stmt);
    break;
  case NodeKind::YieldStatement:
    check_yield(stmt);
    break;
  case NodeKind::BreakStatement:
    if (ctx_.loop_depth == 0) {
      error(stmt->span, "'break' is only allowed inside a loop");
    }
    break;
  case NodeKind::MatchStatement:
    check_match(stmt);
    break;
  case NodeKind::ModeBlock:
    check_mode_block(stmt);
    break;
  case NodeKind::ResourceBlock:
    check_resource_block(stmt);
    break;
  case NodeKind::ReturnStatement:
    check_return(stmt);
    break;
  case NodeKind::ExpressionStatement:
    check_expr_stmt(stmt);
    break;
  default:
    break;
  }
}

void TypeChecker::check_let(const Stmt* stmt) {
  const auto& let = stmt->as<LetStatement>();

  const Type* declared_type = nullptr;
  if (let.type != nullptr) {
    declared_type = resolve_type_node(let.type);
  }

  const Type* init_type = nullptr;
  if (let.initializer != nullptr) {
    init_type = check_expr(let.initializer, declared_type);
  }

  if (declared_type != nullptr && init_type != nullptr) {
    // let x: T = expr — check assignability.
    if (!is_assignable(init_type, declared_type)) {
      error(let.initializer->span,
            "initializer type '" + print_type(init_type) + "' is not assignable to '" +
                print_type(declared_type) + "'");
    }
    typed_.set_local_type(stmt, declared_type);
  } else if (declared_type != nullptr) {
    // let x: T — type without initializer.
    typed_.set_local_type(stmt, declared_type);
  } else if (init_type != nullptr) {
    // let x = expr — infer from initializer.
    typed_.set_local_type(stmt, init_type);
  } else {
    // let x — no type, no initializer.
    error(stmt->span, "declaration without type annotation requires an initializer");
  }

  // Cache in symbol table for later identifier lookups.
  const auto* local_type = typed_.local_type(stmt);
  if (local_type != nullptr) {
    auto decl_it = decl_symbols_.find(let.name_span.offset);
    if (decl_it != decl_symbols_.end()) {
      symbol_types_[decl_it->second] = local_type;
    }
  }
}

void TypeChecker::check_assignment(const Stmt* stmt) {
  const auto& assign = stmt->as<Assignment>();

  if (!is_lvalue(assign.target)) {
    error(assign.target->span, "invalid assignment target");
  }

  const auto* target_type = check_expr(assign.target);
  const auto* value_type = check_expr(assign.value, target_type);

  if (target_type != nullptr && value_type != nullptr && !is_assignable(value_type, target_type)) {
    error(assign.value->span,
          "cannot assign '" + print_type(value_type) + "' to '" + print_type(target_type) + "'");
  }
}

void TypeChecker::check_if(const Stmt* stmt) {
  const auto& ifn = stmt->as<IfStatement>();

  const auto* cond_type = check_expr(ifn.condition);
  if (cond_type != nullptr && !is_assignable(cond_type, types_.bool_type())) {
    error(ifn.condition->span, "condition must be 'bool', got '" + print_type(cond_type) + "'");
  }
  check_body(ifn.then_body);
  if (ifn.has_else()) {
    check_body(ifn.else_body);
  }
}

void TypeChecker::check_while(const Stmt* stmt) {
  const auto& wh = stmt->as<WhileStatement>();

  const auto* cond_type = check_expr(wh.condition);
  if (cond_type != nullptr && !is_assignable(cond_type, types_.bool_type())) {
    error(wh.condition->span, "condition must be 'bool', got '" + print_type(cond_type) + "'");
  }
  ctx_.loop_depth++;
  check_body(wh.body);
  ctx_.loop_depth--;
}

void TypeChecker::check_for(const Stmt* stmt) {
  const auto& fo = stmt->as<ForStatement>();

  const auto* iter_type = check_expr(fo.iterable);

  // The iterable expression must produce Generator<T>.
  const Type* elem_type = nullptr;
  if (iter_type != nullptr) {
    if (iter_type->kind() == TypeKind::Generator) {
      elem_type = static_cast<const TypeGenerator*>(iter_type)->yield_type();
    } else {
      error(fo.iterable->span, "for-in requires Generator<T>, got '" + print_type(iter_type) + "'");
    }
  }

  // Bind loop variable to the element type.
  auto decl_it = decl_symbols_.find(fo.var_span.offset);
  if (decl_it != decl_symbols_.end() && elem_type != nullptr) {
    symbol_types_[decl_it->second] = elem_type;
    typed_.set_local_type(stmt, elem_type);
  }

  ctx_.loop_depth++;
  check_body(fo.body);
  ctx_.loop_depth--;
}

void TypeChecker::check_yield(const Stmt* stmt) {
  const auto& yield = stmt->as<YieldStatement>();
  const auto* value_type = check_expr(yield.value);

  // yield is only valid inside a generator function (return type is Generator<T>).
  if (ctx_.return_type == nullptr || ctx_.return_type->kind() != TypeKind::Generator) {
    error(stmt->span, "yield is only valid inside a generator function");
    return;
  }

  // The yielded value must match the Generator's element type.
  const auto* gen = static_cast<const TypeGenerator*>(ctx_.return_type);
  const auto* expected = gen->yield_type();
  if (value_type != nullptr && expected != nullptr && value_type != expected) {
    error(yield.value->span,
          "yield type '" + print_type(value_type) + "' does not match Generator element type '" +
              print_type(expected) + "'");
  }

  typed_.set_local_type(stmt, value_type);
}

void TypeChecker::check_match(const Stmt* stmt) {
  const auto& match = stmt->as<MatchStmt>();
  const auto* scrutinee_type = check_expr(match.scrutinee);

  for (const auto& arm : match.arms) {
    suppress_payload_check_ = true;
    const auto* pattern_type = check_expr(arm.pattern, scrutinee_type);
    suppress_payload_check_ = false;
    pending_payload_constructions_.erase(arm.pattern);
    if (scrutinee_type != nullptr && pattern_type != nullptr) {
      if (!is_assignable(pattern_type, scrutinee_type)) {
        error(arm.pattern->span,
              "match arm type '" + print_type(pattern_type) + "' does not match scrutinee type '" +
                  print_type(scrutinee_type) + "'");
      }
    }

    // Validate destructuring bindings against variant payload arity.
    // This runs for ALL enum variant match arms, not just those with
    // bindings — a payload-bearing variant without bindings is an error.
    // Extract the variant name from FieldExpr (.field) or QualifiedName (last segment).
    std::string_view variant_name;
    bool is_variant_pattern = false;
    if (scrutinee_type != nullptr && scrutinee_type->kind() == TypeKind::Enum) {
      if (arm.pattern->is<FieldExpr>()) {
        variant_name = arm.pattern->as<FieldExpr>().field;
        is_variant_pattern = true;
      } else if (arm.pattern->is<QualifiedName>()) {
        const auto& qn = arm.pattern->as<QualifiedName>();
        if (qn.segments.size() >= 2) {
          variant_name = qn.segments.back();
          is_variant_pattern = true;
        }
      }
    }
    if (is_variant_pattern) {
      const auto* enum_type = static_cast<const TypeEnum*>(scrutinee_type);
      const EnumVariant* matched_variant = nullptr;
      for (const auto& variant : enum_type->variants()) {
        if (variant.name == variant_name) {
          matched_variant = &variant;
          break;
        }
      }
      if (matched_variant != nullptr) {
        if (!arm.bindings.empty() && matched_variant->payload_types.empty()) {
          error(arm.binding_spans.empty() ? arm.pattern->span : arm.binding_spans[0],
                "variant '" + std::string(matched_variant->name) +
                    "' has no payload to destructure");
        } else if (!arm.has_rest &&
                   arm.bindings.size() != matched_variant->payload_types.size()) {
          error(arm.pattern->span,
                "variant '" + std::string(matched_variant->name) + "' expects " +
                    std::to_string(matched_variant->payload_types.size()) + " binding(s), got " +
                    std::to_string(arm.bindings.size()));
        } else if (arm.has_rest &&
                   arm.bindings.size() > matched_variant->payload_types.size()) {
          error(arm.pattern->span,
                "variant '" + std::string(matched_variant->name) + "' has " +
                    std::to_string(matched_variant->payload_types.size()) +
                    " field(s), but pattern binds " +
                    std::to_string(arm.bindings.size()) + " (too many even with ..)");
        } else {
          // Register binding types via the declaration symbol table.
          // Named bindings: match field_names for reordering; positional: by index.
          for (size_t i = 0; i < arm.bindings.size(); ++i) {
            size_t field_idx = i; // default positional mapping
            // Try name-based matching if the variant has field_names.
            if (!matched_variant->field_names.empty()) {
              for (size_t j = 0; j < matched_variant->field_names.size(); ++j) {
                if (matched_variant->field_names[j] == arm.bindings[i]) {
                  field_idx = j;
                  break;
                }
              }
            }
            if (field_idx < matched_variant->payload_types.size()) {
              auto sym_it = decl_symbols_.find(arm.binding_spans[i].offset);
              if (sym_it != decl_symbols_.end()) {
                symbol_types_[sym_it->second] = matched_variant->payload_types[field_idx];
              }
            }
          }
        }
      }
    }

    // Register `as` binding: `Pattern as name:` binds the whole matched
    // value to `name` with the scrutinee's type.
    if (!arm.as_binding.empty()) {
      auto sym_it = decl_symbols_.find(arm.as_binding_span.offset);
      if (sym_it != decl_symbols_.end()) {
        symbol_types_[sym_it->second] = scrutinee_type;
      }
    }

    check_body(arm.body);
  }
}

void TypeChecker::check_mode_block(const Stmt* stmt) {
  const auto& mb = stmt->as<ModeBlock>();

  auto saved_ctx = ctx_;
  ctx_.active_modes.insert(mb.mode_name);
  check_body(mb.body);
  ctx_ = saved_ctx;
}

void TypeChecker::check_resource_block(const Stmt* stmt) {
  const auto& rb = stmt->as<ResourceBlock>();
  check_body(rb.body);
}

void TypeChecker::check_return(const Stmt* stmt) {
  const auto& ret = stmt->as<ReturnStatement>();

  // In generator functions, only bare return is valid (early termination).
  bool in_generator =
      ctx_.return_type != nullptr && ctx_.return_type->kind() == TypeKind::Generator;

  if (ret.value != nullptr) {
    const auto* val_type = check_expr(ret.value, ctx_.return_type);
    if (in_generator) {
      error(ret.value->span,
            "'return value' is not valid in a generator function; "
            "use yield to produce values");
    } else if (val_type != nullptr && ctx_.return_type != nullptr &&
               !is_assignable(val_type, ctx_.return_type)) {
      error(ret.value->span,
            "return type '" + print_type(val_type) + "' does not match function return type '" +
                print_type(ctx_.return_type) + "'");
    }
  } else {
    // Bare return — valid for void functions and generator functions.
    if (ctx_.return_type != nullptr && ctx_.return_type->kind() != TypeKind::Void &&
        !in_generator) {
      error(stmt->span, "bare return in function returning '" + print_type(ctx_.return_type) + "'");
    }
  }
}

void TypeChecker::check_expr_stmt(const Stmt* stmt) {
  const auto& es = stmt->as<ExpressionStatement>();
  check_expr(es.expr);
}

// ---------------------------------------------------------------------------
// Expression checking
// ---------------------------------------------------------------------------

auto TypeChecker::check_expr(const Expr* expr) -> const Type* {
  return check_expr(expr, nullptr);
}

auto TypeChecker::check_expr(const Expr* expr, const Type* expected) -> const Type* {
  if (expr == nullptr) {
    return nullptr;
  }

  const Type* result = nullptr;

  switch (expr->kind()) {
  case NodeKind::Identifier:
    result = check_identifier(expr);
    break;
  case NodeKind::IntLiteral:
    result = check_int_literal(expr, expected);
    break;
  case NodeKind::FloatLiteral:
    result = check_float_literal(expr, expected);
    break;
  case NodeKind::StringLiteral:
    result = check_string_literal(expr);
    break;
  case NodeKind::BoolLiteral:
    result = check_bool_literal(expr);
    break;
  case NodeKind::BinaryExpr:
    result = check_binary(expr);
    break;
  case NodeKind::UnaryExpr:
    result = check_unary(expr);
    break;
  case NodeKind::CallExpr:
    result = check_call(expr);
    break;
  case NodeKind::PipeExpr:
    result = check_pipe(expr);
    break;
  case NodeKind::TryExpr:
    result = check_try(expr);
    break;
  case NodeKind::FieldExpr:
    result = check_field(expr);
    break;
  case NodeKind::IndexExpr:
    result = check_index(expr);
    break;
  case NodeKind::Lambda:
    result = check_lambda(expr, expected);
    break;
  case NodeKind::ListLiteral:
    result = check_list_literal(expr);
    break;
  case NodeKind::QualifiedName: {
    // Static method call or enum variant: Type::method / Enum::Variant.
    // The resolver may resolve to a function symbol (static method) or
    // the type symbol itself (enum variant).
    result = check_identifier(expr);
    // If the result is an enum type and this is a 2-segment QualifiedName,
    // validate the variant and mark payload-bearing variants.
    if (result != nullptr && result->kind() == TypeKind::Enum) {
      const auto& qn = expr->as<QualifiedName>();
      if (qn.segments.size() >= 2) {
        const auto* enum_type = static_cast<const TypeEnum*>(result);
        std::string_view vname = qn.segments.back();
        bool found = false;
        for (const auto& variant : enum_type->variants()) {
          if (variant.name == vname) {
            found = true;
            if (!variant.payload_types.empty()) {
              pending_payload_constructions_.insert(expr);
            }
            break;
          }
        }
        if (!found) {
          error(expr->span,
                "'" + std::string(vname) + "' is not a variant of '" +
                    std::string(enum_type->name()) + "'");
          result = nullptr;
        }
      }
    }
    break;
  }
  case NodeKind::ErrorExpr:
    // Recovery placeholder — skip silently.
    break;
  default:
    error(expr->span, "unsupported expression in type checker");
    break;
  }

  // Coerce uninstantiated generic enum types to the expected instantiated
  // type when the expected type is an enum with the same decl_id. This
  // handles cases like `let x: Option<i64> = Option.None` where the
  // variant expression has the uninstantiated generic type.
  //
  // Only fires when the result still contains unresolved generic params.
  // If the result is already a concrete instantiation (e.g. Option<string>
  // inferred from Option.Some("oops")), it is NOT overwritten — the normal
  // is_assignable check catches mismatches.
  if (result != nullptr && expected != nullptr && result->kind() == TypeKind::Enum &&
      expected->kind() == TypeKind::Enum) {
    const auto* result_enum = static_cast<const TypeEnum*>(result);
    const auto* expected_enum = static_cast<const TypeEnum*>(expected);
    if (result_enum->decl_id() == expected_enum->decl_id() && result_enum != expected_enum) {
      // Check if the result enum has any unresolved generic params.
      bool has_generic = false;
      for (const auto& variant : result_enum->variants()) {
        for (const auto* pt : variant.payload_types) {
          if (pt != nullptr && pt->kind() == TypeKind::GenericParam) {
            has_generic = true;
          }
        }
      }
      if (has_generic) {
        result = expected;
      }
    }
  }

  // Reject generic enum values that still have unresolved type params
  // and no expected type to coerce to. Only check value-producing
  // expressions (FieldExpr for variant access, CallExpr for construction),
  // not type-name identifiers.
  //
  // Exception: GenericParams whose binder is NOT the enum's own
  // declaration are bound parameters from an enclosing class or
  // function and will be resolved at monomorphization. Only reject
  // GenericParams that belong to the enum itself (truly unresolved).
  if (result != nullptr && result->kind() == TypeKind::Enum && !suppress_payload_check_ &&
      (expr->kind() == NodeKind::FieldExpr || expr->kind() == NodeKind::CallExpr ||
       expr->kind() == NodeKind::QualifiedName)) {
    const auto* re = static_cast<const TypeEnum*>(result);
    for (const auto& variant : re->variants()) {
      for (const auto* pt : variant.payload_types) {
        if (pt != nullptr && pt->kind() == TypeKind::GenericParam) {
          const auto* gp = static_cast<const TypeGenericParam*>(pt);
          // Allow params from enclosing scopes (class/function generics).
          if (gp->binder() == re->decl_id()) {
            error(expr->span,
                  "cannot infer type argument(s) for generic enum '" + std::string(re->name()) +
                      "'; provide an explicit type annotation");
            result = nullptr;
            goto done_generic_check; // NOLINT
          }
        }
      }
    }
  }
done_generic_check:

  if (result != nullptr) {
    typed_.set_expr_type(expr, result);
  }

  // Check for payload-bearing variant accessed without constructor syntax.
  // check_field inserts; check_call removes. If still present here, the
  // variant was used bare (e.g. `Token.Int` instead of `Token.Int(42)`).
  // Suppressed in match patterns — arity is checked by check_match.
  if ((expr->kind() == NodeKind::FieldExpr || expr->kind() == NodeKind::QualifiedName) &&
      !suppress_payload_check_ && pending_payload_constructions_.count(expr) > 0) {
    pending_payload_constructions_.erase(expr);
    if (result != nullptr && result->kind() == TypeKind::Enum) {
      const auto* en = static_cast<const TypeEnum*>(result);
      std::string variant_str;
      if (expr->kind() == NodeKind::FieldExpr) {
        variant_str = std::string(en->name()) + "." + std::string(expr->as<FieldExpr>().field);
      } else {
        const auto& qn = expr->as<QualifiedName>();
        variant_str = std::string(en->name()) + "::" + std::string(qn.segments.back());
      }
      error(expr->span,
            "enum variant '" + variant_str +
                "' has a payload; use constructor syntax: " + variant_str + "(...)");
      return nullptr;
    }
  }

  return result;
}

// ---------------------------------------------------------------------------
// Identifier
// ---------------------------------------------------------------------------

/// True if `expr` names a type rather than something reached through one:
/// a bare identifier, or a qualified path whose last segment is the type
/// itself (`m::T`).  `T::m` and `m::T::m` name a member (§6), so a call on
/// them is a static-method call, never a construction.
auto TypeChecker::names_a_type(const Expr* expr) const -> bool {
  if (expr->is<IdentifierExpr>()) {
    return true;
  }
  if (!expr->is<QualifiedName>()) {
    return false;
  }
  const auto& qn = expr->as<QualifiedName>();
  if (qn.segments.size() != 2) {
    return false; // `m::T::member` — and a single segment is not qualified
  }
  // `m::T` only when `m` is an import binding; `T::m` is a member of T.
  auto head_it = resolve_.uses.find(expr->span.offset);
  return head_it != resolve_.uses.end() && head_it->second->kind == SymbolKind::Module;
}

auto TypeChecker::symbol_for_use(const Expr* expr) const -> const Symbol* {
  auto at = [&](uint32_t offset) -> const Symbol* {
    auto it = resolve_.uses.find(offset);
    return it == resolve_.uses.end() ? nullptr : it->second;
  };
  const auto* head = at(expr->span.offset);
  if (head == nullptr || !expr->is<QualifiedName>()) {
    return head;
  }
  const auto& qn = expr->as<QualifiedName>();
  if (head->kind != SymbolKind::Module || qn.segments.size() < 2) {
    return head;
  }
  auto export_offset = segment_offset(qn.segments, qn.segment_spans, expr->span, 1);
  const auto* exported = at(export_offset);
  if (exported == nullptr || qn.segments.size() < 3) {
    return exported;
  }
  return at(segment_offset(qn.segments, qn.segment_spans, expr->span, 2));
}

auto TypeChecker::check_identifier(const Expr* expr) -> const Type* {
  // Works for IdentifierExpr and QualifiedName (static method calls,
  // enum variants, and names through import bindings).
  auto name_text = [&] {
    if (expr->is<IdentifierExpr>()) {
      return std::string(expr->as<IdentifierExpr>().name);
    }
    return qualified_path_text(expr->as<QualifiedName>().segments);
  };
  const auto* sym = symbol_for_use(expr);
  if (sym == nullptr) {
    // A path through an import binding is the resolver's to diagnose:
    // a missing import is the graph's report, and a missing export is
    // the resolver's "has no export".  Either way it has been said.
    const auto* head =
        resolve_.uses.contains(expr->span.offset) ? resolve_.uses.at(expr->span.offset) : nullptr;
    if (head == nullptr || head->kind != SymbolKind::Module) {
      error(expr->span, "unresolved identifier '" + name_text() + "'");
    }
    return nullptr;
  }
  if (sym->kind == SymbolKind::Module) {
    error(expr->span, "'" + name_text() + "' is a module, not a value");
    return nullptr;
  }
  // `b::T::m` reaches a static method (§6).  An instance method has a
  // receiver the qualified form cannot supply, so it is not a value here.
  if (expr->is<QualifiedName>() && expr->as<QualifiedName>().segments.size() == 3 &&
      sym->kind == SymbolKind::Function && sym->decl != nullptr) {
    const auto* fn_decl = sym->decl_as_decl();
    if (fn_decl->is<FunctionDecl>()) {
      const auto& params = fn_decl->as<FunctionDecl>().params;
      if (!params.empty() && params.front().name == "self") {
        error(expr->span,
              "'" + name_text() +
                  "' is an instance method; a qualified path reaches static "
                  "methods only (CONTRACT_MODULE_SYSTEM.md §6)");
        return nullptr;
      }
    }
  }
  const auto* result = resolve_symbol_type(sym);
  if (result == nullptr && sym->kind == SymbolKind::Param) {
    error(expr->span, "'" + std::string(sym->name) + "' has no known type in this context");
  }
  return result;
}

// ---------------------------------------------------------------------------
// Literals
// ---------------------------------------------------------------------------

auto TypeChecker::check_int_literal(const Expr* /*expr*/, const Type* expected) -> const Type* {
  // If the target type is a known integer type, adopt it.
  // This allows `let x: i64 = 42` without requiring a suffix.
  if (expected != nullptr && is_integer(expected)) {
    return expected;
  }
  return types_.i32(); // default integer literal type
}

auto TypeChecker::check_float_literal(const Expr* /*expr*/, const Type* expected) -> const Type* {
  // If the target type is a known float type, adopt it.
  if (expected != nullptr && is_float(expected)) {
    return expected;
  }
  return types_.f64(); // default float literal type
}

auto TypeChecker::check_string_literal(const Expr* /*expr*/) -> const Type* {
  return types_.named_type(nullptr, "string", {});
}

auto TypeChecker::check_bool_literal(const Expr* /*expr*/) -> const Type* {
  return types_.bool_type();
}

// ---------------------------------------------------------------------------
// Binary expressions
// ---------------------------------------------------------------------------

auto TypeChecker::check_binary(const Expr* expr) -> const Type* {
  const auto& bin = expr->as<BinaryExpr>();
  // Check both sides, using peer type as context for literal fitting.
  // First pass: LHS without context, RHS with LHS as context.
  const auto* lhs = check_expr(bin.left);
  const auto* rhs = check_expr(bin.right, lhs);
  // Second pass: if RHS provided a concrete type and LHS is a literal
  // that defaulted, re-check LHS with RHS as context.
  if (lhs != nullptr && rhs != nullptr && lhs != rhs &&
      (bin.left->kind() == NodeKind::IntLiteral || bin.left->kind() == NodeKind::FloatLiteral)) {
    lhs = check_expr(bin.left, rhs);
  }
  if (lhs == nullptr || rhs == nullptr) {
    return nullptr;
  }

  switch (bin.op) {
  // Arithmetic: same numeric type required.
  case BinaryOp::Add: {
    // String concatenation: string + string → string.
    if (is_string(lhs)) {
      if (!is_assignable(lhs, rhs)) {
        error(expr->span,
              "mismatched types in string concatenation: '" + print_type(lhs) + "' and '" +
                  print_type(rhs) + "'");
        return nullptr;
      }
      return lhs;
    }
    if (!is_numeric(lhs)) {
      error(bin.left->span, "'+' requires numeric or string type, got '" + print_type(lhs) + "'");
      return nullptr;
    }
    if (!is_assignable(lhs, rhs)) {
      error(expr->span,
            "mismatched types in arithmetic: '" + print_type(lhs) + "' and '" + print_type(rhs) +
                "'");
      return nullptr;
    }
    return lhs;
  }
  case BinaryOp::Sub:
  case BinaryOp::Mul:
  case BinaryOp::Div:
  case BinaryOp::Mod: {
    if (!is_numeric(lhs)) {
      error(bin.left->span, "arithmetic requires numeric type, got '" + print_type(lhs) + "'");
      return nullptr;
    }
    if (!is_assignable(lhs, rhs)) {
      error(expr->span,
            "mismatched types in arithmetic: '" + print_type(lhs) + "' and '" + print_type(rhs) +
                "'");
      return nullptr;
    }
    return lhs;
  }

  // Comparison: same numeric type, result is bool.
  case BinaryOp::Lt:
  case BinaryOp::LtEq:
  case BinaryOp::Gt:
  case BinaryOp::GtEq: {
    if (!is_numeric(lhs)) {
      error(bin.left->span, "comparison requires numeric type, got '" + print_type(lhs) + "'");
      return nullptr;
    }
    if (!is_assignable(lhs, rhs)) {
      error(expr->span,
            "mismatched types in comparison: '" + print_type(lhs) + "' and '" + print_type(rhs) +
                "'");
      return nullptr;
    }
    return types_.bool_type();
  }

  // Equality: same type required, result is bool.
  case BinaryOp::EqEq:
  case BinaryOp::BangEq: {
    if (!is_assignable(lhs, rhs)) {
      error(expr->span,
            "mismatched types in equality: '" + print_type(lhs) + "' and '" + print_type(rhs) +
                "'");
      return nullptr;
    }
    // Reject == / != on payload-bearing enums (unsound without full
    // structural equality — CONTRACT_TYPE_SYSTEM_FOUNDATIONS §13.1).
    if (lhs != nullptr && lhs->kind() == TypeKind::Enum) {
      const auto* enum_type = static_cast<const TypeEnum*>(lhs);
      for (const auto& variant : enum_type->variants()) {
        if (!variant.payload_types.empty()) {
          error(expr->span,
                "equality comparison is not yet supported for payload-bearing "
                "enum '" +
                    std::string(enum_type->name()) + "'; use match to inspect variants");
          return nullptr;
        }
      }
    }
    return types_.bool_type();
  }

  // Logical: both bool, result is bool.
  case BinaryOp::And:
  case BinaryOp::Or: {
    if (!is_assignable(lhs, types_.bool_type())) {
      error(bin.left->span, "logical operator requires 'bool', got '" + print_type(lhs) + "'");
      return nullptr;
    }
    if (!is_assignable(rhs, types_.bool_type())) {
      error(bin.right->span, "logical operator requires 'bool', got '" + print_type(rhs) + "'");
      return nullptr;
    }
    return types_.bool_type();
  }
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// Unary expressions
// ---------------------------------------------------------------------------

auto TypeChecker::check_unary(const Expr* expr) -> const Type* {
  const auto& un = expr->as<UnaryExpr>();
  const auto* operand = check_expr(un.operand);
  if (operand == nullptr) {
    return nullptr;
  }

  switch (un.op) {
  case UnaryOp::Negate: {
    if (!is_numeric(operand)) {
      error(un.operand->span, "negation requires numeric type, got '" + print_type(operand) + "'");
      return nullptr;
    }
    return operand;
  }

  case UnaryOp::Not: {
    if (!is_assignable(operand, types_.bool_type())) {
      error(un.operand->span, "logical not requires 'bool', got '" + print_type(operand) + "'");
      return nullptr;
    }
    return types_.bool_type();
  }

  case UnaryOp::Deref: {
    // Dereference requires mode unsafe.
    if (ctx_.active_modes.find("unsafe") == ctx_.active_modes.end()) {
      error(expr->span, "dereference requires 'mode unsafe =>'");
      return nullptr;
    }
    if (operand->kind() != TypeKind::Pointer) {
      error(un.operand->span, "cannot dereference non-pointer type '" + print_type(operand) + "'");
      return nullptr;
    }
    return static_cast<const TypePointer*>(operand)->pointee();
  }

  case UnaryOp::AddrOf: {
    // Address-of requires an lvalue.
    if (!is_lvalue(un.operand)) {
      error(un.operand->span, "cannot take address of non-lvalue");
      return nullptr;
    }
    return types_.pointer_to(operand);
  }
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// Generic type inference and substitution helpers
// ---------------------------------------------------------------------------

void TypeChecker::infer_type_bindings(const Type* pattern,
                                      const Type* concrete,
                                      std::unordered_map<uint32_t, const Type*>& bindings,
                                      Span error_span) {
  if (pattern == nullptr || concrete == nullptr) {
    return;
  }

  if (pattern->kind() == TypeKind::GenericParam) {
    const auto* gp = static_cast<const TypeGenericParam*>(pattern);
    auto it = bindings.find(gp->index());
    if (it != bindings.end()) {
      // Already bound — check consistency.
      if (it->second != concrete && !is_assignable(it->second, concrete)) {
        error(error_span,
              "conflicting types for generic parameter '" + std::string(gp->name()) + "': '" +
                  print_type(it->second) + "' vs '" + print_type(concrete) + "'");
      }
    } else {
      bindings[gp->index()] = concrete;
    }
    return;
  }

  // Structural recursion for composite types.
  if (pattern->kind() == TypeKind::Pointer && concrete->kind() == TypeKind::Pointer) {
    infer_type_bindings(static_cast<const TypePointer*>(pattern)->pointee(),
                        static_cast<const TypePointer*>(concrete)->pointee(),
                        bindings,
                        error_span);
  } else if (pattern->kind() == TypeKind::Generator && concrete->kind() == TypeKind::Generator) {
    infer_type_bindings(static_cast<const TypeGenerator*>(pattern)->yield_type(),
                        static_cast<const TypeGenerator*>(concrete)->yield_type(),
                        bindings,
                        error_span);
  } else if (pattern->kind() == TypeKind::Function && concrete->kind() == TypeKind::Function) {
    const auto* fp = static_cast<const TypeFunction*>(pattern);
    const auto* fc = static_cast<const TypeFunction*>(concrete);
    if (fp->param_types().size() == fc->param_types().size()) {
      for (size_t j = 0; j < fp->param_types().size(); ++j) {
        infer_type_bindings(fp->param_types()[j], fc->param_types()[j], bindings, error_span);
      }
      infer_type_bindings(fp->return_type(), fc->return_type(), bindings, error_span);
    }
  } else if (pattern->kind() == TypeKind::Struct && concrete->kind() == TypeKind::Struct) {
    const auto* sp = static_cast<const TypeStruct*>(pattern);
    const auto* sc = static_cast<const TypeStruct*>(concrete);
    // Only infer bindings between instances of the same class.
    if (sp->decl_id() == sc->decl_id() && sp->fields().size() == sc->fields().size()) {
      for (size_t j = 0; j < sp->fields().size(); ++j) {
        infer_type_bindings(sp->fields()[j].type, sc->fields()[j].type, bindings, error_span);
      }
    }
  }
}

auto TypeChecker::substitute_generics(const Type* type,
                                      const std::unordered_map<uint32_t, const Type*>& bindings)
    -> const Type* {
  if (type == nullptr || bindings.empty()) {
    return type;
  }

  switch (type->kind()) {
  case TypeKind::GenericParam: {
    const auto* gp = static_cast<const TypeGenericParam*>(type);
    auto it = bindings.find(gp->index());
    return it != bindings.end() ? it->second : type;
  }
  case TypeKind::Pointer: {
    const auto* ptr = static_cast<const TypePointer*>(type);
    const auto* sub = substitute_generics(ptr->pointee(), bindings);
    return sub == ptr->pointee() ? type : types_.pointer_to(sub);
  }
  case TypeKind::Generator: {
    const auto* gen = static_cast<const TypeGenerator*>(type);
    const auto* sub = substitute_generics(gen->yield_type(), bindings);
    return sub == gen->yield_type() ? type : types_.generator_type(sub);
  }
  case TypeKind::Function: {
    const auto* fn = static_cast<const TypeFunction*>(type);
    bool changed = false;
    std::vector<const Type*> params;
    params.reserve(fn->param_types().size());
    for (const auto* param : fn->param_types()) {
      const auto* sub = substitute_generics(param, bindings);
      if (sub != param)
        changed = true;
      params.push_back(sub);
    }
    const auto* ret = substitute_generics(fn->return_type(), bindings);
    if (ret != fn->return_type())
      changed = true;
    return changed ? types_.function_type(std::move(params), ret) : type;
  }
  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(type);
    bool changed = false;
    std::vector<StructField> new_fields;
    new_fields.reserve(st->fields().size());
    for (const auto& field : st->fields()) {
      const auto* sub = substitute_generics(field.type, bindings);
      if (sub != field.type)
        changed = true;
      new_fields.push_back({field.name, sub});
    }
    return changed ? types_.make_struct(st->decl_id(), st->name(), std::move(new_fields)) : type;
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    bool changed = false;
    std::vector<EnumVariant> new_variants;
    new_variants.reserve(en->variants().size());
    for (const auto& variant : en->variants()) {
      std::vector<const Type*> new_payload;
      new_payload.reserve(variant.payload_types.size());
      for (const auto* pt : variant.payload_types) {
        const auto* sub = substitute_generics(pt, bindings);
        if (sub != pt)
          changed = true;
        new_payload.push_back(sub);
      }
      new_variants.push_back({variant.name, std::move(new_payload), variant.field_names});
    }
    return changed ? types_.make_enum(en->decl_id(), en->name(), std::move(new_variants)) : type;
  }
  default:
    return type;
  }
}

// ---------------------------------------------------------------------------
// Shared generic constraint verification
// ---------------------------------------------------------------------------

auto TypeChecker::concept_for_constraint(const TypeNode* constraint) const -> const Symbol* {
  auto at = [&](uint32_t offset) -> const Symbol* {
    auto it = resolve_.uses.find(offset);
    return it == resolve_.uses.end() ? nullptr : it->second;
  };
  // A bound is a type path.  `Concept` records its symbol at the path's
  // own offset; `m::Concept` records the import binding there and the
  // concept at the second segment (CONTRACT_MODULE_SYSTEM.md §6), so
  // reading only the head would silently skip a qualified bound.
  const auto* head = at(constraint->span.offset);
  if (!constraint->is<NamedType>()) {
    return head;
  }
  const auto& path = constraint->as<NamedType>().name;
  if (head == nullptr || head->kind != SymbolKind::Module || path.segments.size() < 2) {
    return head;
  }
  auto name_offset = segment_offset(path.segments, path.segment_spans, path.span, 1);
  return at(name_offset);
}

void TypeChecker::verify_concept_constraints(
    const Expr* callee_expr,
    Span error_span,
    const std::unordered_map<uint32_t, const Type*>& bindings) {
  if (bindings.empty() ||
      !(callee_expr->is<IdentifierExpr>() || callee_expr->is<QualifiedName>())) {
    return;
  }
  const auto* callee_sym = symbol_for_use(callee_expr);
  if (callee_sym == nullptr || callee_sym->kind != SymbolKind::Function ||
      callee_sym->decl == nullptr) {
    return;
  }
  const auto* fn_decl = callee_sym->decl_as_decl();
  if (!fn_decl->is<FunctionDecl>()) {
    return;
  }
  const auto& func = fn_decl->as<FunctionDecl>();
  for (const auto& gp_decl : func.type_params) {
    auto idx = static_cast<uint32_t>(&gp_decl - func.type_params.data());
    auto binding_it = bindings.find(idx);
    if (binding_it == bindings.end()) {
      continue;
    }
    for (const auto* constraint : gp_decl.constraints) {
      const auto* concept_sym = concept_for_constraint(constraint);
      if (concept_sym == nullptr || concept_sym->kind != SymbolKind::Concept ||
          concept_sym->decl == nullptr) {
        continue;
      }
      const auto* concept_decl = concept_sym->decl_as_decl();
      if (!type_conforms_to(binding_it->second, concept_decl)) {
        error(error_span,
              "type '" + print_type(binding_it->second) + "' does not satisfy concept '" +
                  std::string(concept_sym->name) + "' required by generic parameter '" +
                  std::string(gp_decl.name) + "'");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Call expressions
// ---------------------------------------------------------------------------

auto TypeChecker::check_call(const Expr* expr) -> const Type* {
  const auto& call = expr->as<CallExpr>();
  // Suppress payload-check on the callee — check_call validates after.
  suppress_payload_check_ = true;
  const auto* callee_type = check_expr(call.callee);
  suppress_payload_check_ = false;
  if (callee_type == nullptr) {
    return nullptr;
  }

  // Enum variant constructor: Token.Int(42) — callee is a FieldExpr
  // whose type is an enum type.
  if (callee_type->kind() == TypeKind::Enum && call.callee->is<FieldExpr>()) {
    const auto& field = call.callee->as<FieldExpr>();
    const auto* enum_type = static_cast<const TypeEnum*>(callee_type);
    for (const auto& variant : enum_type->variants()) {
      if (variant.name == field.field) {
        if (variant.payload_types.empty()) {
          error(expr->span,
                "enum variant '" + std::string(enum_type->name()) + "." +
                    std::string(variant.name) + "' has no payload; use without parentheses");
          return callee_type;
        }
        if (call.args.size() != variant.payload_types.size()) {
          error(expr->span,
                "enum variant '" + std::string(enum_type->name()) + "." +
                    std::string(variant.name) + "' expects " +
                    std::to_string(variant.payload_types.size()) + " payload field(s), got " +
                    std::to_string(call.args.size()));
          return callee_type;
        }
        // Infer generic bindings from arguments and return the
        // instantiated enum type.
        std::unordered_map<uint32_t, const Type*> type_bindings;
        for (size_t i = 0; i < call.args.size(); ++i) {
          const auto* arg_type = check_expr(call.args[i]);
          if (arg_type != nullptr && variant.payload_types[i] != nullptr) {
            if (!is_assignable(arg_type, variant.payload_types[i])) {
              error(call.args[i]->span,
                    "payload field type '" + print_type(arg_type) + "' is not assignable to '" +
                        print_type(variant.payload_types[i]) + "'");
            }
            // Infer generic param bindings from concrete arg types.
            infer_type_bindings(
                variant.payload_types[i], arg_type, type_bindings, call.args[i]->span);
          }
        }
        // Clear the "needs-construction" mark set by check_field.
        pending_payload_constructions_.erase(call.callee);
        // If generic bindings were inferred, return instantiated type.
        if (!type_bindings.empty()) {
          return substitute_generics(callee_type, type_bindings);
        }
        return callee_type;
      }
    }
    // Variant not found — already diagnosed in check_field.
    return callee_type;
  }

  // Enum variant constructor via :: syntax: Option::Some(42) — callee is
  // a QualifiedName whose type resolves to the enum type.
  if (callee_type->kind() == TypeKind::Enum && call.callee->is<QualifiedName>()) {
    const auto& qn = call.callee->as<QualifiedName>();
    if (qn.segments.size() >= 2) {
      std::string_view variant_name = qn.segments.back();
      const auto* enum_type = static_cast<const TypeEnum*>(callee_type);
      for (const auto& variant : enum_type->variants()) {
        if (variant.name == variant_name) {
          if (variant.payload_types.empty()) {
            error(expr->span,
                  "enum variant '" + std::string(enum_type->name()) + "::" +
                      std::string(variant.name) + "' has no payload; use without parentheses");
            return callee_type;
          }

          // Determine argument order: named args may need reordering.
          std::vector<size_t> arg_order;
          bool has_named = false;
          if (!call.arg_names.empty()) {
            for (const auto& an : call.arg_names) {
              if (!an.empty()) {
                has_named = true;
                break;
              }
            }
          }
          // enum class requires named construction — reject positional
          if (!has_named && !variant.field_names.empty()) {
            error(expr->span,
                  "enum class variant '" + std::string(enum_type->name()) +
                      "::" + std::string(variant.name) +
                      "' requires named fields in construction");
            return callee_type;
          }
          if (has_named && !variant.field_names.empty()) {
            arg_order.resize(call.args.size());
            for (size_t i = 0; i < call.arg_names.size(); ++i) {
              if (!call.arg_names[i].empty()) {
                bool found = false;
                for (size_t j = 0; j < variant.field_names.size(); ++j) {
                  if (variant.field_names[j] == call.arg_names[i]) {
                    arg_order[i] = j;
                    found = true;
                    break;
                  }
                }
                if (!found) {
                  error(call.args[i]->span,
                        "no field '" + std::string(call.arg_names[i]) +
                            "' in variant '" + std::string(variant.name) + "'");
                  return callee_type;
                }
              } else {
                arg_order[i] = i; // positional fallback
              }
            }
          }

          if (call.args.size() != variant.payload_types.size()) {
            error(expr->span,
                  "enum variant '" + std::string(enum_type->name()) + "::" +
                      std::string(variant.name) + "' expects " +
                      std::to_string(variant.payload_types.size()) + " payload field(s), got " +
                      std::to_string(call.args.size()));
            return callee_type;
          }
          std::unordered_map<uint32_t, const Type*> type_bindings;
          for (size_t i = 0; i < call.args.size(); ++i) {
            size_t field_idx = (has_named && !arg_order.empty()) ? arg_order[i] : i;
            const auto* arg_type = check_expr(call.args[i]);
            if (arg_type != nullptr && field_idx < variant.payload_types.size() &&
                variant.payload_types[field_idx] != nullptr) {
              if (!is_assignable(arg_type, variant.payload_types[field_idx])) {
                error(call.args[i]->span,
                      "payload field type '" + print_type(arg_type) + "' is not assignable to '" +
                          print_type(variant.payload_types[field_idx]) + "'");
              }
              infer_type_bindings(
                  variant.payload_types[field_idx], arg_type, type_bindings, call.args[i]->span);
            }
          }
          // Clear the "needs-construction" mark set by check_expr for QualifiedName.
          pending_payload_constructions_.erase(call.callee);
          if (!type_bindings.empty()) {
            return substitute_generics(callee_type, type_bindings);
          }
          return callee_type;
        }
      }
      // Variant not found — emit diagnostic.
      error(expr->span,
            "'" + std::string(variant_name) + "' is not a variant of '" +
                std::string(enum_type->name()) + "'");
      return callee_type;
    }
  }

  // Constructor call: the callee must NAME the type — `Point` or `m::Point`
  // — not merely any expression whose type happens to be a struct (`p`
  // where `p: Point`), and not a path whose last segment is a member of
  // it.  `m::T::missing(...)` denotes a static member that does not
  // exist; reading it as a construction of `T` hides the error whenever
  // the arguments happen to match `T`'s fields.
  if (callee_type->kind() == TypeKind::Struct && names_a_type(call.callee)) {
    const auto* callee_sym = symbol_for_use(call.callee);
    if (callee_sym != nullptr && callee_sym->kind == SymbolKind::Type) {
      return check_construct(expr, static_cast<const TypeStruct*>(callee_type));
    }
  }

  if (callee_type->kind() != TypeKind::Function) {
    error(call.callee->span, "cannot call non-function type '" + print_type(callee_type) + "'");
    return nullptr;
  }

  const auto* fn_type = static_cast<const TypeFunction*>(callee_type);
  const auto& params = fn_type->param_types();

  if (call.args.size() != params.size()) {
    error(expr->span,
          "expected " + std::to_string(params.size()) + " argument(s), got " +
              std::to_string(call.args.size()));
    return nullptr;
  }

  // Detect if the callee is an extern fn (for ABI boundary enforcement).
  bool callee_is_extern = false;
  if (call.callee->is<IdentifierExpr>() || call.callee->is<QualifiedName>()) {
    const auto* callee_sym = symbol_for_use(call.callee);
    if (callee_sym != nullptr && callee_sym->kind == SymbolKind::Function &&
        callee_sym->decl != nullptr) {
      const auto* fn_decl = callee_sym->decl_as_decl();
      if (fn_decl->is<FunctionDecl>()) {
        callee_is_extern = fn_decl->as<FunctionDecl>().is_extern;
      }
    }
  }

  // Check arguments and infer generic type bindings from call site.
  // type_bindings maps generic param index → concrete type.
  std::unordered_map<uint32_t, const Type*> type_bindings;

  // Populate bindings from explicit type arguments: f<i32, f64>(x).
  if (!call.type_args.empty() &&
      (call.callee->is<IdentifierExpr>() || call.callee->is<QualifiedName>())) {
    const auto* callee_sym = symbol_for_use(call.callee);
    if (callee_sym != nullptr && callee_sym->kind == SymbolKind::Function) {
      // Determine expected type param count.
      size_t expected_count = 0;
      if (callee_sym->decl != nullptr) {
        const auto* fn_decl = callee_sym->decl_as_decl();
        if (fn_decl->is<FunctionDecl>()) {
          expected_count = fn_decl->as<FunctionDecl>().type_params.size();
          // For class methods (no own type params), use the enclosing
          // class's type params when invoked via Type<Args>::method().
          if (expected_count == 0 && callee_sym->name.find('.') != std::string_view::npos) {
            // The enclosing class of a method symbol `T.m`.  Two modules
            // may each declare a `T`, so the class must come from the
            // METHOD'S OWN module, not from the first same-named
            // declaration in the program
            // (CONTRACT_TYPE_SYSTEM_FOUNDATIONS.md §11).
            auto class_name = callee_sym->name.substr(0, callee_sym->name.find('.'));
            for (const auto* file_decl : all_decls_) {
              if (file_decl->kind() != NodeKind::ClassDecl ||
                  file_decl->as<ClassDecl>().name != class_name) {
                continue;
              }
              if (declaring_module(file_decl) != callee_sym->module) {
                continue;
              }
              expected_count = file_decl->as<ClassDecl>().type_params.size();
              break;
            }
          }
        }
      } else {
        // Compiler builtin functions (null_ptr, ptr_cast) take 1 type param.
        expected_count = 1;
      }

      if (call.type_args.size() != expected_count) {
        error(expr->span,
              "expected " + std::to_string(expected_count) + " type argument(s), got " +
                  std::to_string(call.type_args.size()));
      } else {
        std::vector<const Type*> resolved_type_args;
        for (size_t i = 0; i < call.type_args.size(); ++i) {
          const auto* resolved = resolve_type_node(call.type_args[i]);
          if (resolved != nullptr) {
            type_bindings[static_cast<uint32_t>(i)] = resolved;
            resolved_type_args.push_back(resolved);
          }
        }
        typed_.set_call_type_args(expr, std::move(resolved_type_args));
      }
    }
  }

  for (size_t i = 0; i < params.size(); ++i) {
    // Reject lambdas in function-pointer positions of extern fn calls.
    // Lambdas cannot cross the C ABI boundary as closures have no
    // context pointer in a C function pointer representation.
    // (CONTRACT_C_ABI_INTEROP §4.4.5)
    if (callee_is_extern && params[i] != nullptr && params[i]->kind() == TypeKind::Function &&
        call.args[i]->is<LambdaExpr>()) {
      error(call.args[i]->span,
            "lambda cannot be passed as a C function pointer; "
            "use a named function instead");
    }

    const auto* arg_type = check_expr(call.args[i], params[i]);
    if (arg_type == nullptr || params[i] == nullptr) {
      continue;
    }
    if (!is_assignable(arg_type, params[i])) {
      error(call.args[i]->span,
            "argument type '" + print_type(arg_type) + "' is not assignable to parameter type '" +
                print_type(params[i]) + "'");
    }
    // Infer generic bindings by structural matching.
    infer_type_bindings(params[i], arg_type, type_bindings, call.args[i]->span);
  }

  verify_concept_constraints(call.callee, expr->span, type_bindings);

  // Substitute generic params in the return type.
  return substitute_generics(fn_type->return_type(), type_bindings);
}

// ---------------------------------------------------------------------------
// Constructor expressions (Point(1, 2))
// ---------------------------------------------------------------------------

auto TypeChecker::check_construct(const Expr* expr, const TypeStruct* struct_type) -> const Type* {
  const auto& call = expr->as<CallExpr>();
  const auto& fields = struct_type->fields();

  if (call.args.size() != fields.size()) {
    error(expr->span,
          "'" + std::string(struct_type->name()) + "' expects " + std::to_string(fields.size()) +
              " field(s), got " + std::to_string(call.args.size()));
    return nullptr;
  }

  std::unordered_map<uint32_t, const Type*> type_bindings;
  for (size_t i = 0; i < fields.size(); ++i) {
    const auto* arg_type = check_expr(call.args[i]);
    if (arg_type != nullptr && fields[i].type != nullptr) {
      if (!is_assignable(arg_type, fields[i].type)) {
        error(call.args[i]->span,
              "field '" + std::string(fields[i].name) + "' expects type '" +
                  print_type(fields[i].type) + "', got '" + print_type(arg_type) + "'");
      }
      infer_type_bindings(fields[i].type, arg_type, type_bindings, call.args[i]->span);
    }
  }

  if (!type_bindings.empty()) {
    return substitute_generics(struct_type, type_bindings);
  }
  return struct_type;
}

// ---------------------------------------------------------------------------
// Pipe expressions
// ---------------------------------------------------------------------------

auto TypeChecker::check_pipe(const Expr* expr) -> const Type* {
  const auto& pipe = expr->as<PipeExpr>();
  const auto* lhs_type = check_expr(pipe.left);
  if (lhs_type == nullptr) {
    return nullptr;
  }

  // The RHS of a pipe is typically a callable. Check it as an expression.
  const auto* rhs_type = check_expr(pipe.right);
  if (rhs_type == nullptr) {
    return nullptr;
  }

  if (rhs_type->kind() != TypeKind::Function) {
    error(pipe.right->span, "pipe target must be callable, got '" + print_type(rhs_type) + "'");
    return nullptr;
  }

  const auto* fn_type = static_cast<const TypeFunction*>(rhs_type);
  const auto& params = fn_type->param_types();

  if (params.empty()) {
    error(pipe.right->span, "pipe target must accept at least one argument");
    return nullptr;
  }

  // LHS becomes first argument — check assignability and infer generics.
  if (!is_assignable(lhs_type, params[0])) {
    error(pipe.left->span,
          "pipe source type '" + print_type(lhs_type) +
              "' is not assignable to first parameter type '" + print_type(params[0]) + "'");
    return nullptr;
  }

  // Infer generic type bindings from the pipe's first argument.
  std::unordered_map<uint32_t, const Type*> type_bindings;
  infer_type_bindings(params[0], lhs_type, type_bindings, pipe.left->span);

  verify_concept_constraints(pipe.right, expr->span, type_bindings);

  // Substitute generic params in the return type.
  return substitute_generics(fn_type->return_type(), type_bindings);
}

// ---------------------------------------------------------------------------
// Try/propagate expressions
// ---------------------------------------------------------------------------

auto TypeChecker::check_try(const Expr* expr) -> const Type* {
  const auto& try_expr = expr->as<TryExpr>();
  const auto* operand_type = check_expr(try_expr.operand);
  if (operand_type == nullptr) {
    return nullptr;
  }

  if (operand_type->kind() != TypeKind::Enum) {
    error(expr->span,
          "'?' operator requires an Option or Result type, got '" + print_type(operand_type) + "'");
    return nullptr;
  }

  const auto* enum_type = static_cast<const TypeEnum*>(operand_type);
  auto name = enum_type->name();

  if (name != "Option" && name != "Result") {
    error(expr->span,
          "'?' operator requires an Option or Result type, got '" + print_type(operand_type) + "'");
    return nullptr;
  }

  // Validate enclosing function return type compatibility.
  if (ctx_.return_type == nullptr) {
    error(expr->span, "'?' operator can only be used inside a function with a return type");
    return nullptr;
  }

  const auto& variants = enum_type->variants();

  if (name == "Result") {
    // Result<T, E>: Ok(T) at variant 0, Err(E) at variant 1.
    // The expression type is T. The enclosing function must return Result<_, E>.
    if (variants.size() < 2 || variants[0].payload_types.empty() ||
        variants[1].payload_types.empty()) {
      error(expr->span, "Result type has unexpected variant structure");
      return nullptr;
    }
    const auto* ok_type = variants[0].payload_types[0];
    const auto* err_type = variants[1].payload_types[0];

    // Check that the enclosing function returns Result<_, E>.
    if (ctx_.return_type->kind() != TypeKind::Enum) {
      error(expr->span,
            "enclosing function must return Result to use '?' on Result, "
            "but returns '" +
                print_type(ctx_.return_type) + "'");
      return nullptr;
    }
    const auto* ret_enum = static_cast<const TypeEnum*>(ctx_.return_type);
    if (ret_enum->name() != "Result") {
      error(expr->span,
            "enclosing function must return Result to use '?' on Result, "
            "but returns '" +
                print_type(ctx_.return_type) + "'");
      return nullptr;
    }
    // Check that the error types match.
    if (ret_enum->variants().size() >= 2 && !ret_enum->variants()[1].payload_types.empty()) {
      const auto* fn_err_type = ret_enum->variants()[1].payload_types[0];
      if (!is_assignable(err_type, fn_err_type)) {
        error(expr->span,
              "error type '" + print_type(err_type) +
                  "' is not assignable to function return error type '" + print_type(fn_err_type) +
                  "'");
      }
    }
    return ok_type;
  }

  // Option<T>: Some(T) at variant 0, None at variant 1.
  // The expression type is T. The enclosing function must return Option<_>.
  if (variants.empty() || variants[0].payload_types.empty()) {
    error(expr->span, "Option type has unexpected variant structure");
    return nullptr;
  }
  const auto* some_type = variants[0].payload_types[0];

  if (ctx_.return_type->kind() != TypeKind::Enum) {
    error(expr->span,
          "enclosing function must return Option to use '?' on Option, "
          "but returns '" +
              print_type(ctx_.return_type) + "'");
    return nullptr;
  }
  const auto* ret_enum = static_cast<const TypeEnum*>(ctx_.return_type);
  if (ret_enum->name() != "Option") {
    error(expr->span,
          "enclosing function must return Option to use '?' on Option, "
          "but returns '" +
              print_type(ctx_.return_type) + "'");
    return nullptr;
  }
  return some_type;
}

// ---------------------------------------------------------------------------
// Field access
// ---------------------------------------------------------------------------

auto TypeChecker::check_field(const Expr* expr) -> const Type* {
  const auto& field = expr->as<FieldExpr>();
  const auto* obj_type = check_expr(field.object);
  if (obj_type == nullptr) {
    return nullptr;
  }

  // Try struct field lookup first.
  if (obj_type->kind() == TypeKind::Struct) {
    const auto* st = static_cast<const TypeStruct*>(obj_type);
    for (const auto& f : st->fields()) {
      if (f.name == field.field) {
        return f.type;
      }
    }
  }

  // Enum variant access: EnumName.VariantName → the enum type.
  if (obj_type->kind() == TypeKind::Enum) {
    const auto* en = static_cast<const TypeEnum*>(obj_type);
    for (const auto& variant : en->variants()) {
      if (variant.name == field.field) {
        // Mark payload-bearing variants as needing constructor syntax.
        // check_call will clear the mark when it handles the construction.
        if (!variant.payload_types.empty()) {
          pending_payload_constructions_.insert(expr);
        }
        return obj_type;
      }
    }
    error(field.field_span,
          "'" + std::string(field.field) + "' is not a variant of '" + std::string(en->name()) +
              "'");
    return nullptr;
  }

  // Try method lookup on any type (struct conformances + extend decls).
  // Skip static methods (no self parameter) — those are only callable
  // via Type::method(), not through an instance.
  const Decl* method_decl = nullptr;
  const auto* method_type = lookup_method(obj_type, field.field, &method_decl);
  if (method_type != nullptr) {
    // Check if this is a static method (no self) — reject for instance calls.
    bool is_static = false;
    if (method_decl != nullptr) {
      const auto& fn = method_decl->as<FunctionDecl>();
      is_static = fn.params.empty() || fn.params[0].name != "self";
    }
    if (!is_static) {
      if (method_decl != nullptr) {
        typed_.set_method_resolution(expr, method_decl);
      }
      return method_type;
    }
    // Static method — fall through to field-not-found error.
  }

  if (obj_type->kind() == TypeKind::Struct) {
    error(field.field_span,
          "no field or method '" + std::string(field.field) + "' on type '" + print_type(obj_type) +
              "'");
  } else {
    error(field.field_span,
          "no method '" + std::string(field.field) + "' on type '" + print_type(obj_type) + "'");
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// build_method_fn_type — build a function type for a method with self removed.
// ---------------------------------------------------------------------------

auto TypeChecker::build_method_fn_type(const FunctionDecl& method) -> const Type* {
  std::vector<const Type*> param_types;
  bool valid = true;
  for (size_t i = 0; i < method.params.size(); ++i) {
    if (i == 0 && method.params[i].name == "self") {
      continue; // skip receiver
    }
    const auto* pt = resolve_type_node(method.params[i].type);
    if (pt == nullptr) {
      valid = false;
    }
    param_types.push_back(pt);
  }
  const auto* ret =
      method.return_type != nullptr ? resolve_type_node(method.return_type) : types_.void_type();
  if (!valid || ret == nullptr) {
    return nullptr;
  }
  return types_.function_type(std::move(param_types), ret);
}

// ---------------------------------------------------------------------------
// build_method_table — pre-populate method_table_ for O(1) lookup.
// Called once after compute_derived_conformances().
// ---------------------------------------------------------------------------

void TypeChecker::build_method_table() {
  // 1. Struct conformance block methods.
  for (const auto* decl : all_decls_) {
    if (decl->kind() != NodeKind::ClassDecl) {
      continue;
    }
    const auto& cls = decl->as<ClassDecl>();
    auto decl_it = decl_symbols_.find(cls.name_span.offset);
    if (decl_it == decl_symbols_.end()) {
      continue;
    }
    const auto* struct_type = resolve_symbol_type(decl_it->second);
    if (struct_type == nullptr || struct_type->kind() != TypeKind::Struct) {
      continue;
    }
    // Direct class methods.
    for (const auto* method_decl : cls.methods) {
      const auto& method = method_decl->as<FunctionDecl>();
      const auto* fn_type = build_method_fn_type(method);
      add_method(MethodKey{struct_type, method.name}, {fn_type, method_decl});
    }
    // Conformance block methods.
    for (const auto& conf : cls.conformances) {
      for (const auto* method_decl : conf.methods) {
        const auto& method = method_decl->as<FunctionDecl>();
        const auto* fn_type = build_method_fn_type(method);
        add_method(MethodKey{struct_type, method.name}, {fn_type, method_decl});
      }
    }
  }

  // 2. Top-level extend declarations.
  for (const auto* decl : all_decls_) {
    if (decl->kind() != NodeKind::ExtendDecl) {
      continue;
    }
    const auto& ext = decl->as<ExtendDecl>();
    const auto* target = resolve_type_node(ext.target_type);
    if (target == nullptr) {
      continue;
    }
    const auto* owner = declaring_module(decl);
    for (const auto* method_decl : ext.methods) {
      const auto& method = method_decl->as<FunctionDecl>();
      const auto* fn_type = build_method_fn_type(method);
      add_method(MethodKey{target, method.name}, {fn_type, method_decl, owner});
    }
  }

  // 3. Derived conformance concept methods.
  //    For each (type, derived_concept), register each concept method
  //    with the type. Resolve the concrete extend implementation for dispatch.
  for (const auto& [type, concepts] : derived_conformances_) {
    // Asked from the deriving class's module, as derivation itself was
    // (§5): this pass runs after compute_derived_conformances() has
    // cleared the current module, and without restoring it every
    // non-prelude extension is invisible — the class's own included.
    ModuleScope derived_in(current_module_,
                           type != nullptr && type->kind() == TypeKind::Struct
                               ? declaring_module(static_cast<const TypeStruct*>(type)->decl_id())
                               : nullptr);
    for (const auto* concept_decl : concepts) {
      const auto& cpt = concept_decl->as<ConceptDecl>();
      ConceptSelfMapGuard guard(concept_self_map_, concept_decl);
      concept_self_map_[concept_decl] = type;
      for (const auto* cpt_method_decl : cpt.methods) {
        const auto& method = cpt_method_decl->as<FunctionDecl>();
        MethodKey key{type, method.name};
        // Skip only when a method already VISIBLE from this module
        // answers the name.  An extension another module declared sits
        // in the table but answers nothing here (§5), so treating its
        // presence as coverage left the type with no method at all.
        if (auto it = method_table_.find(key);
            it != method_table_.end() && visible_entry(it->second) != nullptr) {
          continue;
        }
        const auto* fn_type = build_method_fn_type(method);
        // Find the concrete extend implementation for HIR lowering.
        // Matching the target type and the method's spelling is not
        // enough: an extension is module-local (§5), and one written
        // for a DIFFERENT concept that happens to name a method the
        // same way does not implement this one.
        const Decl* impl_decl = nullptr;
        const ModuleInfo* impl_module = nullptr;
        for (const auto* decl : all_decls_) {
          if (decl->kind() != NodeKind::ExtendDecl) {
            continue;
          }
          const auto& ext = decl->as<ExtendDecl>();
          if (resolve_type_node(ext.target_type) != type) {
            continue;
          }
          const auto* owner = declaring_module(decl);
          if (!extend_is_visible(owner)) {
            continue;
          }
          // An unconstrained `extend T:` supplies methods to anyone; a
          // conforming one supplies them for its own concept only.
          const auto* conforms_to = concept_named_at(ext.target.concept_span);
          if (!ext.target.concept_name.empty() && conforms_to != concept_decl) {
            continue;
          }
          for (const auto* ext_method : ext.methods) {
            if (ext_method->as<FunctionDecl>().name == method.name) {
              impl_decl = ext_method;
              impl_module = owner;
              break;
            }
          }
          if (impl_decl != nullptr) {
            break;
          }
        }
        // A concept method with no concrete implementation is what the
        // derivation itself provides, and is available wherever the
        // type is; a concrete one carries the module that wrote it.
        add_method(key,
                   {fn_type,
                    impl_decl != nullptr ? impl_decl : cpt_method_decl,
                    impl_decl != nullptr ? impl_module : nullptr});
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Method lookup — table-driven with generic param fallback
// ---------------------------------------------------------------------------

auto TypeChecker::lookup_method(const Type* obj_type,
                                std::string_view name,
                                const Decl** resolved_decl) -> const Type* {
  // O(1) table lookup for concrete types (covers struct conformance blocks,
  // extend declarations, and derived conformance methods).
  auto it = method_table_.find(MethodKey{obj_type, name});
  if (it != method_table_.end()) {
    if (const auto* entry = visible_entry(it->second)) {
      if (resolved_decl != nullptr) {
        *resolved_decl = entry->method_decl;
      }
      return entry->fn_type;
    }
  }

  // For concrete struct instantiations (e.g., Vector<i32>), fall back to
  // the generic struct type (Vector<T>) which is how class methods are
  // registered. Build type bindings from the concrete field types to
  // substitute into the method's return/param types.
  if (obj_type->kind() == TypeKind::Struct) {
    const auto* concrete_st = static_cast<const TypeStruct*>(obj_type);
    // Look up the class declaration's generic struct type.
    for (const auto& [key, entries] : method_table_) {
      if (key.name != name || key.type == nullptr || key.type->kind() != TypeKind::Struct) {
        continue;
      }
      const auto* generic_st = static_cast<const TypeStruct*>(key.type);
      if (generic_st->decl_id() != concrete_st->decl_id()) {
        continue;
      }
      // An instantiation reaches only the methods its own module may
      // see, exactly as the direct lookup above does (§5).
      const auto* visible = visible_entry(entries);
      if (visible == nullptr) {
        continue;
      }
      const auto& entry = *visible;
      // Found matching class. Build substitution from generic → concrete
      // field types.
      std::unordered_map<uint32_t, const Type*> bindings;
      for (size_t i = 0; i < generic_st->fields().size() && i < concrete_st->fields().size(); ++i) {
        infer_type_bindings(
            generic_st->fields()[i].type, concrete_st->fields()[i].type, bindings, Span{});
      }
      if (resolved_decl != nullptr) {
        *resolved_decl = entry.method_decl;
      }
      if (bindings.empty()) {
        return entry.fn_type;
      }
      return substitute_generics(entry.fn_type, bindings);
    }
  }

  // Generic type parameter constraint search — this can't be pre-built
  // because it's parameterized on the receiver type variable.
  if (obj_type->kind() == TypeKind::GenericParam) {
    const auto* gp = static_cast<const TypeGenericParam*>(obj_type);
    if (gp->binder() != nullptr) {
      const auto* decl_node = gp->binder();
      const std::vector<GenericParam>* type_params = nullptr;
      if (decl_node->is<FunctionDecl>()) {
        type_params = &decl_node->as<FunctionDecl>().type_params;
      } else if (decl_node->is<ClassDecl>()) {
        type_params = &decl_node->as<ClassDecl>().type_params;
      }
      if (type_params != nullptr && gp->index() < type_params->size()) {
        const auto& gp_decl = (*type_params)[gp->index()];
        for (const auto* constraint : gp_decl.constraints) {
          const auto* concept_sym = concept_for_constraint(constraint);
          if (concept_sym == nullptr || concept_sym->kind != SymbolKind::Concept) {
            continue;
          }
          const auto* cpt_decl = concept_sym->decl_as_decl();
          if (cpt_decl == nullptr || !cpt_decl->is<ConceptDecl>()) {
            continue;
          }
          const auto& cpt = cpt_decl->as<ConceptDecl>();
          for (const auto* concept_method : cpt.methods) {
            const auto& method = concept_method->as<FunctionDecl>();
            if (method.name == name) {
              ConceptSelfMapGuard guard(concept_self_map_, cpt_decl);
              concept_self_map_[cpt_decl] = obj_type;
              if (resolved_decl != nullptr) {
                *resolved_decl = concept_method;
              }
              return build_method_fn_type(method);
            }
          }
        }
      }
    }
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// Receiver validation — conformance/extend methods must have self first param
// ---------------------------------------------------------------------------

void TypeChecker::validate_receiver(const Decl* method, Span context_span) {
  const auto& fn_decl = method->as<FunctionDecl>();
  if (fn_decl.params.empty() || fn_decl.params[0].name != "self") {
    error(fn_decl.name_span.length > 0 ? fn_decl.name_span : context_span,
          "method '" + std::string(fn_decl.name) + "' must have 'self' as its first parameter");
  }
}

// ---------------------------------------------------------------------------
// Index expressions
// ---------------------------------------------------------------------------

auto TypeChecker::check_index(const Expr* expr) -> const Type* {
  const auto& idx = expr->as<IndexExpr>();
  const auto* obj_type = check_expr(idx.object);
  if (obj_type == nullptr) {
    return nullptr;
  }

  // Type-check index expressions but don't enforce semantics yet.
  for (const auto* index : idx.indices) {
    check_expr(index);
  }

  error(expr->span, "indexing is not yet supported in the type checker");
  return nullptr;
}

// ---------------------------------------------------------------------------
// Lambda expressions
// ---------------------------------------------------------------------------

auto TypeChecker::check_lambda(const Expr* expr, const Type* expected) -> const Type* {
  const auto& lam = expr->as<LambdaExpr>();

  // Lambdas require a contextual expected function type.
  if (expected == nullptr || expected->kind() != TypeKind::Function) {
    error(expr->span, "lambda requires expected function type context");
    return nullptr;
  }

  const auto* fn_expected = static_cast<const TypeFunction*>(expected);
  const auto& expected_params = fn_expected->param_types();

  if (lam.params.size() != expected_params.size()) {
    error(expr->span,
          "lambda has " + std::to_string(lam.params.size()) + " parameter(s), expected " +
              std::to_string(expected_params.size()));
    return nullptr;
  }

  // Bind lambda param types from context and register them.
  for (size_t i = 0; i < lam.params.size(); ++i) {
    auto use_it = resolve_.uses.find(lam.params[i].second.offset);
    if (use_it != resolve_.uses.end()) {
      symbol_types_[use_it->second] = expected_params[i];
    }
  }

  // Check body expression against expected return type.
  const auto* body_type = check_expr(lam.body);
  if (body_type != nullptr && fn_expected->return_type() != nullptr &&
      !is_assignable(body_type, fn_expected->return_type())) {
    error(lam.body->span,
          "lambda body type '" + print_type(body_type) + "' does not match expected return type '" +
              print_type(fn_expected->return_type()) + "'");
  }

  return expected;
}

// ---------------------------------------------------------------------------
// List literal
// ---------------------------------------------------------------------------

auto TypeChecker::check_list_literal(const Expr* expr) -> const Type* {
  const auto& list = expr->as<ListLiteral>();

  // Type-check elements but list typing is not yet frozen.
  for (const auto* elem : list.elements) {
    check_expr(elem);
  }

  if (!list.elements.empty()) {
    // All elements must have the same type (if we can check them).
    // For now, just validate they type-check.
  }

  error(expr->span, "list literal typing is not yet supported");
  return nullptr;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

void TypeChecker::error(Span span, std::string message) {
  diagnostics_.push_back(Diagnostic::error(span, std::move(message)));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

auto TypeChecker::is_lvalue(const Expr* expr) -> bool {
  if (expr == nullptr) {
    return false;
  }
  switch (expr->kind()) {
  case NodeKind::Identifier:
    return true;
  case NodeKind::FieldExpr:
    return true;
  case NodeKind::IndexExpr:
    return true;
  case NodeKind::UnaryExpr: {
    // Dereferenced pointer is an lvalue.
    const auto& un = expr->as<UnaryExpr>();
    return un.op == UnaryOp::Deref;
  }
  default:
    return false;
  }
}

auto TypeChecker::find_generic_param_index(const Symbol* sym) -> uint32_t {
  // The symbol's decl points to the enclosing Decl (FunctionDecl or ClassDecl).
  if (sym->decl == nullptr) {
    return 0;
  }
  const auto* decl = sym->decl_as_decl();

  const std::vector<GenericParam>* type_params = nullptr;
  if (decl->is<FunctionDecl>()) {
    type_params = &decl->as<FunctionDecl>().type_params;
  } else if (decl->is<ClassDecl>()) {
    type_params = &decl->as<ClassDecl>().type_params;
  } else if (decl->is<EnumDeclNode>()) {
    type_params = &decl->as<EnumDeclNode>().type_params;
  }

  if (type_params != nullptr) {
    for (uint32_t i = 0; i < type_params->size(); ++i) {
      if ((*type_params)[i].name == sym->name) {
        return i;
      }
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Compiler builtin function types
// ---------------------------------------------------------------------------

auto TypeChecker::resolve_builtin_function_type(std::string_view name) -> const Type* {
  // null_ptr<T>(): *T
  if (name == "null_ptr") {
    auto* generic_t = types_.generic_param(nullptr, "T", 0);
    auto* ptr_t = types_.pointer_to(generic_t);
    return types_.function_type({}, ptr_t);
  }
  // ptr_cast<T>(ptr: *void): *T
  if (name == "ptr_cast") {
    auto* generic_t = types_.generic_param(nullptr, "T", 0);
    auto* ptr_t = types_.pointer_to(generic_t);
    auto* void_ptr = types_.pointer_to(types_.void_type());
    return types_.function_type({void_ptr}, ptr_t);
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Free-function entry point
// ---------------------------------------------------------------------------

auto typecheck(std::span<const FileNode* const> files, const ResolveResult& resolve,
               TypeContext& types) -> TypeCheckResult {
  TypeChecker checker(types, resolve);
  return checker.check(files);
}

auto typecheck(const Program& program, const ResolveResult& resolve, TypeContext& types)
    -> TypeCheckResult {
  // Prelude modules first: they are every module's environment without
  // being import edges, and a module's declarations must be registered
  // before a generic of theirs is instantiated in another module's
  // signature.  Then the remaining modules in topological order.
  std::vector<const FileNode*> nodes;
  std::unordered_map<const FileNode*, const ModuleInfo*> file_modules;
  for (bool prelude : {true, false}) {
    for (const auto* module : program.topo_order) {
      if (module->is_prelude == prelude) {
        nodes.push_back(module->file->parse.file);
        file_modules.emplace(module->file->parse.file, module);
      }
    }
  }
  for (const auto& file : program.files) {
    if (file->parse.file != nullptr && file->module == nullptr) {
      nodes.push_back(file->parse.file);
    }
  }
  TypeChecker checker(types, resolve);
  checker.set_file_modules(std::move(file_modules));
  return checker.check(nodes);
}

auto typecheck(const FileNode& file, const ResolveResult& resolve, TypeContext& types)
    -> TypeCheckResult {
  const FileNode* files[] = {&file};
  return typecheck(std::span<const FileNode* const>(files), resolve, types);
}

} // namespace dao
