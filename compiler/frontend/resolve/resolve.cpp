#include "frontend/resolve/resolve.h"
#include "frontend/module/module_graph.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>

namespace dao {

auto symbol_kind_name(SymbolKind kind) -> const char* {
  switch (kind) {
  case SymbolKind::Function:
    return "Function";
  case SymbolKind::Type:
    return "Type";
  case SymbolKind::Param:
    return "Param";
  case SymbolKind::Local:
    return "Local";
  case SymbolKind::Field:
    return "Field";
  case SymbolKind::Module:
    return "Module";
  case SymbolKind::Builtin:
    return "Builtin";
  case SymbolKind::Predeclared:
    return "Predeclared";
  case SymbolKind::LambdaParam:
    return "LambdaParam";
  case SymbolKind::GenericParam:
    return "GenericParam";
  case SymbolKind::Concept:
    return "Concept";
  }
  return "Unknown";
}

namespace {

// ---------------------------------------------------------------------------
// Builtin type names — pre-populated into the file scope
// ---------------------------------------------------------------------------

constexpr std::string_view kBuiltinTypes[] = {
    "i8",  "i16", "i32", "i64", "u8",   "u16",
    "u32", "u64", "f32", "f64", "bool",
};

// Predeclared named types — not builtin scalars, but compiler-known
// so that examples work without imports. See
// CONTRACT_TYPE_SYSTEM_FOUNDATIONS.md §5.
constexpr std::string_view kPredeclaredTypes[] = {
    "string",
    "void",
    "Generator",
};

// Compiler builtin functions — generic functions whose bodies are
// replaced by the LLVM backend with inline IR. These are registered
// as predeclared function symbols so they're available without import.
constexpr std::string_view kBuiltinFunctions[] = {
    "null_ptr",
    "ptr_cast",
};

// ---------------------------------------------------------------------------
// Resolver — two-pass name resolution over the AST
// ---------------------------------------------------------------------------

class Resolver {
public:
  /// Per-module resolution over a program (spec §11.2): prelude modules
  /// declare into the prelude scope, every other module into a scope of
  /// its own, in topological order.
  auto run(Program& program) -> ResolveResult {
    std::vector<Unit> units;
    for (auto* module : program.topo_order) {
      units.push_back({.file = module->file->parse.file,
                       .module = module,
                       .is_prelude = module->is_prelude});
    }
    // A file with no module declaration (already a parse error) still
    // resolves, in a scope of its own, so analysis keeps working.
    for (const auto& file : program.files) {
      if (file->parse.file != nullptr && file->module == nullptr) {
        units.push_back({.file = file->parse.file, .module = nullptr, .is_prelude = file->is_prelude});
      }
    }
    return run_units(std::move(units));
  }

  /// Resolution without a program: each file is its own module and the
  /// source map decides prelude membership.
  auto run(std::span<const FileNode* const> files, const SourceMap* source_map)
      -> ResolveResult {
    std::vector<Unit> units;
    for (const auto* file : files) {
      units.push_back({.file = file,
                       .module = nullptr,
                       .is_prelude = source_map != nullptr &&
                                     source_map->is_prelude(file->span.offset)});
    }
    return run_units(std::move(units));
  }

private:
  /// One file being resolved: its module (null without a program) and
  /// the scope its top-level names go into.
  struct Unit {
    const FileNode* file = nullptr;
    ModuleInfo* module = nullptr;
    bool is_prelude = false;
    Scope* scope = nullptr;   // the file's own lexical scope: its import bindings live here
    Scope* decls = nullptr;   // where its top-level names are declared; the shared prelude
                              // scope for a prelude unit, its own scope otherwise
    Scope* exports = nullptr; // what `m::name` reaches; differs only for a prelude unit
  };

  ResolveContext ctx_;
  Scope* builtins_ = nullptr;
  Scope* prelude_ = nullptr;
  const Unit* current_ = nullptr; // the unit being declared or resolved
  std::unordered_map<uint32_t, Symbol*> uses_;
  // The current unit's resolved import edges, by display name.
  std::unordered_map<std::string, const ModuleInfo*> imports_by_display_;
  std::vector<Diagnostic> diagnostics_;

  auto run_units(std::vector<Unit> units) -> ResolveResult {
    // Scope shape (spec §7.6): builtins, then the prelude group as one
    // namespace, then one scope per module.  Builtins and prelude are
    // the lexical environment of every module, so both span the whole
    // program: offset-based scope lookup descends through them to the
    // module scope that contains a position.
    const auto program_span = covering_span(units);
    builtins_ = ctx_.make_scope(ScopeKind::Builtins, nullptr);
    builtins_->set_range(program_span);
    prelude_ = ctx_.make_scope(ScopeKind::Prelude, builtins_);
    prelude_->set_range(program_span);
    populate_builtins();

    for (auto& unit : units) {
      // Every file has a lexical scope of its own.  An import binds a
      // name in the importing module and nowhere else (§3.1-§3.3), so
      // even a prelude file's imports are private to it — sharing the
      // prelude scope for them would publish one prelude module's
      // binding to every module in the program.
      unit.scope = ctx_.make_scope(ScopeKind::Module, prelude_);
      unit.scope->set_range(unit.file->span);
      if (unit.is_prelude) {
        // Prelude modules share one namespace for their DECLARATIONS
        // (§7.2, §7.3), so those go into the prelude scope, which is
        // every module's environment.  Their qualified exports stay
        // their own (§7.5), in a table carrying no range so it never
        // takes part in positional lookup.
        unit.decls = prelude_;
        unit.exports = ctx_.make_scope(ScopeKind::Module, prelude_);
      } else {
        unit.decls = unit.scope;
        unit.exports = unit.scope;
      }
      if (unit.module != nullptr) {
        unit.module->scope = unit.scope;
        unit.module->exports = unit.exports;
      }
    }

    // Pass 1: declare top-level names and bind imports.
    for (const auto& unit : units) {
      current_ = &unit;
      collect_top_level(unit);
    }
    // Pass 2: resolve bodies.
    for (const auto& unit : units) {
      current_ = &unit;
      resolve_bodies(unit);
    }
    current_ = nullptr;

    return ResolveResult{
        .context = std::move(ctx_),
        .uses = std::move(uses_),
        .diagnostics = std::move(diagnostics_),
    };
  }

