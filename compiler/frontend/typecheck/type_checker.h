#ifndef DAO_FRONTEND_TYPECHECK_TYPE_CHECKER_H
#define DAO_FRONTEND_TYPECHECK_TYPE_CHECKER_H

#include "frontend/ast/ast.h"
#include "frontend/diagnostics/diagnostic.h"
#include "frontend/module/program.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/typed_results.h"
#include "frontend/types/type_context.h"
#include "frontend/types/type_printer.h"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// TypeCheckResult — output of the type-checking pass.
// ---------------------------------------------------------------------------

/// A method available on a type via concept/extend.
struct MethodInfo {
  const Type* receiver_type;
  std::string_view method_name;
  const Type* method_type;                   // function type (self removed)
  const ModuleInfo* extend_module = nullptr; // set only for `extend` methods
  // Declared by the type itself: outranks every extension of the same
  // name, so tooling offers it alone where a call would select it.
  bool inherent = false;
};

struct TypeCheckResult {
  TypedResults typed;
  std::vector<Diagnostic> diagnostics;
  std::vector<MethodInfo> methods; // exported method table for tooling
};

// ---------------------------------------------------------------------------
// CheckContext — per-function / per-scope typing context.
// ---------------------------------------------------------------------------

/// A `resource memory` block being checked: the statement (its span is
/// the extent bindings declared inside it fall within, and the escapes
/// recorded for it key on it) and its name, for diagnostics.
struct ResourceBlockScope {
  const Stmt* stmt;
  std::string_view name;
};

struct CheckContext {
  const Type* return_type = nullptr; // enclosing function return type
  const Type* self_type = nullptr;   // type of `self` in current scope (class/extend)
  std::unordered_set<std::string_view> active_modes; // e.g. "unsafe"
  uint32_t loop_depth = 0;                           // nesting depth for break validation
  // The `resource memory` blocks enclosing the statement being checked,
  // outermost first.  A domain reclaims what was allocated inside it
  // when its block is left, so a heap-owning value may not leave one.
  std::vector<ResourceBlockScope> resource_blocks;
};

// ---------------------------------------------------------------------------
// TypeChecker — orchestrates type checking for a file.
//
// Consumes:
//   - parsed AST
//   - resolver results
//   - TypeContext for semantic type construction/interning
//
// Produces:
//   - TypeCheckResult (typed side tables + diagnostics)
// ---------------------------------------------------------------------------

class TypeChecker {
public:
  TypeChecker(TypeContext& types, const ResolveResult& resolve);

  // Check every file's declarations as one program: register all
  // declarations first, then check all bodies.  Files arrive in
  // topological module order; cross-module references need no
  // ordering because of the two passes.
  auto check(std::span<const FileNode* const> files) -> TypeCheckResult;

private:
  TypeContext& types_;
  const ResolveResult& resolve_;
  std::vector<const Decl*> all_decls_; // every file's top-level declarations, load order
  TypedResults typed_;
  std::vector<Diagnostic> diagnostics_;
  CheckContext ctx_;

  // Payload-bearing variant accesses that require constructor call syntax.
  // check_field inserts; check_call removes when it handles the construction.
  // Any remaining entries after expression processing are errors.
  std::unordered_set<const Expr*> pending_payload_constructions_;

  // Suppresses the "needs constructor syntax" check in check_expr when
  // processing match arm patterns (arity is checked separately) or when
  // the expression is the callee of a call (check_call validates after).
  bool suppress_payload_check_ = false;

  // Symbol -> semantic type cache (populated in pass 1).
  std::unordered_map<const Symbol*, const Type*> symbol_types_;

  // The symbol an identifier or qualified name denotes, following the
  // resolver's per-segment entries: `b::name` is the export recorded at
  // `name`, `b::T::m` the member recorded at `m` (CONTRACT_MODULE_SYSTEM.md
  // §6).  Null when unresolved.
  [[nodiscard]] auto symbol_for_use(const Expr* expr) const -> const Symbol*;

  /// The concept a generic bound names, reading a qualified bound at its
  /// last segment; null when the bound resolves to nothing.
  [[nodiscard]] auto concept_for_constraint(const TypeNode* constraint) const -> const Symbol*;

  /// True if the expression names a type rather than a member reached
  /// through one (`T` or `m::T`, never `T::m` or `m::T::m`).
  [[nodiscard]] auto names_a_type(const Expr* expr) const -> bool;

