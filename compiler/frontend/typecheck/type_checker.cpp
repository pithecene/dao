#include "frontend/typecheck/type_checker.h"

#include "frontend/module/module_graph.h"

#include "frontend/typecheck/type_conversion.h"
#include "frontend/types/type_query.h"
#include "frontend/types/type_ownership.h"

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
  type_decls_.clear();
  for (const auto* file : files) {
    auto module_it = file_modules_.find(file);
    const auto* module = module_it == file_modules_.end() ? nullptr : module_it->second;
    for (const auto* decl : file->declarations) {
      all_decls_.push_back(decl);
      if (module != nullptr) {
        decl_module_.emplace(decl, module);
      }
      // A class or enum by the name its methods are mangled under, so a
      // method call finds its owner without scanning every declaration.
      if (decl->kind() == NodeKind::ClassDecl) {
        type_decls_.emplace(TypeDeclKey{module, decl->as<ClassDecl>().name}, decl);
      } else if (decl->kind() == NodeKind::EnumDecl) {
        type_decls_.emplace(TypeDeclKey{module, decl->as<EnumDeclNode>().name}, decl);
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
      methods.push_back({key.type, key.name, entry.fn_type, entry.extend_module, entry.inherent});
    }
  }

  return {.typed = std::move(typed_),
          .diagnostics = std::move(diagnostics_),
          .methods = std::move(methods)};
}

// ---------------------------------------------------------------------------
// TypeNode -> Type* bridge
// ---------------------------------------------------------------------------

namespace {
auto nominal_type_args(const Type* type) -> const std::vector<const Type*>&;
auto nominal_decl(const Type* type) -> const Decl*;

} // namespace

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
  const auto* declaration = nominal_decl(base_type);
  TypeBindings bindings;
  std::vector<const Type*> resolved;
  resolved.reserve(type_args.size());
  for (size_t i = 0; i < type_args.size(); ++i) {
    const auto* arg_type = resolve_type_node(type_args[i]);
    if (arg_type == nullptr) {
      return nullptr;
    }
    bindings[ParamKey{declaration, static_cast<uint32_t>(i)}] = arg_type;
    resolved.push_back(arg_type);
  }
  // `Node<T>` inside `class Node<T>` is the class itself, not a fresh
  // instantiation: the arguments are the parameters the declaration
  // already stands for, and building a copy here would take one whose
  // fields are still being resolved.
  if (nominal_type_args(base_type) == resolved) {
    return base_type;
  }
  // The arguments belong to the instantiated type: a parameter no field
  // mentions (`class Tag<T>: n: i32`) leaves the fields alone, and
  // without the arguments `Tag<i32>` and `Tag<i64>` would be one type
  // (CONTRACT_TYPECHECKING_BASELINE §4).
  return with_type_args(substitute_generics(base_type, bindings), std::move(resolved));
}

/// The same nominal type, carrying the arguments it was instantiated
/// with.  Any other type is its own instantiation and is returned as is.
auto TypeChecker::with_type_args(const Type* type, std::vector<const Type*> args) -> const Type* {
  if (type == nullptr || args.empty()) {
    return type;
  }
  if (type->kind() == TypeKind::Struct) {
    const auto* st = static_cast<const TypeStruct*>(type);
    if (st->type_args() == args) {
      return type;
    }
    return types_.make_struct(st->decl_id(), st->name(), st->fields(), std::move(args));
  }
  if (type->kind() == TypeKind::Enum) {
    const auto* en = static_cast<const TypeEnum*>(type);
    if (en->type_args() == args) {
      return type;
    }
    return types_.make_enum(en->decl_id(), en->name(), en->variants(), std::move(args));
  }
  return type;
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

      // Ptr<T> — the compiler-standard raw pointer
      // (ADR_RAW_POINTER_SURFACE.md): an ordinary generic application.
      if (name == "Ptr") {
        if (named.type_args.size() != 1) {
          error(node->span, "Ptr requires exactly one type argument");
          return nullptr;
        }
        const auto* pointee = resolve_type_node(named.type_args[0]);
        if (pointee == nullptr) {
          return nullptr;
        }
        return types_.pointer_to(pointee);
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
  auto it = symbol_types_.find(sym);
  if (it != symbol_types_.end()) {
    return it->second;
  }
  // Not registered yet: register it now, on demand, so the name resolves
  // in the pass that first reaches it whatever the source order -- a
  // chain of aliases settles link by link, each once.  A class has its
  // shell from register_type_names; only an alias and an enum have
  // nothing to point at before they register.
  const auto* decl = sym->decl_as_decl();
  if (decl == nullptr || !PullDepth::available(*this)) {
    return nullptr;
  }
  PullDepth depth(*this);
  if (decl->is<AliasDecl>()) {
    return register_type_alias(decl, sym, /*report_failures=*/false);
  }
  if (decl->is<EnumDeclNode>()) {
    register_enum(decl, sym, /*report_failures=*/false);
    it = symbol_types_.find(sym);
    return it != symbol_types_.end() ? it->second : nullptr;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Pass 1: register declaration types
// ---------------------------------------------------------------------------

// A guard on the registration fixpoint, not a working limit: a
// declaration pulls what it depends on as it registers, so a pass
// settles everything it reaches and the next confirms it.  The cap is
// reached only by a defect in that reasoning, and then it is reported.
constexpr size_t kRegistrationRoundCap = 100000;

// The pull recurses -- a chain of aliases is followed link by link --
// so its depth is bounded to keep the stack finite on any input.  A
// chain deeper than this settles a cap's worth of links per fixpoint
// round instead: still one visit per link, plus a round per cap.
constexpr size_t kPullDepthCap = 128;

auto TypeChecker::PullDepth::available(const TypeChecker& checker) -> bool {
  return checker.pull_depth_ < kPullDepthCap;
}

void TypeChecker::register_declarations() {
  pending_classes_.clear(); // Reset pass-local state for this file.
  pending_by_decl_.clear();
  registering_.clear();
  complete_types_.clear();
  fields_registered_ = false;
  register_type_names();
  // Each declaration registers what it names as it resolves, on demand
  // (resolve_symbol_type_for_type_decl, aliases_generic_shell): an alias
  // naming an alias, an enum payload naming an alias of an
  // instantiation, an instantiation of a class whose fields name an
  // alias -- every chain settles in the pass that first reaches it,
  // whatever the source order, each link visited once.
  register_type_aliases(/*report_failures=*/false);
  register_enum_variants(/*report_failures=*/false);
  register_struct_fields(/*report_failures=*/false);
  // What remains untyped depends on a slot the pull could not fill in
  // time: a class whose fields name an instantiation of itself, say.
  // Every pass is provisional -- its diagnostics discarded -- until
  // nothing more resolves; then the final passes report.  Progress is
  // an untyped slot becoming typed, so the loop is bounded by the
  // number of slots; the cap is a guard, never a working limit.
  bool converged = false;
  for (size_t round = 0; round < kRegistrationRoundCap; ++round) {
    size_t progress = register_type_aliases(/*report_failures=*/false);
    progress += register_enum_variants(/*report_failures=*/false);
    progress += register_struct_fields(/*report_failures=*/false);
    if (progress == 0) {
      converged = true;
      break;
    }
  }
  if (!converged) {
    // Still making progress at the cap: continuing would let aliases
    // cache instantiations with holes.  Said, not swallowed.
    error(Span{},
          "type registration did not converge within " + std::to_string(kRegistrationRoundCap) +
              " rounds; the declaration graph is too deep");
  }
  fields_registered_ = true;
  register_type_aliases(/*report_failures=*/true);
  register_enum_variants(/*report_failures=*/true);
  register_struct_fields(/*report_failures=*/true);
  register_signatures();
}

auto TypeChecker::aliases_generic_shell(const TypeNode* node) -> bool {
  if (fields_registered_ || node == nullptr) {
    return false;
  }
  // `Ptr<Box<i32>>`, `fn(Box<i32>): i32`, `Vec<Box<i32>>`: the shell may
  // sit anywhere inside the node.
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
  const auto* sym = it->second;
  const auto* decl = sym->decl_as_decl();
  if (decl == nullptr) {
    return false;
  }
  // Pull the declaration's slots now -- the enum's payloads, the
  // class's fields -- as far as they can be typed yet.  Ready means
  // complete by value: an instantiation clones exactly that graph, and
  // a hole anywhere in it would be cloned.  Past the pull's depth the
  // slots stay as they are, and the answer waits for the next round.
  const bool pull = PullDepth::available(*this);
  if (decl->is<EnumDeclNode>()) {
    if (pull) {
      PullDepth depth(*this);
      register_enum(decl, sym, /*report_failures=*/false);
    }
    auto registered = symbol_types_.find(sym);
    return registered == symbol_types_.end() || !type_complete(registered->second);
  }
  auto pending = pending_by_decl_.find(decl);
  if (pending == pending_by_decl_.end()) {
    return false;
  }
  if (pull) {
    PullDepth depth(*this);
    register_class_fields(*pending->second, /*report_failures=*/false);
  }
  return !type_complete(pending->second->shell);
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
      continue; // registered by an earlier pass, or on demand
    }
    if (register_type_alias(decl, sym, report_failures) != nullptr) {
      ++registered;
    }
  }
  return registered;
}

auto TypeChecker::register_type_alias(const Decl* decl, const Symbol* sym, bool report_failures)
    -> const Type* {
  const auto& alias = decl->as<AliasDecl>();
  if (!registering_.insert(decl).second) {
    return nullptr; // reached again through its own chain: nothing to point at
  }
  auto before = diagnostics_.size();
  const Type* aliased_type = nullptr;
  // An alias of a generic instantiation (`type IntBox = lib::Box<i32>`)
  // waits for `Box` to have its fields: instantiating the shell would
  // cache a `Box<i32>` with no fields at all, and the alias would then
  // accept anything.
  if (!aliases_generic_shell(alias.type)) {
    // Resolve the aliased type and cache it so later lookups of the
    // alias name transparently return the underlying type.
    aliased_type = resolve_type_node(alias.type);
  }
  registering_.erase(decl);
  if (aliased_type != nullptr) {
    symbol_types_[sym] = aliased_type;
    typed_.set_decl_type(decl, aliased_type);
    return aliased_type;
  }
  if (!report_failures) {
    // The target may simply not be registered yet; anything said now
    // would be said again by the final run.
    diagnostics_.resize(before);
    return nullptr;
  }
  if (diagnostics_.size() == before && !resolver_owns_path(alias.type)) {
    // The name resolved to a symbol whose type never materialized —
    // one alias naming another that names it back, say.  Left
    // unsaid, the alias is silently unusable everywhere it appears.
    // A path through an import binding is not that case: the
    // resolver has already said what is wrong with it.
    error(alias.name_span, "cannot resolve the type aliased by '" + std::string(alias.name) + "'");
  }
  return nullptr;
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
      // A generic class stands for itself instantiated with its own
      // parameters, so `Vector<T>` inside the class means the type the
      // class declares rather than some other instantiation of it.
      shell->set_type_args(own_type_args(decl, st.type_params));
      symbol_types_[sym] = shell;
      typed_.set_decl_type(decl, shell);
      pending_classes_.push_back({&st, decl, shell});
    }
  }
  // The list is complete; pointers into it stay valid from here on.
  for (auto& pending : pending_classes_) {
    pending_by_decl_[pending.decl] = &pending;
  }
}