  // Smallest span covering the units' files (files occupy disjoint
  // ranges of one program-wide offset space).
  static auto covering_span(const std::vector<Unit>& units) -> Span {
    bool any = false;
    uint32_t begin = 0;
    uint32_t end = 0;
    for (const auto& unit : units) {
      auto span = unit.file->span;
      if (!any) {
        begin = span.offset;
        end = span.offset + span.length;
        any = true;
      } else {
        begin = std::min(begin, span.offset);
        end = std::max(end, span.offset + span.length);
      }
    }
    return any ? Span{.offset = begin, .length = end - begin} : Span{};
  }

  /// A symbol owned by the module being resolved.
  auto new_symbol(SymbolKind kind, std::string_view name, Span decl_span, const void* decl)
      -> Symbol* {
    auto* sym = ctx_.make_symbol(kind, name, decl_span, decl);
    sym->module = current_ != nullptr ? current_->module : nullptr;
    return sym;
  }

  // --- Builtin population ---

  void populate_builtins() {
    for (auto name : kBuiltinTypes) {
      builtins_->declare(name, ctx_.make_symbol(SymbolKind::Builtin, name, Span{}, nullptr));
    }
    for (auto name : kPredeclaredTypes) {
      builtins_->declare(name, ctx_.make_symbol(SymbolKind::Predeclared, name, Span{}, nullptr));
    }
    for (auto name : kBuiltinFunctions) {
      builtins_->declare(name, ctx_.make_symbol(SymbolKind::Function, name, Span{}, nullptr));
    }
  }

  // --- Pass 1: Collect top-level declarations ---

  void collect_top_level(const Unit& unit) {
    for (const auto* decl : unit.file->declarations) {
      collect_decl(*decl);
    }
    // The graph resolved this module's edges; index them once rather
    // than scanning the list for every import in the file.
    imports_by_display_.clear();
    if (unit.module != nullptr) {
      for (const auto* imported : unit.module->imports) {
        imports_by_display_.emplace(imported->display, imported);
      }
    }
    for (const auto* imp : unit.file->imports) {
      collect_import(*imp);
    }
    imports_by_display_.clear();
  }

  void collect_import(const ImportNode& node) {
    const auto& path = node.path;
    if (path.segments.empty()) {
      return;
    }

    // Bind the last segment as a Module symbol.
    auto binding_name = path.segments.back();
    auto binding_len = static_cast<uint32_t>(binding_name.size());

    // Compute the span of the last segment.
    uint32_t offset = path.span.offset;
    for (size_t i = 0; i + 1 < path.segments.size(); ++i) {
      offset += static_cast<uint32_t>(path.segments[i].size()) + 2; // skip "::"
    }
    Span binding_span{.offset = offset, .length = binding_len};

    if (current_->scope->lookup_local(binding_name) != nullptr ||
        builtins_->lookup_local(binding_name) != nullptr) {
      diagnostics_.push_back(Diagnostic::error(
          binding_span,
          "duplicate top-level declaration '" + std::string(binding_name) + "'"));
      return;
    }
    // The binding's target is the module the graph resolved this import
    // to; null without a program or when the graph reported it missing.
    const ModuleInfo* target = nullptr;
    if (current_->module != nullptr) {
      auto it = imports_by_display_.find(module_display(path.segments));
      target = it == imports_by_display_.end() ? nullptr : it->second;
    }
    auto* sym = new_symbol(SymbolKind::Module, binding_name, binding_span, target);
    current_->scope->declare(binding_name, sym);
  }

  void collect_decl(const Decl& decl) {
    std::string_view name;
    Span name_span{};
    SymbolKind kind{};

    switch (decl.kind()) {
    case NodeKind::FunctionDecl: {
      const auto& fn = decl.as<FunctionDecl>();
      name = fn.name;
      name_span = fn.name_span;
      kind = SymbolKind::Function;
      break;
    }
    case NodeKind::ClassDecl: {
      const auto& st = decl.as<ClassDecl>();
      name = st.name;
      name_span = st.name_span;
      kind = SymbolKind::Type;
      break;
    }
    case NodeKind::EnumDecl: {
      const auto& en = decl.as<EnumDeclNode>();
      name = en.name;
      name_span = en.name_span;
      kind = SymbolKind::Type;
      break;
    }
    case NodeKind::AliasDecl: {
      const auto& alias = decl.as<AliasDecl>();
      name = alias.name;
      name_span = alias.name_span;
      kind = SymbolKind::Type;
      break;
    }
    case NodeKind::ConceptDecl: {
      const auto& concept_ = decl.as<ConceptDecl>();
      name = concept_.name;
      name_span = concept_.name_span;
      kind = SymbolKind::Concept;
      break;
    }
    case NodeKind::ExtendDecl:
      // Extend declarations don't introduce a new name — they attach
      // conformance to an existing type. Resolved in pass 2.
      return;
    default:
      return;
    }

    // Builtins and predeclared names cannot be redeclared by any module,
    // prelude included (CONTRACT_MODULE_SYSTEM.md §7.6).  Scope::declare
    // checks only its own scope, so the outer builtins scope is checked
    // here explicitly.
    if (builtins_->lookup_local(name) != nullptr) {
      diagnostics_.push_back(Diagnostic::error(
          name_span, "duplicate top-level declaration '" + std::string(name) + "'"));
      return;
    }

    // The reserved __dao_ prefix belongs to prelude modules, which
    // declare runtime hooks under it (CONTRACT_MODULE_SYSTEM.md §7.7).
    if (name.starts_with("__dao_") && !current_->is_prelude) {
      diagnostics_.push_back(Diagnostic::error(
          name_span,
          "'" + std::string(name) +
              "': the '__dao_' prefix is reserved for compiler/runtime use"));
      return;
    }

    auto* scope = current_->decls;
    auto* existing = scope->lookup_local(name);
    if (existing != nullptr) {
      // Allow arity-based function overloading: same name, different
      // parameter counts. Both must be functions.
      if (kind == SymbolKind::Function &&
          existing->kind == SymbolKind::Function &&
          decl.kind() == NodeKind::FunctionDecl &&
          existing->decl != nullptr) {
        size_t new_arity = decl.as<FunctionDecl>().params.size();
        const auto* existing_decl = existing->decl_as_decl();

        // Check arity collision against the overload set AND the
        // original declaration.
        bool collision = overload_has_arity(name, new_arity);
        if (!collision && existing_decl->is<FunctionDecl>() &&
            existing_decl->as<FunctionDecl>().params.size() == new_arity) {
          collision = true;
        }

        if (!collision) {
          // Register the new overload with a mangled internal name.
          auto mangled = ctx_.intern(
              std::string(name) + "$" + std::to_string(new_arity));
          auto* sym = new_symbol(kind, mangled, name_span, &decl);
          publish_overload(scope, name, mangled, sym);

          // Bootstrap the overload set with the original declaration
          // if this is the first overload being added.
          if (scope->lookup_overloads(name) != nullptr &&
              scope->lookup_overloads(name)->size() == 1) {
            size_t orig_arity =
                existing_decl->as<FunctionDecl>().params.size();
            auto orig_mangled = ctx_.intern(
                std::string(name) + "$" + std::to_string(orig_arity));
            publish_overload(scope, name, orig_mangled, existing);
          }
          return;
        }
      }
      diagnostics_.push_back(Diagnostic::error(
          name_span,
          "duplicate top-level declaration '" + std::string(name) + "'"));
    } else {
      auto* sym = new_symbol(kind, name, name_span, &decl);
      publish(scope, name, sym);
    }
    if (decl.is<ClassDecl>()) {
      declare_class_methods(decl.as<ClassDecl>(), scope);
    }
  }