  /// The concept declaration named at `span` (an `as`, `deny`, or
  /// `extend ... as` clause), or null when the name resolves to no
  /// concept.  Two modules may each declare a concept called `Reveal`,
  /// so conformance is decided by which one, not by the spelling.
  [[nodiscard]] auto concept_named_at(Span span) const -> const Decl* {
    auto it = resolve_.uses.find(span.offset);
    if (it == resolve_.uses.end() || it->second->kind != SymbolKind::Concept) {
      return nullptr;
    }
    return it->second->decl_as_decl();
  }

  // decl_span.offset -> Symbol* for finding symbols at declaration sites.
  std::unordered_map<uint32_t, const Symbol*> decl_symbols_;

  // Derived concept tracking: ConceptDecl nodes marked `derived`.
  std::vector<const Decl*> derived_concepts_;

  // Derived conformances: type -> list of derived concept Decls it auto-conforms to.
  std::unordered_map<const Type*, std::vector<const Decl*>> derived_conformances_;

  // Concept self-type substitution: concept name -> conforming type.
  // When set, resolve_type_node substitutes the concept name with the
  // conforming type (§3.2: concept name in type position means the
  // conforming type).
  // Keyed by the concept DECLARATION, not its spelling: two modules may
  // each declare a concept named `C`, and substituting for the wrong one
  // silently retypes an expression (CONTRACT_TYPE_SYSTEM_FOUNDATIONS.md §11).
  std::unordered_map<const Decl*, const Type*> concept_self_map_;

  /// RAII guard that answers "which module is asking?" for the length
  /// of a scope and restores the previous answer after.  Visibility of
  /// an `extend` depends on it (§5), so a pass that consults it must
  /// say where it is standing.
  struct ModuleScope {
    const ModuleInfo*& slot;
    const ModuleInfo* previous;

    ModuleScope(const ModuleInfo*& current, const ModuleInfo* asking)
        : slot(current), previous(current) {
      slot = asking;
    }
    ~ModuleScope() {
      slot = previous;
    }
    ModuleScope(const ModuleScope&) = delete;
    auto operator=(const ModuleScope&) -> ModuleScope& = delete;
    ModuleScope(ModuleScope&&) = delete;
    auto operator=(ModuleScope&&) -> ModuleScope& = delete;
  };

  // RAII guard that saves and restores a single key in concept_self_map_.
  // Each guard scope inserts exactly one concept→type binding; on
  // destruction the prior state of that key is restored. O(1) instead
  // of copying the entire map.
  struct ConceptSelfMapGuard {
    using Map = std::unordered_map<const Decl*, const Type*>;
    Map& map;
    const Decl* key;
    const Type* old_value = nullptr;
    bool had_key = false;

    ConceptSelfMapGuard(Map& m, const Decl* k)
        : map(m), key(k) { // NOLINT(readability-identifier-length)
      auto iter = map.find(key);
      if (iter != map.end()) {
        had_key = true;
        old_value = iter->second;
      }
    }
    ~ConceptSelfMapGuard() {
      if (had_key) {
        map[key] = old_value;
      } else {
        map.erase(key);
      }
    }
    ConceptSelfMapGuard(const ConceptSelfMapGuard&) = delete;
    auto operator=(const ConceptSelfMapGuard&) -> ConceptSelfMapGuard& = delete;
  };

  // Pre-built method lookup table:
  // (type*, method_name, owner) -> {fn_type, decl}.
  struct MethodEntry {
    const Type* fn_type;     // method function type (self removed)
    const Decl* method_decl; // the FunctionDecl node for HIR resolution
    // Set only for a method introduced by `extend`, which participates
    // in lookup within its declaring module and, if that module is in
    // the prelude, everywhere (CONTRACT_MODULE_SYSTEM.md §5).  A class's
    // own methods travel with the type and leave this null.
    const ModuleInfo* extend_module = nullptr;
    // Declared by the type itself (a class or conformance-block method),
    // not introduced by an `extend`: innermost, shadowed by nothing.
    bool inherent = false;
  };