auto TypeChecker::register_enum_variants(bool report_failures) -> size_t {
  size_t progress = 0;
  for (const auto* decl : all_decls_) {
    if (decl->kind() != NodeKind::EnumDecl) {
      continue;
    }
    const auto& en = decl->as<EnumDeclNode>();
    auto decl_it = decl_symbols_.find(en.name_span.offset);
    if (decl_it == decl_symbols_.end()) {
      continue;
    }
    progress += register_enum(decl, decl_it->second, report_failures);
  }
  return progress;
}

auto TypeChecker::register_enum(const Decl* decl, const Symbol* sym, bool report_failures)
    -> size_t {
  // Register the enum type with resolved variant payload types.
  // Unresolved types are kept as nullptr to preserve arity — the
  // primary diagnostic comes from resolve_type_node; dropping the
  // slot would silently mutate the variant shape and produce
  // misleading secondary errors.  Registered once, an enum keeps its
  // identity; later passes revise its payloads in place as the aliases
  // they name settle.  A provisional pass keeps none of its diagnostics.
  const auto& en = decl->as<EnumDeclNode>();
  const TypeEnum* existing = nullptr;
  if (auto known = symbol_types_.find(sym);
      known != symbol_types_.end() && known->second->kind() == TypeKind::Enum) {
    existing = static_cast<const TypeEnum*>(known->second);
  }
  if (existing != nullptr && !report_failures && type_complete(existing)) {
    return 0; // complete by value: nothing a further pass can change
  }
  if (!registering_.insert(decl).second) {
    return 0; // reached again through its own payloads
  }
  size_t progress = 0;
  std::vector<EnumVariant> variants;
  size_t variant_index = 0;
  for (const auto& variant : en.variants) {
    std::vector<const Type*> payload_types;
    for (size_t i = 0; i < variant.payload_types.size(); ++i) {
      auto before = diagnostics_.size();
      const auto* resolved = resolve_type_node(variant.payload_types[i]);
      if (!report_failures) {
        diagnostics_.resize(before);
      }
      // A payload holding the enum itself by value -- directly, through
      // a class or another enum, or as an instantiation of it -- has no
      // finite size.  The slot stays untyped, and the final pass says
      // why.
      if (resolved != nullptr) {
        std::unordered_set<const Type*> seen;
        if (contains_by_value(resolved, decl, seen)) {
          resolved = nullptr;
          if (report_failures) {
            error(variant.payload_types[i]->span,
                  "enum '" + std::string(en.name) +
                      "' cannot contain itself by value in variant '" + std::string(variant.name) +
                      "'; use a pointer (Ptr<" + std::string(en.name) + ">) for recursive types");
          }
        }
      }
      if (resolved != nullptr && existing != nullptr &&
          variant_index < existing->variants().size() &&
          i < existing->variants()[variant_index].payload_types.size() &&
          existing->variants()[variant_index].payload_types[i] == nullptr) {
        ++progress;
      }
      payload_types.push_back(resolved);
    }
    variants.push_back({variant.name, std::move(payload_types), variant.field_names});
    ++variant_index;
  }
  registering_.erase(decl);
  if (existing != nullptr) {
    // The same object every reference already points at, revised --
    // as a class shell's fields are (register_class_fields).
    const_cast<TypeEnum*>(existing)->set_variants(std::move(variants));
    return progress;
  }
  const auto* enum_type =
      types_.make_enum(decl, en.name, std::move(variants), own_type_args(decl, en.type_params));
  symbol_types_[sym] = enum_type;
  typed_.set_decl_type(decl, enum_type);
  for (const auto& v : enum_type->variants()) {
    progress += std::ranges::count_if(v.payload_types, [](const Type* t) { return t != nullptr; });
  }
  return progress;
}

auto TypeChecker::register_struct_fields(bool report_failures) -> size_t {
  size_t progress = 0;
  for (auto& pending : pending_classes_) {
    progress += register_class_fields(pending, report_failures);
  }
  return progress;
}

auto TypeChecker::register_class_fields(PendingClass& pending, bool report_failures) -> size_t {
  // Sub-pass 1b-ii: resolve class field types now that all type
  // shells (classes and enums) are registered in symbol_types_.
  // Unresolved types are kept as nullptr to preserve arity — same
  // rationale as enum variant payloads: dropping the slot silently
  // mutates the struct shape and produces misleading secondary
  // constructor-arity errors instead of the real type-resolution
  // failure.  A provisional pass keeps none of its diagnostics: a field
  // it cannot type yet may be typed by a later pass, and one that never
  // is gets reported exactly once, by the final pass.
  const auto& had = pending.shell->fields();
  if (!report_failures && had.size() == pending.class_decl->fields.size() &&
      type_complete(pending.shell)) {
    // Settled only when complete by value all the way down: a field
    // typed by an instantiation that still carries a hole must be
    // re-resolved once that hole is filled.
    return 0;
  }
  if (!registering_.insert(pending.decl).second) {
    return 0; // reached again through its own fields
  }
  size_t progress = 0;
  std::vector<StructField> fields;
  size_t index = 0;
  for (const auto* field : pending.class_decl->fields) {
    auto before = diagnostics_.size();
    const auto* field_type = resolve_type_node(field->type);
    if (!report_failures) {
      diagnostics_.resize(before);
    }
    if (field_type != nullptr && (index >= had.size() || had[index].type == nullptr)) {
      ++progress;
    }
    if (field_type != nullptr && report_failures) {
      // A field holding the class itself by value -- directly, through
      // another declaration, or as an instantiation of it -- has no
      // finite size.  Said once, here, where the cycle closes.
      std::unordered_set<const Type*> seen;
      if (contains_by_value(field_type, pending.decl, seen)) {
        error(field->type->span,
              "class '" + std::string(pending.class_decl->name) +
                  "' cannot contain itself by value in field '" + std::string(field->name) +
                  "'; use a pointer (Ptr<" + std::string(pending.class_decl->name) +
                  ">) for recursive types");
      }
    }
    fields.push_back({field->name, field_type});
    ++index;
  }
  pending.shell->set_fields(std::move(fields));
  registering_.erase(pending.decl);
  return progress;
}

auto TypeChecker::type_complete(const Type* type) -> bool {
  std::unordered_set<const Type*> seen;
  if (!complete_by_value(type, seen)) {
    return false;
  }
  // No hole anywhere.  A cycle by value is not a hole, but nothing
  // finite either: a declaration holding itself by value is rejected
  // (register_enum, register_class_fields) and never counts as
  // complete, so no alias instantiates it.  Every type the walk
  // reached is checked, since a cycle behind a pointer is a cycle of
  // the type behind the pointer.
  std::unordered_set<const Type*> on_path;
  std::unordered_set<const Type*> acyclic;
  for (const auto* reached : seen) {
    if (value_cycle(reached, on_path, acyclic)) {
      return false;
    }
  }
  // Slots only ever fill, so what is complete stays complete -- and so
  // is everything the walk reached.
  complete_types_.insert(seen.begin(), seen.end());
  return true;
}

auto TypeChecker::value_cycle(const Type* type,
                              std::unordered_set<const Type*>& on_path,
                              std::unordered_set<const Type*>& acyclic) -> bool {
  if (type == nullptr || acyclic.contains(type)) {
    return false;
  }
  if (type->kind() != TypeKind::Struct && type->kind() != TypeKind::Enum) {
    return false; // a pointer, or a scalar: the path by value ends here
  }
  if (!on_path.insert(type).second) {
    return true;
  }
  bool cyclic = false;
  if (type->kind() == TypeKind::Struct) {
    const auto* st = static_cast<const TypeStruct*>(type);
    cyclic = std::ranges::any_of(
        st->fields(), [&](const StructField& f) { return value_cycle(f.type, on_path, acyclic); });
  } else {
    const auto* en = static_cast<const TypeEnum*>(type);
    cyclic = std::ranges::any_of(en->variants(), [&](const EnumVariant& v) {
      return std::ranges::any_of(v.payload_types,
                                 [&](const Type* t) { return value_cycle(t, on_path, acyclic); });
    });
  }
  on_path.erase(type);
  if (!cyclic) {
    acyclic.insert(type);
  }
  return cyclic;
}