  /// Declare a top-level name into the unit's lexical scope and into its
  /// module's export table.  The two are the same scope except in a
  /// prelude module, which declares into the shared prelude scope.
  void publish(Scope* scope, std::string_view name, Symbol* sym) {
    scope->declare(name, sym);
    if (current_ != nullptr && current_->exports != nullptr && current_->exports != scope) {
      current_->exports->declare(name, sym);
    }
  }

  void
  publish_overload(Scope* scope, std::string_view base, std::string_view mangled, Symbol* sym) {
    scope->declare_overload(base, mangled, sym);
    if (current_ != nullptr && current_->exports != nullptr && current_->exports != scope) {
      current_->exports->declare_overload(base, mangled, sym);
    }
  }

  /// A class's direct methods are top-level names in mangled form
  /// ("Vector.push"): HIR method desugaring and `Type::method` calls
  /// find them by name, from any module, so they are declared in pass 1
  /// with the class rather than when its body is resolved.
  void declare_class_methods(const ClassDecl& st, Scope* scope) {
    for (const auto* method : st.methods) {
      const auto& fn_decl = method->as<FunctionDecl>();
      auto mangled_name = ctx_.intern(std::string(st.name) + "." + std::string(fn_decl.name));
      auto* method_sym = new_symbol(SymbolKind::Function, mangled_name, fn_decl.name_span, method);
      publish(scope, mangled_name, method_sym);
    }
  }

  // --- Overload helpers ---

  /// Check if any overload of `name` has the given parameter count.
  auto overload_has_arity(std::string_view name, size_t arity) -> bool {
    const auto* overloads = current_->decls->lookup_overloads(name);
    if (overloads != nullptr) {
      for (const auto* sym : *overloads) {
        if (sym->decl != nullptr) {
          const auto* decl = sym->decl_as_decl();
          if (decl->is<FunctionDecl>() &&
              decl->as<FunctionDecl>().params.size() == arity) {
            return true;
          }
        }
      }
    }
    return false;
  }

  /// Find the overload of `name` with the given arity in the scope chain.
  /// Returns nullptr if no match.
  auto find_overload_by_arity(std::string_view name, size_t arity,
                               Scope* scope) -> Symbol* {
    const auto* overloads = scope->find_overloads(name);
    if (overloads == nullptr) {
      return nullptr;
    }
    for (auto* sym : *overloads) {
      if (sym->decl != nullptr) {
        const auto* decl = sym->decl_as_decl();
        if (decl->is<FunctionDecl>() &&
            decl->as<FunctionDecl>().params.size() == arity) {
          return sym;
        }
      }
    }
    return nullptr;
  }

  /// The exported overload of `name` with `arity` in a module's export
  /// table, or null.  The table is flat — a module's exports are its own
  /// declarations — so this looks locally rather than up a scope chain.
  auto find_export_overload(const Scope* exports, std::string_view name, size_t arity) -> Symbol* {
    const auto* overloads = exports->lookup_overloads(name);
    if (overloads == nullptr) {
      return nullptr;
    }
    for (auto* sym : *overloads) {
      if (sym->decl == nullptr) {
        continue;
      }
      const auto* decl = sym->decl_as_decl();
      if (decl->is<FunctionDecl>() && decl->as<FunctionDecl>().params.size() == arity) {
        return sym;
      }
    }
    return nullptr;
  }