  struct MethodKey {
    const Type* type;
    std::string_view name;
    // The module whose `extend` block introduced the method, which is
    // the only module it participates in method-set lookup from
    // (CONTRACT_MODULE_SYSTEM.md §5).  Null means every module sees it:
    // a type's own methods, which travel with the type; a prelude
    // `extend`, the sole cross-module exception (§7.2); and any program
    // whose symbols carry no module identity, such as a single-file
    // test fixture.
    const ModuleInfo* owner = nullptr;
    auto operator==(const MethodKey&) const -> bool = default;
  };

  struct MethodKeyHash {
    auto operator()(const MethodKey& key) const -> size_t {
      auto h1 = std::hash<const void*>{}(key.type);
      auto h2 = std::hash<std::string_view>{}(key.name);
      auto h3 = std::hash<const void*>{}(static_cast<const void*>(key.owner));
      auto mixed = h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
      return mixed ^ (h3 * 0x9e3779b97f4a7c15ULL + 0x9e3779b9 + (mixed << 6) + (mixed >> 2));
    }
  };

  // (type, name) -> the methods declared for it.  More than one exists
  // when separate modules extend the same type with the same method
  // name; each is visible only where its own module makes it visible,
  // so they must coexist whatever order the modules are checked in.
  std::unordered_map<MethodKey, std::vector<MethodEntry>, MethodKeyHash> method_table_;

  /// The method a lookup from the current module should see, or null.
  /// Innermost-first (CONTRACT_MODULE_SYSTEM.md §7.4): the type's own
  /// method, then the current module's `extend`, then the prelude's --
  /// entries are stored prelude-first, so "first visible" would let a
  /// prelude extension shadow the module's own.
  [[nodiscard]] auto visible_entry(const std::vector<MethodEntry>& entries) const
      -> const MethodEntry* {
    // Innermost-first (§7.4): the type's own method, then the current
    // module's `extend`, then any visible extension (the prelude's).
    for (const auto& entry : entries) {
      if (entry.inherent) {
        return &entry;
      }
    }
    for (const auto& entry : entries) {
      if (entry.extend_module != nullptr && entry.extend_module == current_module_) {
        return &entry;
      }
    }
    for (const auto& entry : entries) {
      if (extend_is_visible(entry.extend_module)) {
        return &entry;
      }
    }
    return nullptr;
  }

  /// Record a method unless one with the same scope is already there
  /// (the first declaration of a name in a scope wins, as before).
  void add_method(const MethodKey& key, const MethodEntry& entry) {
    auto& entries = method_table_[key];
    for (const auto& existing : entries) {
      if (existing.extend_module == entry.extend_module) {
        return;
      }
    }
    entries.push_back(entry);
  }

  // Which module each top-level declaration came from, and the one whose
  // body is being checked.  Empty outside a program (single-file
  // checking), where every declaration is equally visible.
  std::unordered_map<const Decl*, const ModuleInfo*> decl_module_;
  const ModuleInfo* current_module_ = nullptr;

  /// True if an `extend` method declared in `owner` is in scope for the
  /// module being checked.
  [[nodiscard]] auto extend_is_visible(const ModuleInfo* owner) const -> bool {
    return extend_visible_from(owner, current_module_);
  }

  /// The module a top-level declaration belongs to, or null when
  /// checking outside a program.
  [[nodiscard]] auto declaring_module(const Decl* decl) const -> const ModuleInfo* {
    auto it = decl_module_.find(decl);
    return it == decl_module_.end() ? nullptr : it->second;
  }

public:
  /// Record which module each file belongs to, so `extend` scoping and
  /// diagnostics can name it.  Called before check() by the Program
  /// entry point; single-file checking leaves it empty.
  void set_file_modules(std::unordered_map<const FileNode*, const ModuleInfo*> file_modules) {
    file_modules_ = std::move(file_modules);
  }

private:
  std::unordered_map<const FileNode*, const ModuleInfo*> file_modules_;

