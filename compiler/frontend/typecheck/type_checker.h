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
  const Type* method_type; // function type (self removed)
  // The module whose `extend` introduced it; null when it is visible
  // everywhere (a class's own method, or a prelude module's extend).
  const ModuleInfo* owner = nullptr;
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

struct CheckContext {
  const Type* return_type = nullptr; // enclosing function return type
  const Type* self_type = nullptr;   // type of `self` in current scope (class/extend)
  std::unordered_set<std::string_view> active_modes; // e.g. "unsafe"
  uint32_t loop_depth = 0;                           // nesting depth for break validation
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
  // declarations first, then check all bodies.  Per-module checking is
  // not implemented yet.
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
  std::unordered_map<std::string_view, const Type*> concept_self_map_;

  // RAII guard that saves and restores a single key in concept_self_map_.
  // Each guard scope inserts exactly one concept→type binding; on
  // destruction the prior state of that key is restored. O(1) instead
  // of copying the entire map.
  struct ConceptSelfMapGuard {
    using Map = std::unordered_map<std::string_view, const Type*>;
    Map& map;
    std::string_view key;
    const Type* old_value = nullptr;
    bool had_key = false;

    ConceptSelfMapGuard(Map& m, std::string_view k) : map(m), key(k) { // NOLINT(readability-identifier-length)
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
    // Declared by the type itself (a class method or conformance-block
    // method), as opposed to introduced by an `extend`.  An inherent
    // method is innermost: no extension shadows it.
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

  std::unordered_map<MethodKey, MethodEntry, MethodKeyHash> method_table_;

  // The module whose declaration is being registered or checked, so that
  // method-set lookup can tell a module's own `extend` methods from a
  // sibling module's.  Null outside a program.
  const ModuleInfo* current_module_ = nullptr;

  /// RAII: sets current_module_ for a declaration and restores it after.
  struct CurrentModuleGuard {
    const ModuleInfo*& slot;
    const ModuleInfo* saved;
    CurrentModuleGuard(const ModuleInfo*& target, const ModuleInfo* module)
        : slot(target), saved(target) {
      slot = module;
    }
    ~CurrentModuleGuard() {
      slot = saved;
    }
    CurrentModuleGuard(const CurrentModuleGuard&) = delete;
    auto operator=(const CurrentModuleGuard&) -> CurrentModuleGuard& = delete;
  };

  /// The module a top-level declaration belongs to, or null outside a
  /// program.  The resolver stamps every symbol with its owning module;
  /// a named declaration is found by the symbol at its name span, and an
  /// `extend` block — which declares no name of its own — by the symbol
  /// of the first method it introduces.
  [[nodiscard]] auto declaring_module(const Decl* decl) const -> const ModuleInfo*;

  /// The owner to register an `extend` block's methods under: the
  /// declaring module, or null for a prelude module, whose `extend`
  /// methods are visible everywhere (CONTRACT_MODULE_SYSTEM.md §7.2).
  [[nodiscard]] auto extend_owner(const Decl* extend_decl) const -> const ModuleInfo*;

  /// Whether a method registered under `owner` participates in
  /// method-set lookup from the module being checked.
  [[nodiscard]] auto owner_is_visible(const ModuleInfo* owner) const -> bool {
    return owner == nullptr || owner == current_module_;
  }

  // Pending class shells awaiting field resolution (populated by
  // register_type_names, consumed by register_struct_fields).
  struct PendingClass {
    const ClassDecl* class_decl;
    const Decl* decl;
    TypeStruct* shell;
  };
  std::vector<PendingClass> pending_classes_;

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
  void register_struct_fields();
  void register_enum_variants();
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