  /// `b::f(...)` where `f` is overloaded: bind the overload the call's
  /// arity names.  Without this the bare first declaration answers every
  /// arity, so which overload an importer reaches depends on declaration
  /// order and the rest are unreachable (CONTRACT_MODULE_SYSTEM.md §6).
  void rebind_qualified_overload(const Expr& callee, size_t arity, Scope* scope) {
    if (!callee.is<QualifiedName>()) {
      return;
    }
    const auto& qn = callee.as<QualifiedName>();
    if (qn.segments.size() != 2) {
      return; // `b::T::m` is a static method, not an overload set
    }
    auto* binding = scope->lookup(qn.segments[0]);
    if (binding == nullptr || binding->kind != SymbolKind::Module) {
      return;
    }
    const auto* target = binding->decl_as_module();
    if (target == nullptr || target->exports == nullptr) {
      return;
    }
    auto name_offset = callee.span.offset + static_cast<uint32_t>(qn.segments[0].size()) + 2;
    if (auto* match = find_export_overload(target->exports, qn.segments[1], arity)) {
      uses_[name_offset] = match;
    }
  }

  /// Try to resolve an identifier to an overloaded function by arity.
  /// If the name is overloaded and a match is found, records the use
  /// and returns true. Otherwise returns false (caller should fall
  /// through to normal resolution).
  auto try_resolve_overload(const Expr& ident_expr,
                             std::string_view name, size_t arity,
                             Scope* scope) -> bool {
    if (!scope->has_overloads(name)) {
      return false;
    }
    auto* match = find_overload_by_arity(name, arity, scope);
    if (match != nullptr) {
      uses_[ident_expr.span.offset] = match;
      return true;
    }
    return false;
  }

  // --- Pass 2: Resolve bodies ---

  void resolve_bodies(const Unit& unit) {
    for (const auto* decl : unit.file->declarations) {
      resolve_decl(*decl, unit.scope);
    }
  }

  void resolve_decl(const Decl& decl, Scope* scope) {
    switch (decl.kind()) {
    case NodeKind::FunctionDecl:
      resolve_function(decl, scope);
      break;
    case NodeKind::ClassDecl:
      resolve_class(decl, scope);
      break;
    case NodeKind::EnumDecl: {
      // Resolve payload type nodes in variant declarations.
      // Create a scope for generic type parameters if present.
      const auto& en = decl.as<EnumDeclNode>();
      auto* enum_scope = scope;
      if (!en.type_params.empty()) {
        enum_scope = ctx_.make_scope(ScopeKind::Struct, scope);
        enum_scope->set_range(decl.span);
        declare_type_params(en.type_params, enum_scope, decl);
      }
      for (const auto& variant : en.variants) {
        for (const auto* type_node : variant.payload_types) {
          resolve_type(*type_node, enum_scope);
        }
      }
      break;
    }
    case NodeKind::AliasDecl:
      resolve_alias(decl, scope);
      break;
    case NodeKind::ConceptDecl:
      resolve_concept(decl, scope);
      break;
    case NodeKind::ExtendDecl:
      resolve_extend(decl, scope);
      break;
    default:
      break;
    }
  }

  void declare_type_params(const std::vector<GenericParam>& type_params,
                           Scope* scope, const Decl& decl) {
    for (const auto& tp : type_params) {
      if (scope->lookup_local(tp.name) != nullptr) {
        diagnostics_.push_back(Diagnostic::error(
            tp.name_span,
            "duplicate type parameter '" + std::string(tp.name) + "'"));
      } else {
        auto* sym = new_symbol(
            SymbolKind::GenericParam, tp.name, tp.name_span, &decl);
        scope->declare(tp.name, sym);
      }
      // Resolve constraint types.
      for (const auto* constraint : tp.constraints) {
        resolve_type(*constraint, scope);
      }
    }
  }

  void resolve_function(const Decl& decl, Scope* parent) {
    const auto& fn = decl.as<FunctionDecl>();
    auto* fn_scope = ctx_.make_scope(ScopeKind::Function, parent);
    fn_scope->set_range(decl.span);

    // Declare generic type parameters (visible to params, return type, body).
    declare_type_params(fn.type_params, fn_scope, decl);

    // Declare parameters.
    for (const auto& param : fn.params) {
      if (fn_scope->lookup_local(param.name) != nullptr) {
        diagnostics_.push_back(Diagnostic::error(
            param.name_span,
            "duplicate parameter '" + std::string(param.name) + "'"));
      } else {
        auto* sym = new_symbol(SymbolKind::Param, param.name, param.name_span, &decl);
        fn_scope->declare(param.name, sym);
      }

      // Resolve parameter type (type-position, no diagnostic on unknown).
      if (param.type != nullptr) {
        resolve_type(*param.type, fn_scope);
      }
    }

    // Resolve return type.
    if (fn.return_type != nullptr) {
      resolve_type(*fn.return_type, fn_scope);
    }

    // Resolve body statements in a nested block scope so that let
    // bindings can shadow parameters without triggering duplicate errors.
    auto* body_scope = ctx_.make_scope(ScopeKind::Block, fn_scope);
    body_scope->set_range(decl.span);
    for (const auto* stmt : fn.body) {
      resolve_stmt(*stmt, body_scope);
    }

    // Resolve expression body (same body scope).
    if (fn.expr_body != nullptr) {
      resolve_expr(*fn.expr_body, body_scope);
    }
  }

  void resolve_class(const Decl& decl, Scope* parent) {
    const auto& st = decl.as<ClassDecl>();
    auto* struct_scope = ctx_.make_scope(ScopeKind::Struct, parent);
    struct_scope->set_range(decl.span);

    // Declare generic type parameters (visible to field types).
    declare_type_params(st.type_params, struct_scope, decl);

    for (const auto* field : st.fields) {
      if (struct_scope->lookup_local(field->name) != nullptr) {
        diagnostics_.push_back(Diagnostic::error(
            field->name_span,
            "duplicate declaration '" + std::string(field->name) + "'"));
      } else {
        auto* sym =
            new_symbol(SymbolKind::Field, field->name, field->name_span, field);
        struct_scope->declare(field->name, sym);
      }

      if (field->type != nullptr) {
        resolve_type(*field->type, struct_scope);
      }
    }

    // Method bodies; their symbols were declared with the class in pass 1.
    for (const auto* method : st.methods) {
      resolve_function(*method, struct_scope);
    }

    // Resolve conformance blocks — concept name + method signatures.
    for (const auto& conf : st.conformances) {
      resolve_conformance_target(conf, parent);
      for (const auto* method : conf.methods) {
        resolve_function(*method, struct_scope);
      }
    }

    // Resolve deny specs — concept name lookup only.
    for (const auto& deny : st.denials) {
      resolve_conformance_target(deny, parent);
    }
  }