  // Pending class shells awaiting field resolution (populated by
  // register_type_names, consumed by register_struct_fields).
  struct PendingClass {
    const ClassDecl* class_decl;
    const Decl* decl;
    TypeStruct* shell;
  };
  std::vector<PendingClass> pending_classes_;
  // The same, by declaration, for the on-demand pull in
  // aliases_generic_shell.  Filled once the list is complete.
  std::unordered_map<const Decl*, PendingClass*> pending_by_decl_;
  // Declarations whose registration is under way: one reached again
  // through its own chain (an alias naming itself through another, an
  // enum whose payload holds it) registers nothing the second time.
  std::unordered_set<const Decl*> registering_;
  // Types found complete by value (type_complete): slots only ever
  // fill, so the answer stands and the walk is not repeated.
  std::unordered_set<const Type*> complete_types_;
  // How deep the on-demand pull currently is; bounded (kPullDepthCap)
  // so a long chain cannot exhaust the stack.
  size_t pull_depth_ = 0;
  // Structs and enums substitute_generics is inside of: one reached
  // again holds itself by value, and is returned as it is.
  std::unordered_set<const Type*> substituting_;
  struct PullDepth {
    explicit PullDepth(TypeChecker& checker) : checker_(checker) {
      ++checker_.pull_depth_;
    }
    ~PullDepth() {
      --checker_.pull_depth_;
    }
    PullDepth(const PullDepth&) = delete;
    auto operator=(const PullDepth&) -> PullDepth& = delete;
    [[nodiscard]] static auto available(const TypeChecker& checker) -> bool;

  private:
    TypeChecker& checker_;
  };
  // Set once the registration fixpoint has run: aliases of generic
  // instantiations wait for it (see aliases_generic_shell).
  bool fields_registered_ = false;

  void build_method_table();
  auto build_method_fn_type(const FunctionDecl& method) -> const Type*;

  // --- TypeNode -> Type* bridge ---

  auto resolve_type_node(const TypeNode* node) -> const Type*;
  auto instantiate_generic(const Type* base_type, std::string_view name,
                           const std::vector<GenericParam>& type_params,
                           const std::vector<TypeNode*>& type_args, Span span) -> const Type*;

  // --- Symbol -> Type* bridge ---

  auto resolve_symbol_type(const Symbol* sym) -> const Type*;
  auto resolve_symbol_type_for_type_decl(const Symbol* sym) -> const Type*;

  // --- Declaration checking ---

  void register_declarations();
  void register_type_names();
  /// Register every alias not registered yet.  Returns how many this
  /// pass registered -- the progress the registration fixpoint
  /// converges on.  The final run reports the aliases that never
  /// resolved; a provisional run stays quiet, since a target it cannot
  /// see yet may still arrive.
  auto register_type_aliases(bool report_failures) -> size_t;
  /// Register one alias: the aliased type, or null when it does not
  /// resolve yet (or ever).  Also the on-demand path: a name that
  /// reaches an unregistered alias registers it first.
  auto register_type_alias(const Decl* decl, const Symbol* sym, bool report_failures)
      -> const Type*;
  /// Whether a type node is a path through an import binding, whose
  /// failures the resolver diagnoses (so the checker must not restate).
  [[nodiscard]] auto resolver_owns_path(const TypeNode* node) const -> bool;
  /// Whether a type node names a generic instantiation of a declaration
  /// that is not complete by value yet (an alias of it must wait).
  /// Pulls the declaration's slots first, so the answer is current.
  auto aliases_generic_shell(const TypeNode* node) -> bool;
  /// Each returns how many slots (fields / payloads) went from untyped
  /// to typed in this pass -- the progress the registration fixpoint
  /// converges on.
  auto register_struct_fields(bool report_failures) -> size_t;
  auto register_class_fields(PendingClass& pending, bool report_failures) -> size_t;
  auto register_enum_variants(bool report_failures) -> size_t;
  auto register_enum(const Decl* decl, const Symbol* sym, bool report_failures) -> size_t;
  /// Whether a type carries no untyped slot anywhere substitution would
  /// clone: an instantiation cloned from it would otherwise carry the
  /// hole.  Remembers a yes.
  [[nodiscard]] auto type_complete(const Type* type) -> bool;
  [[nodiscard]] auto complete_by_value(const Type* type,
                                       std::unordered_set<const Type*>& seen) const -> bool;
  /// Whether `type` is, or holds by value, a struct or enum of the
  /// declaration `target` -- the shape of a declaration with no finite
  /// size.
  [[nodiscard]] static auto contains_by_value(const Type* type,
                                              const Decl* target,
                                              std::unordered_set<const Type*>& seen) -> bool;
  /// Whether a path by value from `type` returns to a type on it.
  /// `acyclic` accumulates what has been cleared, so a graph is walked
  /// once however many paths share it.
  [[nodiscard]] static auto value_cycle(const Type* type,
                                        std::unordered_set<const Type*>& on_path,
                                        std::unordered_set<const Type*>& acyclic) -> bool;
  void register_signatures();
  void compute_derived_conformances();
  auto type_conforms_to(const Type* type, const Decl* concept_decl) -> bool;
  void check_declaration(const Decl* decl);
  void check_function(const Decl* decl);
  void check_class(const Decl* decl);