auto TypeChecker::complete_by_value(const Type* type, std::unordered_set<const Type*>& seen) const
    -> bool {
  if (type == nullptr) {
    return false;
  }
  if (complete_types_.contains(type)) {
    return true;
  }
  if (!seen.insert(type).second) {
    return true; // a cycle by value is a separate diagnostic; not a hole
  }
  switch (type->kind()) {
  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(type);
    // A shell whose fields are not registered yet has nothing to check
    // and is not complete: the declaration says how many it will have.
    if (const auto* decl = st->decl_id();
        decl != nullptr && decl->is<ClassDecl>() &&
        st->fields().size() != decl->as<ClassDecl>().fields.size()) {
      return false;
    }
    return std::ranges::all_of(
        st->fields(), [&](const StructField& f) { return complete_by_value(f.type, seen); });
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    return std::ranges::all_of(en->variants(), [&](const EnumVariant& v) {
      return std::ranges::all_of(v.payload_types,
                                 [&](const Type* t) { return complete_by_value(t, seen); });
    });
  }
  // Substitution clones through these as it does through fields
  // (substitute_generics), so a hole behind them would be cloned too.
  case TypeKind::Pointer:
    return complete_by_value(static_cast<const TypePointer*>(type)->pointee(), seen);
  case TypeKind::Generator:
    return complete_by_value(static_cast<const TypeGenerator*>(type)->yield_type(), seen);
  case TypeKind::Function: {
    const auto* fn = static_cast<const TypeFunction*>(type);
    return complete_by_value(fn->return_type(), seen) &&
           std::ranges::all_of(fn->param_types(),
                               [&](const Type* t) { return complete_by_value(t, seen); });
  }
  default:
    return true; // scalars carry nothing
  }
}