  /// Resolve the concept a conformance position names and record it at
  /// its own segment, so the checker compares concepts by identity.
  /// `b::Concept` reaches the binding's module exports; an unqualified
  /// name is looked up in scope (CONTRACT_MODULE_SYSTEM.md §6).
  template <typename Target> void resolve_conformance_target(const Target& target, Scope* scope) {
    if (target.module_binding.empty()) {
      if (auto* sym = scope->lookup(target.concept_name)) {
        uses_[target.concept_span.offset] = sym;
      }
      return;
    }
    auto* binding = scope->lookup(target.module_binding);
    if (binding == nullptr || binding->kind != SymbolKind::Module) {
      diagnostics_.push_back(Diagnostic::error(target.binding_span,
                                               "'" + std::string(target.module_binding) +
                                                   "' is not an imported module"));
      return;
    }
    uses_[target.binding_span.offset] = binding;
    const auto* module = binding->decl_as_module();
    if (module == nullptr || module->exports == nullptr) {
      return; // no program, or an import the graph already reported missing
    }
    auto* exported = module->exports->lookup_local(target.concept_name);
    if (exported == nullptr || exported->kind != SymbolKind::Concept) {
      diagnostics_.push_back(Diagnostic::error(target.concept_span,
                                               "module '" + module->display + "' has no concept '" +
                                                   std::string(target.concept_name) + "'"));
      return;
    }
    uses_[target.concept_span.offset] = exported;
  }

  void resolve_alias(const Decl& decl, Scope* scope) {
    const auto& alias = decl.as<AliasDecl>();
    if (alias.type != nullptr) {
      resolve_type(*alias.type, scope);
    }
  }

  void resolve_concept(const Decl& decl, Scope* parent) {
    const auto& concept_ = decl.as<ConceptDecl>();
    auto* concept_scope = ctx_.make_scope(ScopeKind::Function, parent);
    concept_scope->set_range(decl.span);

    // Declare generic type parameters.
    declare_type_params(concept_.type_params, concept_scope, decl);

    // Resolve method signatures.
    for (const auto* method : concept_.methods) {
      resolve_function(*method, concept_scope);
    }
  }

  void resolve_extend(const Decl& decl, Scope* parent) {
    const auto& ext = decl.as<ExtendDecl>();

    // Resolve the target type.
    if (ext.target_type != nullptr) {
      resolve_type(*ext.target_type, parent);
    }

    resolve_conformance_target(ext, parent);

    // Extract target type name for method symbol mangling.
    // Must include type arguments to match print_type() output used
    // by the monomorphization fixup (e.g. "Generator<i32>.method").
    auto format_type_node = [](const TypeNode* node, auto& self) -> std::string {
      if (node == nullptr) { return {}; }
      if (node->is<NamedType>()) {
        const auto& named = node->as<NamedType>();
        std::string result;
        for (size_t seg = 0; seg < named.name.segments.size(); ++seg) {
          if (seg > 0) { result += "::"; }
          result += named.name.segments[seg];
        }
        if (!named.type_args.empty()) {
          result += "<";
          for (size_t arg = 0; arg < named.type_args.size(); ++arg) {
            if (arg > 0) { result += ", "; }
            result += self(named.type_args[arg], self);
          }
          result += ">";
        }
        return result;
      }
      if (node->is<PointerType>()) {
        return "*" + self(node->as<PointerType>().pointee, self);
      }
      if (node->is<FunctionTypeNode>()) {
        const auto& ftn = node->as<FunctionTypeNode>();
        std::string result = "fn(";
        for (size_t pidx = 0; pidx < ftn.param_types.size(); ++pidx) {
          if (pidx > 0) { result += ", "; }
          result += self(ftn.param_types[pidx], self);
        }
        result += "): ";
        result += self(ftn.return_type, self);
        return result;
      }
      return {};
    };
    auto target_name = format_type_node(ext.target_type, format_type_node);

    // Resolve method signatures and create Function symbols.
    for (const auto* method : ext.methods) {
      resolve_function(*method, parent);

      // Create a Function symbol with mangled name so HIR/MIR can
      // reference this extend method as a real function.
      // Name format: "<type>.<method>" (e.g. "i32.to_string").
      if (!target_name.empty()) {
        const auto& fn_decl = method->as<FunctionDecl>();
        auto mangled_name = ctx_.intern(
            target_name + "." + std::string(fn_decl.name));
        new_symbol(SymbolKind::Function, mangled_name,
                         fn_decl.name_span, method);
      }
    }
  }

  // --- Statements ---