  // --- Statement checking ---

  void check_statement(const Stmt* stmt);
  void check_let(const Stmt* stmt);
  void check_assignment(const Stmt* stmt);
  void check_if(const Stmt* stmt);
  void check_while(const Stmt* stmt);
  void check_for(const Stmt* stmt);
  void check_yield(const Stmt* stmt);
  void check_match(const Stmt* stmt);
  void check_mode_block(const Stmt* stmt);
  void check_resource_block(const Stmt* stmt);
  void check_return(const Stmt* stmt);
  /// The binding a place is rooted at (`box.inner.text` roots at `box`),
  /// or null for a place rooted at no binding (a dereference).
  [[nodiscard]] auto place_root_symbol(const Expr* expr) const -> const Symbol*;
  /// Reject a store, inside a resource block, to a heap-owning binding
  /// declared outside it.
  void check_store_escapes_domain(const Expr* target);
  void check_expr_stmt(const Stmt* stmt);

  void check_body(const std::vector<Stmt*>& body);

  // --- Expression checking ---

  auto check_expr(const Expr* expr) -> const Type*;
  auto check_expr(const Expr* expr, const Type* expected) -> const Type*;

  auto check_identifier(const Expr* expr) -> const Type*;
  auto check_int_literal(const Expr* expr, const Type* expected) -> const Type*;
  auto check_float_literal(const Expr* expr, const Type* expected) -> const Type*;
  auto check_string_literal(const Expr* expr) -> const Type*;
  auto check_bool_literal(const Expr* expr) -> const Type*;
  auto check_binary(const Expr* expr) -> const Type*;
  auto check_unary(const Expr* expr) -> const Type*;
  auto check_call(const Expr* expr) -> const Type*;
  void infer_type_bindings(const Type* pattern,
                           const Type* concrete,
                           std::unordered_map<uint32_t, const Type*>& bindings,
                           Span error_span);
  auto substitute_generics(const Type* type,
                           const std::unordered_map<uint32_t, const Type*>& bindings)
      -> const Type*;
  void verify_concept_constraints(const Expr* callee_expr,
                                  Span error_span,
                                  const std::unordered_map<uint32_t, const Type*>& bindings);
  auto check_construct(const Expr* expr, const TypeStruct* struct_type) -> const Type*;
  auto check_pipe(const Expr* expr) -> const Type*;
  auto check_try(const Expr* expr) -> const Type*;
  auto check_field(const Expr* expr) -> const Type*;
  auto lookup_method(const Type* obj_type,
                     std::string_view name,
                     const Decl** resolved_decl = nullptr) -> const Type*;
  void validate_receiver(const Decl* method, Span context_span);
  auto check_index(const Expr* expr) -> const Type*;
  auto check_lambda(const Expr* expr, const Type* expected) -> const Type*;
  auto check_list_literal(const Expr* expr) -> const Type*;

  // --- Diagnostics ---

  void error(Span span, std::string message);
  void warning(Span span, std::string message);

  // --- Helpers ---

  auto is_lvalue(const Expr* expr) -> bool;
  auto find_generic_param_index(const Symbol* sym) -> uint32_t;
  auto resolve_builtin_function_type(std::string_view name) -> const Type*;
};

// ---------------------------------------------------------------------------
// Top-level entry point.
// ---------------------------------------------------------------------------

auto typecheck(const Program& program, const ResolveResult& resolve, TypeContext& types)
    -> TypeCheckResult;
auto typecheck(std::span<const FileNode* const> files, const ResolveResult& resolve,
               TypeContext& types) -> TypeCheckResult;

// Single-file convenience for tests.
auto typecheck(const FileNode& file, const ResolveResult& resolve, TypeContext& types)
    -> TypeCheckResult;

} // namespace dao

#endif // DAO_FRONTEND_TYPECHECK_TYPE_CHECKER_H