auto TypeChecker::contains_by_value(const Type* type,
                                    const Decl* target,
                                    std::unordered_set<const Type*>& seen) -> bool {
  if (type == nullptr || !seen.insert(type).second) {
    return false;
  }
  // By declaration, not by object: an instantiation is another object
  // of the same declaration, and holds it by value just the same.
  switch (type->kind()) {
  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(type);
    if (st->decl_id() == target) {
      return true;
    }
    return std::ranges::any_of(st->fields(), [&](const StructField& f) {
      return contains_by_value(f.type, target, seen);
    });
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    if (en->decl_id() == target) {
      return true;
    }
    return std::ranges::any_of(en->variants(), [&](const EnumVariant& v) {
      return std::ranges::any_of(v.payload_types,
                                 [&](const Type* t) { return contains_by_value(t, target, seen); });
    });
  }
  default:
    return false; // behind a pointer the size is finite
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
    // Full checking requires concept-level type reasoning (deferred);
    // the shape of a body needs none.
    for (const auto* method : decl->as<ConceptDecl>().methods) {
      warn_whole_body_domain(method->as<FunctionDecl>());
    }
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
    warn_whole_body_domain(fn);
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
  check_store_escapes_domain(assign.target);
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
  // A domain cannot stay current across a suspension: the consumer
  // would run with it current and a yielded value would be reclaimed
  // under the consumer at the next resume.
  if (!ctx_.resource_blocks.empty()) {
    error(stmt->span,
          "'yield' inside resource block '" + std::string(ctx_.resource_blocks.back().name) +
              "': a resource block cannot stay open across a suspension");
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
    // The variant a pattern names: the last segment of a qualified name.
    // A `FieldExpr` pattern is the dot spelling and was rejected by
    // check_expr above.
    std::string_view variant_name;
    bool is_variant_pattern = false;
    if (scrutinee_type != nullptr && scrutinee_type->kind() == TypeKind::Enum) {
      if (arm.pattern->is<QualifiedName>()) {
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
  // Only `resource memory` is an allocation domain; another kind's body
  // is checked without the domain rules.
  const bool is_domain = rb.resource_kind == "memory";
  if (is_domain) {
    ctx_.resource_blocks.push_back({.stmt = stmt, .name = rb.resource_name});
  }
  check_body(rb.body);
  if (is_domain) {
    ctx_.resource_blocks.pop_back();
  }
}

auto TypeChecker::place_root_symbol(const Expr* expr) const -> const Symbol* {
  switch (expr->kind()) {
  case NodeKind::Identifier:
    return symbol_for_use(expr);
  case NodeKind::FieldExpr:
    return place_root_symbol(expr->as<FieldExpr>().object);
  case NodeKind::IndexExpr:
    return place_root_symbol(expr->as<IndexExpr>().object);
  default:
    return nullptr; // no binding is stored to (a store through a pointer is a call)
  }
}

void TypeChecker::check_store_escapes_domain(const Expr* target) {
  if (ctx_.resource_blocks.empty()) {
    return;
  }
  const auto* root = place_root_symbol(target);
  if (root == nullptr) {
    return;
  }
  const auto* root_type = resolve_symbol_type(root);
  if (root_type == nullptr || !owns_heap_memory(root_type)) {
    return;
  }
  // The binding escapes every enclosing block it was declared outside
  // of: each block's exit copies it one domain outward.
  for (auto it = ctx_.resource_blocks.rbegin(); it != ctx_.resource_blocks.rend(); ++it) {
    const Span extent = it->stmt->span;
    const bool declared_inside = root->decl_span.offset >= extent.offset &&
                                 root->decl_span.offset < extent.offset + extent.length;
    if (declared_inside) {
      break;
    }
    if (holds_generator(root_type)) {
      error(target->span,
            "'" + std::string(root->name) + "' is declared outside resource block '" +
                std::string(it->name) +
                "' and stored to inside it: a generator cannot be copied out of the block");
      return;
    }
    typed_.add_resource_escape(it->stmt, root);
  }
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
    } else if (!ctx_.resource_blocks.empty() && holds_generator(val_type)) {
      // Anything else that owns heap memory is copied out at the exits.
      error(ret.value->span,
            "returning a generator from inside resource block '" +
                std::string(ctx_.resource_blocks.back().name) +
                "': a generator cannot be copied out of the block");
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

namespace {

/// The nominal type's instantiation arguments, empty for anything else.
auto nominal_type_args(const Type* type) -> const std::vector<const Type*>& {
  static const std::vector<const Type*> kNone;
  if (type == nullptr) {
    return kNone;
  }
  if (type->kind() == TypeKind::Struct) {
    return static_cast<const TypeStruct*>(type)->type_args();
  }
  if (type->kind() == TypeKind::Enum) {
    return static_cast<const TypeEnum*>(type)->type_args();
  }
  return kNone;
}

/// The declaration a nominal type comes from, null for anything else.
auto nominal_decl(const Type* type) -> const Decl* {
  if (type == nullptr) {
    return nullptr;
  }
  if (type->kind() == TypeKind::Struct) {
    return static_cast<const TypeStruct*>(type)->decl_id();
  }
  if (type->kind() == TypeKind::Enum) {
    return static_cast<const TypeEnum*>(type)->decl_id();
  }
  return nullptr;
}

} // namespace

auto TypeChecker::check_expr(const Expr* expr) -> const Type* {
  return check_expr(expr, nullptr);
}

/// A construction whose arguments left a parameter of its own class or
/// enum unbound, completed from the type its context asks for.  Only
/// the unbound parameters are filled: an argument that did bind one
/// stands, so a value of the wrong type is still reported rather than
/// papered over.  Anything but a construction is returned unchanged — a
/// value of an existing type is what it is, whatever it is assigned to.
auto TypeChecker::instantiation_from_context(const Expr* expr, const Type* result,
                                             const Type* expected) -> const Type* {
  if (expr == nullptr || result == nullptr || expected == nullptr || result == expected) {
    return result;
  }
  // A construction: a call, or a payload-less variant reached through
  // its enum (`Option::None`).  A qualified name that denotes a value
  // is that value's type, whatever it is assigned to.
  if (expr->is<QualifiedName>()) {
    const auto* sym = symbol_for_use(expr);
    if (sym != nullptr && sym->kind != SymbolKind::Type) {
      return result;
    }
  } else if (!expr->is<CallExpr>()) {
    return result;
  }
  const auto* decl = nominal_decl(result);
  if (decl == nullptr || decl != nominal_decl(expected)) {
    return result;
  }
  const auto& args = nominal_type_args(result);
  const auto& asked = nominal_type_args(expected);
  if (args.size() != asked.size()) {
    return result;
  }
  // Which parameters are open: the ones a construction's arguments left
  // unbound, as it recorded them.  A variant reached by name
  // (`Option::None`) records nothing because it binds nothing — every
  // parameter of its enum is open.
  std::vector<uint32_t> every_position;
  const auto* open = typed_.open_type_params(expr);
  if (open == nullptr) {
    if (!expr->is<QualifiedName>()) {
      return result; // a construction whose arguments bound every parameter
    }
    every_position.reserve(args.size());
    for (uint32_t position = 0; position < args.size(); ++position) {
      every_position.push_back(position);
    }
    open = &every_position;
  }
  std::vector<const Type*> filled(args.begin(), args.end());
  bool any = false;
  for (const auto position : *open) {
    if (position >= filled.size() || asked[position] == nullptr) {
      continue;
    }
    filled[position] = asked[position];
    any = true;
  }
  if (!any) {
    return result;
  }

  // Instantiate the declaration once from the completed arguments
  // rather than substituting into the partly instantiated result: a
  // bound argument may itself be a parameter of this declaration
  // (`Pair(p)` inside `Pair<A, B>` binds `A` to `B`), and substituting
  // would then rewrite that binding along with the open one.
  const auto* base = typed_.decl_type(decl);
  if (base == nullptr) {
    return result;
  }
  TypeBindings instantiation;
  for (size_t i = 0; i < filled.size(); ++i) {
    instantiation[ParamKey{decl, static_cast<uint32_t>(i)}] = filled[i];
  }
  return with_type_args(substitute_generics(base, instantiation), std::move(filled));
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

  // An expression whose own arguments left a parameter unbound takes
  // the instantiation its context asks for: `let x: Option<i64> =
  // Option::None` has nothing to infer from, and neither has `Tag("t")`
  // where no field of `class Tag<T>` is typed by `T`.  An instantiation
  // that did get bound is left alone — `Option::Some("oops")` is
  // `Option<string>`, and the assignability check reports the mismatch.
  result = instantiation_from_context(expr, result, expected);

  // Reject generic enum values that still have unresolved type params
  // and no expected type to coerce to. Only check value-producing
  // expressions (QualifiedName for variant access, CallExpr for
  // construction), not type-name identifiers.
  //
  // Exception: GenericParams whose binder is NOT the enum's own
  // declaration are bound parameters from an enclosing class or
  // function and will be resolved at monomorphization. Only reject
  // GenericParams that belong to the enum itself (truly unresolved).
  if (result != nullptr && result->kind() == TypeKind::Enum && !suppress_payload_check_ &&
      (expr->kind() == NodeKind::CallExpr || expr->kind() == NodeKind::QualifiedName)) {
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
  if (expr->kind() == NodeKind::QualifiedName && !suppress_payload_check_ &&
      pending_payload_constructions_.count(expr) > 0) {
    pending_payload_constructions_.erase(expr);
    if (result != nullptr && result->kind() == TypeKind::Enum) {
      const auto* en = static_cast<const TypeEnum*>(result);
      const auto& qn = expr->as<QualifiedName>();
      std::string variant_str = std::string(en->name()) + "::" + std::string(qn.segments.back());
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
  return resolve_.symbol_for(*expr);
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
  if (sym->kind == SymbolKind::Predeclared && sym->name == "Ptr") {
    error(expr->span,
          "'Ptr' is a type, not a value; a pointer comes from 'Ptr<T>::new()', an allocation, "
          "or a cast");
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

  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// Generic type inference and substitution helpers
// ---------------------------------------------------------------------------

void TypeChecker::infer_type_bindings(const Type* pattern,
                                      const Type* concrete,
                                      TypeBindings& bindings,
                                      Span error_span, GenericBinding binding) {
  // The class pairs matched so far belong to one outermost match: they
  // are forgotten when a new one begins, and kept for its whole walk.
  struct Depth {
    uint32_t& depth;
    explicit Depth(uint32_t& d) : depth(d) { ++depth; }
    Depth(const Depth&) = delete;
    auto operator=(const Depth&) -> Depth& = delete;
    ~Depth() { --depth; }
  };
  if (infer_depth_ == 0) {
    inferring_structs_.clear();
  }
  const Depth depth(infer_depth_);
  if (pattern == nullptr || concrete == nullptr) {
    return;
  }

  if (pattern->kind() == TypeKind::GenericParam) {
    const auto* gp = static_cast<const TypeGenericParam*>(pattern);
    // A parameter some other declaration owns is fixed: it is no slot
    // for this argument, whatever position it sits at.  A method's
    // signature may be written with its class's parameters, so both
    // that class and the method itself may be bound here.
    const bool mine = binding.binder == nullptr ||
                      gp->binder() == binding.binder || gp->binder() == binding.owner;
    if (!mine) {
      return;
    }
    auto it = bindings.find(ParamKey{gp->binder(), gp->index()});
    if (it != bindings.end()) {
      // Already bound — check consistency.  The binding in hand is
      // fixed: a second argument has to mean the same type, and a
      // parameter of the same declaration is no wildcard for it, or a
      // recursive call could rebind `T` under its own signature.
      if (it->second != concrete && !is_assignable(it->second, concrete)) {
        error(error_span,
              "conflicting types for generic parameter '" + std::string(gp->name()) + "': '" +
                  print_type(it->second) + "' vs '" + print_type(concrete) + "'");
      }
    } else {
      bindings[ParamKey{gp->binder(), gp->index()}] = concrete;
    }
    return;
  }

  // Structural recursion for composite types.
  if (pattern->kind() == TypeKind::Pointer && concrete->kind() == TypeKind::Pointer) {
    infer_type_bindings(static_cast<const TypePointer*>(pattern)->pointee(),
                        static_cast<const TypePointer*>(concrete)->pointee(),
                        bindings,
                        error_span, binding);
  } else if (pattern->kind() == TypeKind::Generator && concrete->kind() == TypeKind::Generator) {
    infer_type_bindings(static_cast<const TypeGenerator*>(pattern)->yield_type(),
                        static_cast<const TypeGenerator*>(concrete)->yield_type(),
                        bindings,
                        error_span, binding);
  } else if (pattern->kind() == TypeKind::Function && concrete->kind() == TypeKind::Function) {
    const auto* fp = static_cast<const TypeFunction*>(pattern);
    const auto* fc = static_cast<const TypeFunction*>(concrete);
    if (fp->param_types().size() == fc->param_types().size()) {
      for (size_t j = 0; j < fp->param_types().size(); ++j) {
        infer_type_bindings(fp->param_types()[j], fc->param_types()[j], bindings, error_span, binding);
      }
      infer_type_bindings(fp->return_type(), fc->return_type(), bindings, error_span, binding);
    }
  } else if (pattern->kind() == TypeKind::Struct && concrete->kind() == TypeKind::Struct) {
    const auto* sp = static_cast<const TypeStruct*>(pattern);
    const auto* sc = static_cast<const TypeStruct*>(concrete);
    // Only infer bindings between instances of the same class.  A pair
    // of classes is matched field by field once per outermost match: a
    // class reached again through a pointer (`next: Ptr<Node>`), or a
    // shared class reached along two fields, yields nothing new.
    if (sp->decl_id() == sc->decl_id() && sp->fields().size() == sc->fields().size() &&
        inferring_structs_.insert(std::make_pair(sp, sc)).second) {
      infer_from_type_args(sp->type_args(), sc->type_args(), bindings, error_span, binding);
      for (size_t j = 0; j < sp->fields().size(); ++j) {
        infer_type_bindings(sp->fields()[j].type, sc->fields()[j].type, bindings, error_span, binding);
      }
    }
  } else if (pattern->kind() == TypeKind::Enum && concrete->kind() == TypeKind::Enum) {
    const auto* ep = static_cast<const TypeEnum*>(pattern);
    const auto* ec = static_cast<const TypeEnum*>(concrete);
    if (ep->decl_id() == ec->decl_id()) {
      infer_from_type_args(ep->type_args(), ec->type_args(), bindings, error_span, binding);
    }
  }
}

/// What the argument's instantiation says about the parameters of the
/// pattern's.  A parameter no field or payload mentions is recorded
/// nowhere else, so this is where it is inferred from.
void TypeChecker::infer_from_type_args(const std::vector<const Type*>& pattern,
                                       const std::vector<const Type*>& concrete,
                                       TypeBindings& bindings, Span error_span,
                                       GenericBinding binding) {
  if (pattern.size() != concrete.size()) {
    return;
  }
  for (size_t i = 0; i < pattern.size(); ++i) {
    infer_type_bindings(pattern[i], concrete[i], bindings, error_span, binding);
  }
}

auto TypeChecker::substitute_generics(const Type* type, const TypeBindings& bindings)
    -> const Type* {
  if (type == nullptr || bindings.empty()) {
    return type;
  }

  // A type reached twice is substituted once: a class holding two
  // pointers to the same inner type has one instantiation of it, not
  // two, and a deep nesting of such classes would otherwise be rebuilt
  // once per path.  The answers hold for this substitution alone, so
  // the outermost call owns them.
  const bool outermost = substituted_.empty() && substituting_.empty();
  if (auto done = substituted_.find(type); done != substituted_.end()) {
    return done->second;
  }
  struct Clear {
    std::unordered_map<const Type*, const Type*>& answers;
    GenericParamReach& reach;
    bool mine;
    ~Clear() {
      if (mine) {
        answers.clear();
        reach.forget();
      }
    }
  } clear{substituted_, mentions_param_, outermost};

  const auto* substituted = substitute_generics_walk(type, bindings);
  substituted_.emplace(type, substituted);
  return substituted;
}

auto TypeChecker::substitute_generics_walk(const Type* type, const TypeBindings& bindings)
    -> const Type* {
  switch (type->kind()) {
  case TypeKind::GenericParam: {
    const auto* gp = static_cast<const TypeGenericParam*>(type);
    auto it = bindings.find(ParamKey{gp->binder(), gp->index()});
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
    // A class that reaches itself (`class Node<T>: next: Ptr<Node<T>>`)
    // is instantiated into a shell registered before its fields are
    // walked, so the recursive occurrence resolves to this
    // instantiation rather than leaving the generic one behind.
    auto shell_it = substituting_.find(type);
    if (shell_it != substituting_.end()) {
      return shell_it->second;
    }
    if (!mentions_param_.mentions(type)) {
      return type; // nothing of this type is being substituted
    }
    auto* shell = types_.make_struct_shell(st->decl_id(), st->name());
    substituting_.emplace(type, shell);
    std::vector<StructField> new_fields;
    new_fields.reserve(st->fields().size());
    for (const auto& field : st->fields()) {
      new_fields.push_back({field.name, substitute_generics(field.type, bindings)});
    }
    std::vector<const Type*> new_args;
    new_args.reserve(st->type_args().size());
    for (const auto* arg : st->type_args()) {
      new_args.push_back(substitute_generics(arg, bindings));
    }
    substituting_.erase(type);
    shell->set_fields(std::move(new_fields));
    shell->set_type_args(std::move(new_args));
    return shell;
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    auto shell_it = substituting_.find(type);
    if (shell_it != substituting_.end()) {
      return shell_it->second; // as for a class above
    }
    if (!mentions_param_.mentions(type)) {
      return type;
    }
    auto* shell = types_.make_enum_shell(en->decl_id(), en->name());
    substituting_.emplace(type, shell);
    std::vector<EnumVariant> new_variants;
    new_variants.reserve(en->variants().size());
    for (const auto& variant : en->variants()) {
      std::vector<const Type*> new_payload;
      new_payload.reserve(variant.payload_types.size());
      for (const auto* pt : variant.payload_types) {
        new_payload.push_back(substitute_generics(pt, bindings));
      }
      new_variants.push_back({variant.name, std::move(new_payload), variant.field_names});
    }
    std::vector<const Type*> new_args;
    new_args.reserve(en->type_args().size());
    for (const auto* arg : en->type_args()) {
      new_args.push_back(substitute_generics(arg, bindings));
    }
    substituting_.erase(type);
    shell->set_variants(std::move(new_variants));
    shell->set_type_args(std::move(new_args));
    return shell;
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

/// A method is named by a member expression and resolved by the
/// receiver's type, never by a symbol at the callee's offset, so the
/// declaration a call binds is read from where each kind of callee
/// records it.  A call through a function value resolves to none: that
/// signature is someone else's, and its parameters are already fixed.
/// Whether the value a method is reached on is instantiated WITH a
/// parameter of that method itself — `other.bad(…)` where `other` is a
/// `Box<U>` and `U` is `bad`'s own.  The receiver's substitution then
/// puts that parameter through the signature, where it is the one in
/// scope: deciding it at the call would decide what the enclosing
/// signature is checked against, and retype a pointer for free.
auto TypeChecker::receiver_fixes(const Expr* callee, const Decl* callee_decl) -> bool {
  if (callee_decl == nullptr || callee == nullptr || !callee->is<FieldExpr>()) {
    return false;
  }
  return type_mentions_param_of(typed_.expr_type(callee->as<FieldExpr>().object), callee_decl);
}

auto TypeChecker::callee_function_decl(const Expr* callee) -> const Decl* {
  const Decl* decl = nullptr;
  if (callee->is<FieldExpr>()) {
    decl = typed_.method_resolution(callee);
  } else if (callee->is<IdentifierExpr>() || callee->is<QualifiedName>()) {
    const auto* sym = symbol_for_use(callee);
    if (sym != nullptr && sym->kind == SymbolKind::Function && sym->decl != nullptr) {
      decl = sym->decl_as_decl();
    }
  }
  return decl != nullptr && decl->is<FunctionDecl>() ? decl : nullptr;
}

void TypeChecker::verify_concept_constraints(
    const Decl* callee_decl,
    Span error_span,
    const TypeBindings& bindings) {
  if (bindings.empty() || callee_decl == nullptr) {
    return;
  }
  const auto& func = callee_decl->as<FunctionDecl>();
  for (const auto& gp_decl : func.type_params) {
    auto idx = static_cast<uint32_t>(&gp_decl - func.type_params.data());
    auto binding_it = bindings.find(ParamKey{callee_decl, idx});
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

/// The class or enum a called method belongs to, read from its mangled
/// symbol name (`Box.make`) in the module that declares it; null when
/// the callee names no method.  Its parameters are what a signature
/// like `fn make(value: T): Box<T>` is written with.
auto TypeChecker::enclosing_type_decl(const Expr* callee) -> const Decl* {
  const auto* sym = symbol_for_use(callee);
  if (sym == nullptr || sym->kind != SymbolKind::Function) {
    return nullptr;
  }
  const auto dot = sym->name.find('.');
  if (dot == std::string_view::npos) {
    return nullptr;
  }
  auto it = type_decls_.find(TypeDeclKey{sym->module, sym->name.substr(0, dot)});
  return it == type_decls_.end() ? nullptr : it->second;
}

auto TypeChecker::check_call(const Expr* expr) -> const Type* {
  const auto& call = expr->as<CallExpr>();
  // Suppress payload-check on the callee — check_call validates after.
  suppress_payload_check_ = true;
  const auto* enclosing_callee = call_callee_;
  call_callee_ = call.callee;
  const auto* callee_type = check_expr(call.callee);
  call_callee_ = enclosing_callee;
  suppress_payload_check_ = false;
  if (callee_type == nullptr) {
    return nullptr;
  }

  // An operation of the compiler-standard Ptr<T>: a method the callee's
  // field expression resolved, or the static factory `Ptr<T>::new()`,
  // the one builtin function symbol.  HIR lowers the call to the
  // operation, so it is recorded on the call.  Only a member expression
  // carries a method marker: for `p.get()()` the callee is the inner
  // call, whose own marker belongs to that call alone — inheriting it
  // would lower the outer call as a second operation with no receiver.
  std::optional<PtrOp> ptr_op =
      call.callee->is<FieldExpr>() ? typed_.ptr_op(call.callee) : std::nullopt;
  if (!ptr_op.has_value() && call.callee->is<IdentifierExpr>()) {
    const auto* callee_sym = symbol_for_use(call.callee);
    if (callee_sym != nullptr && callee_sym->kind == SymbolKind::Function &&
        callee_sym->decl == nullptr) {
      ptr_op = PtrOp::New;
    }
  }
  if (ptr_op.has_value()) {
    // Its arguments are positional: no names, no rest marker.
    if (std::ranges::any_of(call.arg_names, [](std::string_view name) { return !name.empty(); })) {
      error(expr->span,
            "Ptr." + std::string(ptr_op_name(*ptr_op)) + " takes its arguments by position");
      return nullptr;
    }
    typed_.set_ptr_op(expr, *ptr_op);
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
          // A rest marker names no field and carries no value: it
          // destructures a pattern, and a construction is no pattern.
          if (std::ranges::find(call.arg_names, "..") != call.arg_names.end()) {
            error(expr->span, "'..' is no argument of a construction; give every field a value");
            return nullptr;
          }
          if (has_named && !variant.field_names.empty()) {
            // Every field is given by name, and each exactly once: the
            // values are placed in the payload by the field they name,
            // so a field named twice would overwrite one slot and leave
            // another with no value at all.
            arg_order.resize(call.args.size());
            std::vector<bool> given(variant.field_names.size(), false);
            for (size_t i = 0; i < call.arg_names.size() && i < call.args.size(); ++i) {
              if (call.arg_names[i].empty()) {
                error(call.args[i]->span,
                      "variant '" + std::string(variant.name) +
                          "' is constructed with every field named; this one is not");
                return nullptr; // reported; an unresolved result would report it again
              }
              bool found = false;
              for (size_t j = 0; j < variant.field_names.size(); ++j) {
                if (variant.field_names[j] == call.arg_names[i]) {
                  if (given[j]) {
                    error(call.args[i]->span,
                          "field '" + std::string(call.arg_names[i]) + "' of variant '" +
                              std::string(variant.name) + "' is given twice");
                    return nullptr;
                  }
                  given[j] = true;
                  arg_order[i] = j;
                  found = true;
                  break;
                }
              }
              if (!found) {
                error(call.args[i]->span,
                      "no field '" + std::string(call.arg_names[i]) +
                          "' in variant '" + std::string(variant.name) + "'");
                return nullptr;
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
          TypeBindings type_bindings;

          // Written-out arguments decide the instantiation, as they do
          // for a class: `Choice::Some<i32>(value = x)` is a
          // `Choice<i32>` and its payload has to be an `i32`.
          const auto* enum_decl = enum_type->decl_id();
          const size_t param_count = enum_decl != nullptr && enum_decl->is<EnumDeclNode>()
                                         ? enum_decl->as<EnumDeclNode>().type_params.size()
                                         : 0;
          const bool written_out = !call.type_args.empty();
          if (written_out) {
            if (call.type_args.size() != param_count) {
              error(expr->span, "'" + std::string(enum_type->name()) + "' expects " +
                                    std::to_string(param_count) + " type argument(s), got " +
                                    std::to_string(call.type_args.size()));
              return nullptr; // reported; an unresolved result would report it again
            }
            std::vector<const Type*> resolved_args;
            resolved_args.reserve(call.type_args.size());
            for (size_t i = 0; i < call.type_args.size(); ++i) {
              const auto* resolved = resolve_type_node(call.type_args[i]);
              if (resolved == nullptr) {
                return callee_type;
              }
              type_bindings[ParamKey{enum_decl, static_cast<uint32_t>(i)}] = resolved;
              resolved_args.push_back(resolved);
            }
            typed_.set_call_type_args(expr, std::move(resolved_args));
          }

          // The payload fields are checked in the order the VARIANT
          // declares them, whatever order they were written in: named
          // fields may be given in any order, and what an earlier field
          // binds is what a later one is checked against, so
          // `Both(second = Maybe::Nothing, first = 1)` learns `T` from
          // `first` before `second` is read as `Maybe<i32>`.
          std::vector<size_t> declaration_order(call.args.size());
          for (size_t i = 0; i < declaration_order.size(); ++i) {
            declaration_order[i] = i;
          }
          if (has_named && !arg_order.empty()) {
            std::ranges::sort(declaration_order,
                              [&](size_t a, size_t b) { return arg_order[a] < arg_order[b]; });
          }
          for (const size_t i : declaration_order) {
            size_t field_idx = (has_named && !arg_order.empty()) ? arg_order[i] : i;
            if (field_idx >= variant.payload_types.size() ||
                variant.payload_types[field_idx] == nullptr) {
              check_expr(call.args[i]);
              continue;
            }
            // Written out or bound by an earlier payload, what is decided
            // by now applies: `Holder::Both(1, Maybe::Nothing)` checks its
            // second payload against `Maybe<i32>`, not `Maybe<T>`.
            const auto* payload_type =
                substitute_generics(variant.payload_types[field_idx], type_bindings);
            const auto* arg_type = check_expr(call.args[i], payload_type);
            if (arg_type == nullptr) {
              continue;
            }
            if (!is_assignable(arg_type, payload_type,
                               written_out ? kFixedGenerics
                                           : GenericBinding{enum_type->decl_id()})) {
              error(call.args[i]->span,
                    "payload field type '" + print_type(arg_type) + "' is not assignable to '" +
                        print_type(payload_type) + "'");
            }
            if (!written_out) {
              infer_type_bindings(variant.payload_types[field_idx], arg_type, type_bindings,
                                  call.args[i]->span);
            }
          }
          // Clear the "needs-construction" mark set by check_expr for QualifiedName.
          pending_payload_constructions_.erase(call.callee);
          return with_inferred_args(expr, substitute_generics(callee_type, type_bindings),
                                    enum_type->decl_id(), type_bindings);
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

  // A name belongs to a field: an `enum class` variant's payload is
  // given by name (CONTRACT_SYNTAX_SURFACE, enum construction), and
  // that construction was handled above.  Everything else takes its
  // arguments by position, so a name here names nothing.
  if (std::ranges::any_of(call.arg_names, [](std::string_view name) { return !name.empty(); })) {
    error(expr->span, "arguments are given by position; a name belongs to a variant's payload");
    return nullptr;
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

  // Detect if the callee is an extern fn (for ABI boundary enforcement),
  // and note which declaration this call binds type parameters of.  A
  // call through a function value binds none: that signature is someone
  // else's, and its parameters are already fixed.
  bool callee_is_extern = false;
  GenericBinding binding;
  binding.binder = callee_function_decl(call.callee);
  if (binding.binder != nullptr) {
    callee_is_extern = binding.binder->as<FunctionDecl>().is_extern;
  }
  // A method's signature may be written with its class's parameters
  // rather than its own (`fn make(value: T): Box<T>` in `class Box<T>`),
  // and the call binds those as much as the method's own.
  binding.owner = enclosing_type_decl(call.callee);

  // Check arguments and infer generic type bindings from call site.
  // type_bindings maps generic param index → concrete type.
  TypeBindings type_bindings;
  // Whether a type argument written at the call was written with the
  // parameters it does not itself bind, which fixes those for this call.
  bool written_args_fix_the_rest = false;

  const bool receiver_fixes_the_callee = receiver_fixes(call.callee, binding.binder);
  if (receiver_fixes_the_callee && !call.type_args.empty()) {
    // Written out, they would bind the very parameter the receiver
    // already stands for, and the signature — return type included —
    // would be rewritten under the enclosing one.  There is no type to
    // write here that means anything but what the receiver says.
    error(expr->span, "'" + std::string(binding.binder->as<FunctionDecl>().name) +
                          "' is reached on a receiver instantiated with its own type parameter, "
                          "so its type arguments are already decided");
    return nullptr;
  }

  // A Ptr<T> method takes explicit type arguments only as `cast<U>()`,
  // exactly one; the others take none.
  if (ptr_op.has_value() && *ptr_op != PtrOp::New) {
    const size_t expected_count = *ptr_op == PtrOp::Cast ? 1 : 0;
    if (call.type_args.size() != expected_count) {
      error(expr->span,
            "Ptr." + std::string(ptr_op_name(*ptr_op)) + " expects " +
                std::to_string(expected_count) + " type argument(s), got " +
                std::to_string(call.type_args.size()));
      return nullptr;
    }
    if (expected_count == 1) {
      const auto* target = resolve_type_node(call.type_args[0]);
      if (target == nullptr) {
        return nullptr;
      }
      type_bindings[ParamKey{nullptr, 0}] = target;
      typed_.set_call_type_args(expr, {target});
    }
  }

  // Populate bindings from explicit type arguments: f<i32, f64>(x).
  if (ptr_op == PtrOp::New) {
    // `Ptr<T>::new()` takes its pointee, the one parameter of the
    // compiler's own signature for it.
    if (call.type_args.size() != 1) {
      error(expr->span, "Ptr.new expects 1 type argument(s), got " +
                            std::to_string(call.type_args.size()));
      return nullptr;
    }
    const auto* pointee = resolve_type_node(call.type_args[0]);
    if (pointee == nullptr) {
      return nullptr;
    }
    type_bindings[ParamKey{nullptr, 0}] = pointee;
    typed_.set_call_type_args(expr, {pointee});
  } else if (!call.type_args.empty() && !ptr_op.has_value()) {
    // Which declaration's parameters the arguments are for: the
    // callee's own where it declares any, otherwise its class's — a
    // method written with its class's parameters is instantiated by
    // `Type<Args>::method()` and by `value.method<Args>()` alike.
    const Decl* owner = nullptr;
    size_t expected_count = 0;
    const auto type_params_of = [](const Decl* decl) -> size_t {
      if (decl == nullptr) {
        return 0;
      }
      if (decl->is<FunctionDecl>()) {
        return decl->as<FunctionDecl>().type_params.size();
      }
      if (decl->kind() == NodeKind::ClassDecl) {
        return decl->as<ClassDecl>().type_params.size();
      }
      if (decl->kind() == NodeKind::EnumDecl) {
        return decl->as<EnumDeclNode>().type_params.size();
      }
      return 0;
    };
    if (call.type_args_name_the_type && binding.owner != nullptr) {
      // Written on the type — `Box<i32>::zero(0)` instantiates the
      // class, whatever parameters the method it names declares.
      owner = binding.owner;
      expected_count = type_params_of(owner);
    } else if (binding.binder != nullptr && binding.binder->is<FunctionDecl>() &&
               !binding.binder->as<FunctionDecl>().type_params.empty()) {
      owner = binding.binder;
      expected_count = type_params_of(owner);
    } else if (binding.owner != nullptr) {
      owner = binding.owner;
      expected_count = type_params_of(owner);
    }

    if (call.type_args.size() != expected_count) {
      error(expr->span, "expected " + std::to_string(expected_count) + " type argument(s), got " +
                            std::to_string(call.type_args.size()));
      return nullptr;
    }
    // Whose parameters the arguments leave for the call to work out:
    // written on the type, the method's own; written for the method, the
    // class's.
    const Decl* left_to_infer = call.type_args_name_the_type && binding.owner != nullptr
                                    ? binding.binder
                                    : binding.owner;
    std::vector<const Type*> resolved_type_args;
    resolved_type_args.reserve(call.type_args.size());
    for (size_t i = 0; i < call.type_args.size(); ++i) {
      const auto* resolved = resolve_type_node(call.type_args[i]);
      if (resolved == nullptr) {
        return nullptr;
      }
      type_bindings[ParamKey{owner, static_cast<uint32_t>(i)}] = resolved;
      // A type argument may be written WITH the parameters that the
      // written-out arguments do not themselves bind — the class's where
      // they are the method's (`Box::identity<Ptr<T>>(p)`), the method's
      // where they are the class's (`Box<U>::write(p, …)` inside
      // `fn write<U>`).  Such a parameter names the one type it stands
      // for, so those parameters are fixed for this call: reading one as
      // a slot to fill would let an argument retype what was written,
      // and §8 leaves `cast<U>` the one pointee conversion.
      written_args_fix_the_rest =
          written_args_fix_the_rest || type_mentions_param_of(resolved, left_to_infer);
      resolved_type_args.push_back(resolved);
    }
    typed_.set_call_type_args(expr, std::move(resolved_type_args));
    typed_.set_type_args_binder(expr, owner);
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

    // With type arguments written at the call, the parameters they
    // bound are already the types they make them, and the value has to
    // be of that type.  What they did NOT bind is still the call's to
    // infer: written on the TYPE they bind its parameters and leave the
    // method's own, written for the METHOD they bind the method's and
    // leave the class's — `Box::select<string>(1, "text")` says what
    // `U` is and learns `T` from its first argument.
    const bool written_out = !call.type_args.empty();
    const bool type_args_bound_the_type = written_out && call.type_args_name_the_type;
    const GenericBinding remaining = written_args_fix_the_rest ? kFixedGenerics
                                     : type_args_bound_the_type
                                         ? GenericBinding{binding.binder}
                                         : GenericBinding{binding.owner};
    // Whatever is decided by now applies: the arguments written at the
    // call, and what the arguments before this one bound.  So the second
    // argument of `keep(1, Option::None)` is checked against
    // `Option<i32>` rather than against `Option<T>`.
    const auto* param_type = substitute_generics(params[i], type_bindings);
    const auto* arg_type = check_expr(call.args[i], param_type);
    if (arg_type == nullptr || param_type == nullptr) {
      continue;
    }
    // `set`'s parameter is the receiver's pointee, already fixed: a type
    // parameter anywhere inside it names that one type, never a type to
    // infer, so the value must be of exactly that type.  Every other
    // parameter belongs to the callee's signature, which this very
    // argument binds (CONTRACT_TYPECHECKING_BASELINE §§4, 8).
    const auto argument_binding = ptr_op == PtrOp::Set || receiver_fixes_the_callee
                                      ? kFixedGenerics
                                  : written_out ? remaining
                                                : binding;
    if (!is_assignable(arg_type, param_type, argument_binding)) {
      error(call.args[i]->span,
            "argument type '" + print_type(arg_type) + "' is not assignable to parameter type '" +
                print_type(param_type) + "'");
    }
    // Infer generic bindings by structural matching, for the parameters
    // this call binds.  A parameter of an enclosing signature is fixed
    // even where it sits at the same position as the callee's, so the
    // binder decides which ones an argument may fill.
    // Nothing is inferred where nothing is being bound: a call through a
    // function value takes a signature someone else wrote, and filling
    // its parameters by position would rebind the caller's own.
    const auto inferring = receiver_fixes_the_callee ? kFixedGenerics
                           : written_out            ? remaining
                                                    : binding;
    if (inferring.binder != nullptr || inferring.owner != nullptr) {
      infer_type_bindings(params[i], arg_type, type_bindings, call.args[i]->span, inferring);
    }
  }

  // The bounds are the callee's own, whether it was named or selected on
  // a receiver: `b.accept<Missing>(v)` has to satisfy `accept`'s bounds
  // exactly as `accept(b, missing)` would.
  verify_concept_constraints(binding.binder, expr->span, type_bindings);

  // Substitute what this call bound into the return type.  The bindings
  // are this call's own — inference above fills only the parameters it
  // may, and the rest were written at the call (`Ptr<i32>::new()`,
  // `Vector<u8>::new()`, whose parameters are the class's) — so they
  // apply by position, as the signature was written.
  return substitute_generics(fn_type->return_type(), type_bindings);
}


/// A declaration's own parameters as types, in order: what a generic
/// class or enum stands instantiated with inside its own body.
auto TypeChecker::own_type_args(const Decl* decl, const std::vector<GenericParam>& type_params)
    -> std::vector<const Type*> {
  std::vector<const Type*> args;
  args.reserve(type_params.size());
  for (size_t i = 0; i < type_params.size(); ++i) {
    args.push_back(types_.generic_param(decl, type_params[i].name, static_cast<uint32_t>(i)));
  }
  return args;
}

/// The instantiation a construction just inferred: the declaration's
/// parameters in order, as far as the arguments bound them.  Without
/// them a constructed `Box(1)` would carry no arguments while the
/// annotation `Box<i32>` carries `i32`, and the two would differ.
/// Whether a nominal type already stands for an instantiation: some
/// position of it holds a type rather than the parameter declared there.
/// An alias (`type IntCell = Cell<i32>`) is such a type, and so is
/// anything reached through one.
auto TypeChecker::already_instantiated(const Type* type, const Decl* decl_id) -> bool {
  const auto& standing = nominal_type_args(type);
  for (size_t i = 0; i < standing.size(); ++i) {
    if (standing[i] != nullptr && !names_own_param(standing[i], decl_id, static_cast<uint32_t>(i))) {
      return true;
    }
  }
  return false;
}

/// Whether a type argument is the declaration's own parameter standing
/// at its own position — what an uninstantiated `class Cell<T>` holds,
/// and what says the position is still to be decided.
auto TypeChecker::names_own_param(const Type* arg, const Decl* decl_id, uint32_t position) -> bool {
  if (arg == nullptr || arg->kind() != TypeKind::GenericParam) {
    return false;
  }
  const auto* param = static_cast<const TypeGenericParam*>(arg);
  return param->binder() == decl_id && param->index() == position;
}

auto TypeChecker::with_inferred_args(const Expr* expr, const Type* type, const Decl* decl_id,
                                     const TypeBindings& bindings)
    -> const Type* {
  if (type == nullptr || decl_id == nullptr) {
    return type;
  }
  const std::vector<GenericParam>* type_params = nullptr;
  if (decl_id->is<ClassDecl>()) {
    type_params = &decl_id->as<ClassDecl>().type_params;
  } else if (decl_id->is<EnumDeclNode>()) {
    type_params = &decl_id->as<EnumDeclNode>().type_params;
  }
  if (type_params == nullptr || type_params->empty()) {
    return type;
  }
  // A parameter no argument bound keeps the declaration's own parameter
  // and is noted as open, so its context can decide it later.  The type
  // alone could not say which those are: a construction written inside
  // the class binds a parameter to that same parameter.
  // What the constructed type already stands instantiated with: a
  // construction written through a concrete alias (`type IntCell =
  // Cell<i32>`) is that instantiation, and nothing about it is open.
  const auto& already = nominal_type_args(type);
  std::vector<const Type*> args;
  std::vector<uint32_t> open;
  args.reserve(type_params->size());
  for (size_t i = 0; i < type_params->size(); ++i) {
    const auto position = static_cast<uint32_t>(i);
    auto it = bindings.find(ParamKey{decl_id, position});
    if (it != bindings.end()) {
      args.push_back(it->second);
      continue;
    }
    const auto* standing = position < already.size() ? already[position] : nullptr;
    if (standing != nullptr && !names_own_param(standing, decl_id, position)) {
      args.push_back(standing);
      continue;
    }
    args.push_back(types_.generic_param(decl_id, (*type_params)[i].name, position));
    open.push_back(position);
  }
  if (!open.empty() && expr != nullptr) {
    typed_.set_open_type_params(expr, std::move(open));
  }
  return with_type_args(type, std::move(args));
}

// ---------------------------------------------------------------------------
// Constructor expressions (Point(1, 2))
// ---------------------------------------------------------------------------

auto TypeChecker::check_construct(const Expr* expr, const TypeStruct* struct_type) -> const Type* {
  const auto& call = expr->as<CallExpr>();
  const auto& fields = struct_type->fields();

  // A class is constructed field by field, in order: naming belongs to
  // an `enum class` variant's payload (CONTRACT_SYNTAX_SURFACE).
  if (std::ranges::any_of(call.arg_names, [](std::string_view name) { return !name.empty(); })) {
    error(expr->span, "'" + std::string(struct_type->name()) +
                          "' takes its fields by position, in declaration order");
    return nullptr;
  }

  if (call.args.size() != fields.size()) {
    error(expr->span,
          "'" + std::string(struct_type->name()) + "' expects " + std::to_string(fields.size()) +
              " field(s), got " + std::to_string(call.args.size()));
    return nullptr;
  }

  TypeBindings type_bindings;

  // Written-out arguments bind first and are no longer open: `Tag<i32>("x")`
  // is a `Tag<i32>` whatever its context asks for, and a field that
  // disagrees with them is reported below.
  const auto* class_decl = struct_type->decl_id();
  const size_t param_count = class_decl != nullptr && class_decl->is<ClassDecl>()
                                 ? class_decl->as<ClassDecl>().type_params.size()
                                 : 0;
  if (!call.type_args.empty()) {
    // A name that already says what its parameters are takes none: `type
    // IntTag = Tag<i32>` IS a `Tag<i32>`, so `IntTag<i64>(1)` would
    // instantiate what is instantiated — and the fields, built for
    // `i32`, would disagree with the arguments it advertised.
    if (already_instantiated(struct_type, class_decl)) {
      error(expr->span, "'" + print_type(struct_type) +
                            "' is already instantiated and takes no type argument(s)");
      return nullptr;
    }
    if (call.type_args.size() != param_count) {
      error(expr->span, "'" + std::string(struct_type->name()) + "' expects " +
                            std::to_string(param_count) + " type argument(s), got " +
                            std::to_string(call.type_args.size()));
      return nullptr;
    }
    std::vector<const Type*> resolved_args;
    resolved_args.reserve(call.type_args.size());
    for (size_t i = 0; i < call.type_args.size(); ++i) {
      const auto* resolved = resolve_type_node(call.type_args[i]);
      if (resolved == nullptr) {
        return nullptr;
      }
      type_bindings[ParamKey{class_decl, static_cast<uint32_t>(i)}] = resolved;
      resolved_args.push_back(resolved);
    }
    typed_.set_call_type_args(expr, std::move(resolved_args));
  }

  const bool written_out = !call.type_args.empty();
  for (size_t i = 0; i < fields.size(); ++i) {
    // With the arguments written out, a field is already the type they
    // make it, and the value has to be of that type; otherwise a field
    // typed by the class's own parameter is bound here — by this
    // argument, or by one before it.  What is decided by now applies, so
    // the second field of `Bundle(1, Maybe::Nothing)` is checked against
    // `Maybe<i32>` rather than against `Maybe<T>`, which nothing could
    // satisfy.
    const auto* field_type = substitute_generics(fields[i].type, type_bindings);
    const auto* arg_type = check_expr(call.args[i], field_type);
    if (arg_type != nullptr && field_type != nullptr) {
      if (!is_assignable(arg_type, field_type,
                         written_out ? kFixedGenerics
                                     : GenericBinding{struct_type->decl_id()})) {
        error(call.args[i]->span,
              "field '" + std::string(fields[i].name) + "' expects type '" +
                  print_type(field_type) + "', got '" + print_type(arg_type) + "'");
      }
      if (!written_out) {
        // With them written out there is nothing left to infer, and
        // inferring anyway would report the same mismatch twice.
        infer_type_bindings(fields[i].type, arg_type, type_bindings, call.args[i]->span);
      }
    }
  }

  // Even with nothing bound the construction is recorded: a class whose
  // parameters no argument reaches (`class Tag<T>: text: string`) leaves
  // all of them open for its context.
  return with_inferred_args(expr, substitute_generics(struct_type, type_bindings),
                            struct_type->decl_id(), type_bindings);
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
  // Named target: this pipe binds its type parameters, exactly as a
  // call of it would.  A computed target binds none.
  GenericBinding binding;
  binding.binder = callee_function_decl(pipe.right);
  if (pipe.right->is<IdentifierExpr>() || pipe.right->is<QualifiedName>()) {
    binding.owner = enclosing_type_decl(pipe.right);
  }
  // A pipe into a method is that method's call: a receiver instantiated
  // with the method's own parameter leaves the pipe nothing to bind
  // either, or the value piped in would decide the parameter the
  // enclosing signature is checked against.
  const bool receiver_fixes_the_target = receiver_fixes(pipe.right, binding.binder);
  if (receiver_fixes_the_target) {
    binding = kFixedGenerics;
  }
  if (!is_assignable(lhs_type, params[0], binding)) {
    error(pipe.left->span,
          "pipe source type '" + print_type(lhs_type) +
              "' is not assignable to first parameter type '" + print_type(params[0]) + "'");
    return nullptr;
  }

  // Infer generic type bindings from the pipe's first argument, where
  // this pipe has any to bind.
  TypeBindings type_bindings;
  if (!receiver_fixes_the_target) {
    infer_type_bindings(params[0], lhs_type, type_bindings, pipe.left->span);
  }

  verify_concept_constraints(binding.binder, expr->span, type_bindings);

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
  // `?` returns from inside the block on the error path -- through the
  // block's exit -- so it is a return of the function's result type,
  // copied out at the exits like any other; only a generator cannot be.
  if (!ctx_.resource_blocks.empty() && holds_generator(ctx_.return_type)) {
    error(expr->span,
          "'?' inside resource block '" + std::string(ctx_.resource_blocks.back().name) +
              "' would return a generator: a generator cannot be copied out of the block");
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

  if (obj_type->kind() == TypeKind::Pointer) {
    return check_ptr_method(expr, field, static_cast<const TypePointer*>(obj_type));
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

  // `Enum.Variant` is field access on a type name: a variant is reached
  // through its enum with `::` (CONTRACT_SYNTAX_SURFACE.md, enum class).
  // A value of the enum type is an instance -- a variable, or a variant
  // itself (`Color::Red`, which resolves to the enum's symbol since a
  // variant has none of its own) -- and its methods are looked up below.
  const auto* obj_sym = symbol_for_use(field.object);
  if (obj_type->kind() == TypeKind::Enum && names_a_type(field.object) && obj_sym != nullptr &&
      obj_sym->kind == SymbolKind::Type) {
    const auto* en = static_cast<const TypeEnum*>(obj_type);
    error(field.field_span,
          "variant access uses '::': " + std::string(en->name()) + "::" + std::string(field.field));
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
// check_ptr_method — a member of the compiler-standard Ptr<T>
// (ADR_RAW_POINTER_SURFACE.md, CONTRACT_TYPECHECKING_BASELINE.md §8).
//
// The receiver's semantic type already says this is a pointer, so the
// member names one operation of its fixed method set, typed as a method
// with `self` removed.  `get`, `set`, and `offset` touch memory: they
// need `mode unsafe =>` and a sized pointee.  `cast` returns `Ptr<U>`
// for the explicit `U` check_call binds.
// ---------------------------------------------------------------------------

auto TypeChecker::check_ptr_method(const Expr* expr, const FieldExpr& field,
                                   const TypePointer* ptr) -> const Type* {
  const auto operation = ptr_method_op(field.field);
  if (!operation.has_value()) {
    error(field.field_span,
          "no method '" + std::string(field.field) + "' on type '" + print_type(ptr) + "'");
    return nullptr;
  }
  const std::string qualified = "Ptr." + std::string(field.field);
  if (call_callee_ != expr) {
    error(field.field_span, "'" + qualified + "' is an operation of Ptr<T>; call it");
    return nullptr;
  }
  if (ptr_op_touches_memory(*operation)) {
    if (ctx_.active_modes.find("unsafe") == ctx_.active_modes.end()) {
      error(field.field_span, qualified + " requires 'mode unsafe =>'");
      return nullptr;
    }
    if (ptr->pointee()->kind() == TypeKind::Void) {
      error(field.field_span,
            qualified + " is invalid on 'Ptr<void>', which has no value or element size; "
                        "cast it to a sized pointee first");
      return nullptr;
    }
  }
  typed_.set_ptr_op(expr, *operation);
  const auto* pointee = ptr->pointee();
  switch (*operation) {
  case PtrOp::Get:
    return types_.function_type({}, pointee);
  case PtrOp::Set:
    return types_.function_type({pointee}, types_.void_type());
  case PtrOp::Offset:
    return types_.function_type({types_.builtin(BuiltinKind::I64)}, ptr);
  case PtrOp::Cast:
    return types_.function_type({},
                                types_.pointer_to(types_.generic_param(nullptr, "U", 0)));
  case PtrOp::IsNull:
    return types_.function_type({}, types_.bool_type());
  case PtrOp::New:
    break;
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
      add_method(MethodKey{struct_type, method.name},
                 {fn_type, method_decl, nullptr, /*inherent=*/true});
    }
    // Conformance block methods.
    for (const auto& conf : cls.conformances) {
      for (const auto* method_decl : conf.methods) {
        const auto& method = method_decl->as<FunctionDecl>();
        const auto* fn_type = build_method_fn_type(method);
        add_method(MethodKey{struct_type, method.name},
                   {fn_type, method_decl, nullptr, /*inherent=*/true});
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
    // Every registration of this name for the class, under whichever
    // generic or concrete key it sits, then the same precedence as the
    // direct lookup: the type's own method, the current module's
    // extension, any visible extension (the prelude's).  The table is
    // unordered; iteration order must not decide.
    const TypeStruct* chosen_st = nullptr;
    const MethodEntry* chosen = nullptr;
    int chosen_tier = 3;
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
      const int tier =
          visible->inherent                                                                  ? 0
          : (visible->extend_module != nullptr && visible->extend_module == current_module_) ? 1
                                                                                             : 2;
      if (tier < chosen_tier) {
        chosen_tier = tier;
        chosen = visible;
        chosen_st = generic_st;
      }
    }
    if (chosen != nullptr) {
      // What the receiver's instantiation says: its arguments first — a
      // parameter no field mentions lives only there — then its fields.
      TypeBindings bindings;
      infer_from_type_args(chosen_st->type_args(), concrete_st->type_args(), bindings, Span{});
      for (size_t i = 0; i < chosen_st->fields().size() && i < concrete_st->fields().size(); ++i) {
        infer_type_bindings(
            chosen_st->fields()[i].type, concrete_st->fields()[i].type, bindings, Span{});
      }
      if (resolved_decl != nullptr) {
        *resolved_decl = chosen->method_decl;
      }
      if (bindings.empty()) {
        return chosen->fn_type;
      }
      // The receiver's instantiation decides the class's parameters and
      // only those: a method's own `U` sits at its own position 0, as
      // the class's `T` does, and substituting by position alone would
      // bind the method's to the receiver's argument.
      return substitute_generics(chosen->fn_type, bindings);
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

void TypeChecker::warn_whole_body_domain(const FunctionDecl& fn) {
  // A domain spanning the whole body is a policy hidden from every
  // caller: each pays a copy of the result, and none can share a
  // domain across calls.  The block belongs at the call site.  Only a
  // memory block is a domain; another resource kind costs a caller
  // nothing.
  if (fn.body.size() != 1 || fn.body[0]->kind() != NodeKind::ResourceBlock) {
    return;
  }
  const auto& block = fn.body[0]->as<ResourceBlock>();
  if (block.resource_kind != "memory") {
    return;
  }
  diagnostics_.push_back(Diagnostic::warning(
      fn.body[0]->span,
      "resource block '" + std::string(block.resource_name) + "' is the whole body of '" +
          std::string(fn.name) + "': open the domain where the function is called instead"));
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
  // A field or element is a place only when what it is taken from is:
  // `p.get().x` names a field of a copy the read produced, and writing
  // it would store nowhere (a pointer is written with `set`).
  case NodeKind::FieldExpr:
    return is_lvalue(expr->as<FieldExpr>().object);
  case NodeKind::IndexExpr:
    return is_lvalue(expr->as<IndexExpr>().object);
  case NodeKind::ErrorExpr:
    // A recovery placeholder the parser already reported (a pointer
    // sigil among them) raises no second diagnostic as a target.
    return true;
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
  // Ptr<T>::new(): Ptr<T>, the null pointer.
  if (name == "Ptr.new") {
    auto* generic_t = types_.generic_param(nullptr, "T", 0);
    return types_.function_type({}, types_.pointer_to(generic_t));
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