  void resolve_stmt(const Stmt& stmt, Scope* scope) {
    switch (stmt.kind()) {
    case NodeKind::LetStatement: {
      const auto& let_stmt = stmt.as<LetStatement>();

      // Resolve type and initializer BEFORE declaring (prevents self-reference).
      if (let_stmt.type != nullptr) {
        resolve_type(*let_stmt.type, scope);
      }
      if (let_stmt.initializer != nullptr) {
        resolve_expr(*let_stmt.initializer, scope);
      }

      // Declare the local variable (visible after this point).
      if (scope->lookup_local(let_stmt.name) != nullptr) {
        diagnostics_.push_back(Diagnostic::error(
            let_stmt.name_span,
            "duplicate declaration '" + std::string(let_stmt.name) + "'"));
      } else {
        auto* sym =
            new_symbol(SymbolKind::Local, let_stmt.name, let_stmt.name_span, &stmt);
        scope->declare(let_stmt.name, sym);
      }
      break;
    }
    case NodeKind::Assignment: {
      const auto& assign = stmt.as<Assignment>();
      resolve_expr(*assign.target, scope);
      resolve_expr(*assign.value, scope);
      break;
    }
    case NodeKind::IfStatement: {
      const auto& if_stmt = stmt.as<IfStatement>();
      resolve_expr(*if_stmt.condition, scope);

      auto* then_scope = ctx_.make_scope(ScopeKind::Block, scope);
      if (!if_stmt.then_body.empty()) {
        auto first = if_stmt.then_body.front()->span;
        auto last = if_stmt.then_body.back()->span;
        then_scope->set_range(
            {first.offset, last.offset + last.length - first.offset});
      }
      for (const auto* s : if_stmt.then_body) {
        resolve_stmt(*s, then_scope);
      }

      if (if_stmt.has_else()) {
        auto* else_scope = ctx_.make_scope(ScopeKind::Block, scope);
        if (!if_stmt.else_body.empty()) {
          auto first = if_stmt.else_body.front()->span;
          auto last = if_stmt.else_body.back()->span;
          else_scope->set_range(
              {first.offset, last.offset + last.length - first.offset});
        }
        for (const auto* s : if_stmt.else_body) {
          resolve_stmt(*s, else_scope);
        }
      }
      break;
    }
    case NodeKind::WhileStatement: {
      const auto& while_stmt = stmt.as<WhileStatement>();
      resolve_expr(*while_stmt.condition, scope);

      auto* while_scope = ctx_.make_scope(ScopeKind::Block, scope);
      while_scope->set_range(stmt.span);
      for (const auto* s : while_stmt.body) {
        resolve_stmt(*s, while_scope);
      }
      break;
    }
    case NodeKind::ForStatement: {
      const auto& for_stmt = stmt.as<ForStatement>();

      // Resolve iterable in the outer scope.
      resolve_expr(*for_stmt.iterable, scope);

      // Create block scope for the loop body; declare the loop variable.
      auto* for_scope = ctx_.make_scope(ScopeKind::Block, scope);
      for_scope->set_range(stmt.span);
      auto* sym = new_symbol(
          SymbolKind::Local, for_stmt.var, for_stmt.var_span, &stmt);
      for_scope->declare(for_stmt.var, sym);

      for (const auto* s : for_stmt.body) {
        resolve_stmt(*s, for_scope);
      }
      break;
    }
    case NodeKind::ModeBlock: {
      const auto& mode = stmt.as<ModeBlock>();
      auto* mode_scope = ctx_.make_scope(ScopeKind::Block, scope);
      mode_scope->set_range(stmt.span);
      for (const auto* s : mode.body) {
        resolve_stmt(*s, mode_scope);
      }
      break;
    }
    case NodeKind::ResourceBlock: {
      const auto& res = stmt.as<ResourceBlock>();
      auto* res_scope = ctx_.make_scope(ScopeKind::Block, scope);
      res_scope->set_range(stmt.span);
      for (const auto* s : res.body) {
        resolve_stmt(*s, res_scope);
      }
      break;
    }
    case NodeKind::YieldStatement: {
      const auto& yield = stmt.as<YieldStatement>();
      resolve_expr(*yield.value, scope);
      break;
    }
    case NodeKind::ReturnStatement: {
      const auto& ret = stmt.as<ReturnStatement>();
      if (ret.value != nullptr) {
        resolve_expr(*ret.value, scope);
      }
      break;
    }
    case NodeKind::MatchStatement: {
      const auto& match = stmt.as<MatchStmt>();
      resolve_expr(*match.scrutinee, scope);
      for (const auto& arm : match.arms) {
        resolve_expr(*arm.pattern, scope);
        auto* arm_scope = ctx_.make_scope(ScopeKind::Block, scope);
        // Register destructuring bindings as locals in the arm scope.
        for (size_t i = 0; i < arm.bindings.size(); ++i) {
          auto* sym = new_symbol(
              SymbolKind::Local, arm.bindings[i], arm.binding_spans[i],
              nullptr);
          arm_scope->declare(arm.bindings[i], sym);
        }
        // Register `as` binding: `Pattern as name:` binds the whole value.
        if (!arm.as_binding.empty()) {
          auto* as_sym = new_symbol(
              SymbolKind::Local, arm.as_binding, arm.as_binding_span,
              nullptr);
          arm_scope->declare(arm.as_binding, as_sym);
        }
        for (const auto* body_stmt : arm.body) {
          resolve_stmt(*body_stmt, arm_scope);
        }
      }
      break;
    }
    case NodeKind::ExpressionStatement: {
      const auto& expr_stmt = stmt.as<ExpressionStatement>();
      resolve_expr(*expr_stmt.expr, scope);
      break;
    }
    default:
      break;
    }
  }

  /// `b::name`, `b::E::V`, and `b::T::m` through an import binding
  /// (CONTRACT_MODULE_SYSTEM.md §6).  The binding is recorded at the
  /// head segment and each resolved segment at its own offset, so
  /// tooling paints the segments and the type checker finds the export.
  void resolve_through_binding(const Expr& expr, const QualifiedName& qn, Symbol* binding) {
    uses_[expr.span.offset] = binding;
    const auto* target = binding->decl_as_module();
    if (target == nullptr) {
      return; // no program, or an import the graph already reported missing
    }
    auto path_text = [&] {
      std::string text;
      for (auto segment : qn.segments) {
        text += (text.empty() ? "" : "::") + std::string(segment);
      }
      return text;
    };
    if (qn.segments.size() > 3) {
      diagnostics_.push_back(Diagnostic::error(
          expr.span, "'" + path_text() + "': a path through import binding '" +
                         std::string(binding->name) +
                         "' reaches at most a type's member (imports bind one segment)"));
      return;
    }

    // A module's export table is its own declarations, prelude module
    // included: `import core::vector` reaches what `core::vector`
    // declares, not what the prelude as a whole does (§7.5).  Import
    // bindings are not re-exported (§3.2).
    const auto* exports = target->exports;
    auto name = qn.segments[1];
    auto name_offset = expr.span.offset + static_cast<uint32_t>(qn.segments[0].size()) + 2;
    auto* exported = exports->lookup_local(name);
    if (exported == nullptr || exported->kind == SymbolKind::Module) {
      diagnostics_.push_back(Diagnostic::error(
          Span{.offset = name_offset, .length = static_cast<uint32_t>(name.size())},
          "module '" + target->display + "' has no export '" + std::string(name) + "'"));
      return;
    }
    uses_[name_offset] = exported;
    if (qn.segments.size() == 2) {
      return;
    }

    // b::T::m is a static method of exported type T; b::E::V an enum
    // variant, recorded as the type for the checker to validate.
    auto member = qn.segments[2];
    auto member_offset = name_offset + static_cast<uint32_t>(name.size()) + 2;
    if (exported->kind != SymbolKind::Type) {
      diagnostics_.push_back(Diagnostic::error(
          Span{.offset = member_offset, .length = static_cast<uint32_t>(member.size())},
          "'" + std::string(name) + "' of module '" + target->display + "' is not a type"));
      return;
    }
    auto mangled = ctx_.intern(std::string(name) + "." + std::string(member));
    if (auto* method = exports->lookup_local(mangled)) {
      uses_[member_offset] = method;
      return;
    }
    // No such static method.  An enum's `b::E::V` names a variant, which
    // has no symbol of its own — the checker validates it against the
    // enum — so the type stands in for it there.  For anything else the
    // member does not exist, and recording the type would let the
    // checker read `b::T::missing(...)` as a construction of `T`.
    const auto* decl = exported->decl == nullptr ? nullptr : exported->decl_as_decl();
    if (decl != nullptr && decl->is<EnumDeclNode>()) {
      uses_[member_offset] = exported;
      return;
    }
    diagnostics_.push_back(Diagnostic::error(
        Span{.offset = member_offset, .length = static_cast<uint32_t>(member.size())},
        "type '" + std::string(name) + "' of module '" + target->display +
            "' has no static member '" + std::string(member) + "'"));
  }

  // --- Expressions ---

  void resolve_expr(const Expr& expr, Scope* scope) {
    switch (expr.kind()) {
    case NodeKind::Identifier: {
      const auto& ident = expr.as<IdentifierExpr>();
      auto* sym = scope->lookup(ident.name);
      if (sym != nullptr) {
        uses_[expr.span.offset] = sym;
      } else {
        diagnostics_.push_back(Diagnostic::error(
            expr.span,
            "unknown name '" + std::string(ident.name) + "'"));
      }
      break;
    }
    case NodeKind::QualifiedName: {
      const auto& qn = expr.as<QualifiedName>();
      if (qn.segments.empty()) {
        break;
      }

      // Resolve first segment against the scope chain — must be a
      // module/import binding per TASK_6_RESOLVE.md.
      auto first_seg = qn.segments.front();
      auto seg_len = static_cast<uint32_t>(first_seg.size());
      Span seg_span{.offset = expr.span.offset, .length = seg_len};
      auto* sym = scope->lookup(first_seg);

      if (sym == nullptr) {
        diagnostics_.push_back(Diagnostic::error(
            seg_span,
            "unknown name '" + std::string(first_seg) + "'"));
      } else if (sym->kind == SymbolKind::Type &&
                 qn.segments.size() == 2) {
        // Static method call or enum variant: Type::method / Enum::Variant.
        // Try mangled method name first; fall back to the type symbol
        // (the type checker handles enum variant resolution).
        auto mangled_name = ctx_.intern(
            std::string(qn.segments[0]) + "." + std::string(qn.segments[1]));
        auto* method_sym = scope->lookup(mangled_name);
        if (method_sym != nullptr) {
          uses_[expr.span.offset] = method_sym;
        } else {
          // Resolve to the type symbol — the type checker will validate
          // whether the second segment is a valid enum variant.
          uses_[expr.span.offset] = sym;
        }
      } else if (sym->kind != SymbolKind::Module) {
        diagnostics_.push_back(Diagnostic::error(
            seg_span,
            "'" + std::string(first_seg) + "' is not a module"));
      } else {
        resolve_through_binding(expr, qn, sym);
      }
      break;
    }
    case NodeKind::BinaryExpr: {
      const auto& bin = expr.as<BinaryExpr>();
      resolve_expr(*bin.left, scope);
      resolve_expr(*bin.right, scope);
      break;
    }
    case NodeKind::UnaryExpr: {
      const auto& unary = expr.as<UnaryExpr>();
      resolve_expr(*unary.operand, scope);
      break;
    }
    case NodeKind::CallExpr: {
      const auto& call = expr.as<CallExpr>();
      // For overloaded functions, select the overload matching the
      // argument count. Falls through to normal resolution otherwise.
      bool resolved = false;
      if (call.callee->is<IdentifierExpr>()) {
        resolved = try_resolve_overload(
            *call.callee, call.callee->as<IdentifierExpr>().name,
            call.args.size(), scope);
      }
      if (!resolved) {
        resolve_expr(*call.callee, scope);
        rebind_qualified_overload(*call.callee, call.args.size(), scope);
      }
      for (const auto* arg : call.args) {
        resolve_expr(*arg, scope);
      }
      // Resolve explicit type arguments (e.g., size_of<T>()).
      for (const auto* ta : call.type_args) {
        resolve_type(*ta, scope);
      }
      break;
    }
    case NodeKind::IndexExpr: {
      const auto& idx = expr.as<IndexExpr>();
      resolve_expr(*idx.object, scope);
      for (const auto* i : idx.indices) {
        resolve_expr(*i, scope);
      }
      break;
    }
    case NodeKind::FieldExpr: {
      const auto& field = expr.as<FieldExpr>();
      resolve_expr(*field.object, scope);
      // Field member access is not resolved in Task 6 (needs type info).
      break;
    }
    case NodeKind::PipeExpr: {
      const auto& pipe = expr.as<PipeExpr>();
      resolve_expr(*pipe.left, scope);
      // Pipe passes the LHS as a single argument → effective arity 1.
      bool resolved = false;
      if (pipe.right->is<IdentifierExpr>()) {
        resolved = try_resolve_overload(
            *pipe.right, pipe.right->as<IdentifierExpr>().name,
            1, scope);
      }
      if (!resolved) {
        resolve_expr(*pipe.right, scope);
      }
      break;
    }
    case NodeKind::TryExpr: {
      const auto& try_expr = expr.as<TryExpr>();
      resolve_expr(*try_expr.operand, scope);
      break;
    }
    case NodeKind::Lambda: {
      const auto& lam = expr.as<LambdaExpr>();

      // Create a block scope for the lambda body.
      auto* lam_scope = ctx_.make_scope(ScopeKind::Block, scope);
      lam_scope->set_range(expr.span);
      for (const auto& [name, span] : lam.params) {
        if (lam_scope->lookup_local(name) != nullptr) {
          diagnostics_.push_back(Diagnostic::error(
              span,
              "duplicate parameter '" + std::string(name) + "'"));
        } else {
          auto* sym = new_symbol(SymbolKind::LambdaParam, name, span, &expr);
          lam_scope->declare(name, sym);
        }
      }
      resolve_expr(*lam.body, lam_scope);
      break;
    }
    case NodeKind::ListLiteral: {
      const auto& list = expr.as<ListLiteral>();
      for (const auto* elem : list.elements) {
        resolve_expr(*elem, scope);
      }
      break;
    }
    // Terminals — no resolution needed.
    case NodeKind::IntLiteral:
    case NodeKind::FloatLiteral:
    case NodeKind::StringLiteral:
    case NodeKind::BoolLiteral:
      break;
    default:
      break;
    }
  }

  // --- Types ---

  /// `b::T` in type position (CONTRACT_MODULE_SYSTEM.md §6): the binding
  /// is recorded at the head and the exported type at `T`'s own offset,
  /// where the type checker reads it.  A head that is not a module
  /// binding is left undiagnosed, as unknown names in type position are.
  void resolve_qualified_type(const QualifiedPath& path, Scope* scope) {
    auto first_seg = path.segments.front();
    auto* binding = scope->lookup(first_seg);
    if (binding == nullptr || binding->kind != SymbolKind::Module) {
      return;
    }
    uses_[path.span.offset] = binding;
    const auto* target = binding->decl_as_module();
    if (target == nullptr) {
      return; // no program, or an import the graph already reported missing
    }
    if (path.segments.size() > 2) {
      diagnostics_.push_back(Diagnostic::error(
          path.span, "'" + module_display(path.segments) + "': a type path through import binding '" +
                         std::string(first_seg) + "' has one more segment (imports bind one segment)"));
      return;
    }
    auto name = path.segments[1];
    auto name_offset = path.span.offset + static_cast<uint32_t>(first_seg.size()) + 2;
    // A module's exports are its own declarations, prelude module
    // included (CONTRACT_MODULE_SYSTEM.md §7.5).
    const auto* exports = target->exports;
    auto* exported = exports->lookup_local(name);
    if (exported == nullptr || exported->kind == SymbolKind::Module) {
      diagnostics_.push_back(Diagnostic::error(
          Span{.offset = name_offset, .length = static_cast<uint32_t>(name.size())},
          "module '" + target->display + "' has no export '" + std::string(name) + "'"));
      return;
    }
    uses_[name_offset] = exported;
  }

  void resolve_type(const TypeNode& type, Scope* scope) {
    switch (type.kind()) {
    case NodeKind::NamedType: {
      const auto& named = type.as<NamedType>();
      const auto& path = named.name;

      if (path.segments.empty()) {
        break;
      }

      if (path.segments.size() == 1) {
        // Single-segment type: look up in scope chain. No diagnostic if
        // not found (type-position references are allowed unresolved).
        auto type_name = path.segments.front();
        auto* sym = scope->lookup(type_name);
        if (sym != nullptr) {
          uses_[path.span.offset] = sym;
        }
      } else {
        resolve_qualified_type(path, scope);
      }

      // Resolve type arguments recursively.
      for (const auto* arg : named.type_args) {
        resolve_type(*arg, scope);
      }
      break;
    }
    case NodeKind::PointerType: {
      const auto& ptr = type.as<PointerType>();
      resolve_type(*ptr.pointee, scope);
      break;
    }
    case NodeKind::FunctionType: {
      const auto& ftn = type.as<FunctionTypeNode>();
      for (const auto* param_type : ftn.param_types) {
        resolve_type(*param_type, scope);
      }
      resolve_type(*ftn.return_type, scope);
      break;
    }
    default:
      break;
    }
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

auto ResolveResult::symbol_for(const Expr& expr) const -> const Symbol* {
  auto at = [&](uint32_t offset) -> const Symbol* {
    auto it = uses.find(offset);
    return it == uses.end() ? nullptr : it->second;
  };
  const auto* head = at(expr.span.offset);
  if (head == nullptr || !expr.is<QualifiedName>()) {
    return head;
  }
  const auto& qn = expr.as<QualifiedName>();
  if (head->kind != SymbolKind::Module || qn.segments.size() < 2) {
    return head;
  }
  auto export_offset = expr.span.offset + static_cast<uint32_t>(qn.segments[0].size()) + 2;
  const auto* exported = at(export_offset);
  if (exported == nullptr || qn.segments.size() < 3) {
    return exported;
  }
  return at(export_offset + static_cast<uint32_t>(qn.segments[1].size()) + 2);
}

auto resolve(std::span<const FileNode* const> files, const SourceMap* source_map)
    -> ResolveResult {
  Resolver resolver;
  return resolver.run(files, source_map);
}

auto resolve(Program& program) -> ResolveResult {
  Resolver resolver;
  return resolver.run(program);
}

auto resolve(const FileNode& file) -> ResolveResult {
  const FileNode* files[] = {&file};
  return resolve(std::span<const FileNode* const>(files), nullptr);
}

} // namespace dao
