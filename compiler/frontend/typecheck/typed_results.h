#ifndef DAO_FRONTEND_TYPECHECK_TYPED_RESULTS_H
#define DAO_FRONTEND_TYPECHECK_TYPED_RESULTS_H

#include "frontend/ast/ast.h"
#include "frontend/resolve/symbol.h"
#include "frontend/types/ptr_ops.h"
#include "frontend/types/type.h"

#include <algorithm>
#include <unordered_map>

namespace dao {

// ---------------------------------------------------------------------------
// TypedResults — side tables mapping AST nodes to semantic types.
//
// The type checker populates this structure. HIR lowering and tooling
// consume it without redoing type checking.
// ---------------------------------------------------------------------------

class TypedResults {
public:
  // --- Expression types ---

  void set_expr_type(const Expr* expr, const Type* type) {
    expr_types_[expr] = type;
  }

  [[nodiscard]] auto expr_type(const Expr* expr) const -> const Type* {
    auto it = expr_types_.find(expr);
    return it != expr_types_.end() ? it->second : nullptr;
  }

  [[nodiscard]] auto expr_types() const
      -> const std::unordered_map<const Expr*, const Type*>& {
    return expr_types_;
  }

  // --- Local variable types (let bindings, for variables) ---

  void set_local_type(const Stmt* stmt, const Type* type) {
    local_types_[stmt] = type;
  }

  [[nodiscard]] auto local_type(const Stmt* stmt) const -> const Type* {
    auto it = local_types_.find(stmt);
    return it != local_types_.end() ? it->second : nullptr;
  }

  // --- Function declaration types ---

  void set_decl_type(const Decl* decl, const Type* type) {
    decl_types_[decl] = type;
  }

  [[nodiscard]] auto decl_type(const Decl* decl) const -> const Type* {
    auto it = decl_types_.find(decl);
    return it != decl_types_.end() ? it->second : nullptr;
  }

  // --- Method resolution (field access → method function symbol) ---

  // --- Method resolution (field expr → resolved method FunctionDecl) ---

  void set_method_resolution(const Expr* field_expr, const Decl* method_decl) {
    method_resolutions_[field_expr] = method_decl;
  }

  [[nodiscard]] auto method_resolution(const Expr* field_expr) const
      -> const Decl* {
    auto it = method_resolutions_.find(field_expr);
    return it != method_resolutions_.end() ? it->second : nullptr;
  }

  // --- Resource blocks: the outer bindings a block stores to, which
  //     its exits copy into the enclosing domain ---
  void add_resource_escape(const Stmt* block, const Symbol* binding) {
    // A block stores to a handful of outer bindings; a list keeps their
    // order, which the MIR builder's flags and copies follow.
    auto& escapes = resource_escapes_[block];
    if (std::ranges::find(escapes, binding) == escapes.end()) {
      escapes.push_back(binding);
    }
  }
  [[nodiscard]] auto resource_escapes(const Stmt* block) const
      -> const std::vector<const Symbol*>* {
    auto it = resource_escapes_.find(block);
    return it != resource_escapes_.end() ? &it->second : nullptr;
  }

  // --- Operations of the compiler-standard Ptr<T> ---
  //     Recorded on the method's field expression and on its call, and on
  //     the `Ptr<T>::new()` call; HIR lowers each call to the operation.

  void set_ptr_op(const Expr* expr, PtrOp op) {
    ptr_ops_[expr] = op;
  }

  [[nodiscard]] auto ptr_op(const Expr* expr) const -> std::optional<PtrOp> {
    auto it = ptr_ops_.find(expr);
    if (it == ptr_ops_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  // --- What a call or construction decided about type parameters ---
  //
  //     The three answers belong to one expression and are asked of it
  //     together, so they are kept together:
  //
  //     - `written`: the type arguments written at the call.
  //     - `binder`: which declaration they bound.  `Box<i32>::make(1)`
  //       binds the class's parameters, `f<i32>(x)` and
  //       `b.mapped<string>(x)` the callee's own; a method's `U` and its
  //       class's `T` are both first, so the declaration is what says
  //       whose they are, all the way into specialization.
  //     - `open`: the parameters a construction left for its context to
  //       decide, by position.  The type alone cannot say which those
  //       are: a construction inside the class's own methods binds a
  //       parameter TO that parameter, which looks exactly like leaving
  //       it open.

  struct CallGenerics {
    std::vector<const Type*> written;
    std::vector<uint32_t> open;
    const Decl* binder = nullptr;
  };

  void set_open_type_params(const Expr* expr, std::vector<uint32_t> positions) {
    call_generics_[expr].open = std::move(positions);
  }

  [[nodiscard]] auto open_type_params(const Expr* expr) const -> const std::vector<uint32_t>* {
    const auto* generics = call_generics(expr);
    return generics != nullptr && !generics->open.empty() ? &generics->open : nullptr;
  }

  void set_type_args_binder(const Expr* call_expr, const Decl* binder) {
    call_generics_[call_expr].binder = binder;
  }

  [[nodiscard]] auto type_args_binder(const Expr* call_expr) const -> const Decl* {
    const auto* generics = call_generics(call_expr);
    return generics != nullptr ? generics->binder : nullptr;
  }

  void set_call_type_args(const Expr* call_expr,
                          std::vector<const Type*> type_args) {
    call_generics_[call_expr].written = std::move(type_args);
  }

  [[nodiscard]] auto call_type_args(const Expr* call_expr) const
      -> const std::vector<const Type*>* {
    const auto* generics = call_generics(call_expr);
    return generics != nullptr && !generics->written.empty() ? &generics->written : nullptr;
  }

private:
  [[nodiscard]] auto call_generics(const Expr* expr) const -> const CallGenerics* {
    auto it = call_generics_.find(expr);
    return it != call_generics_.end() ? &it->second : nullptr;
  }

  std::unordered_map<const Expr*, const Type*> expr_types_;
  std::unordered_map<const Stmt*, const Type*> local_types_;
  std::unordered_map<const Decl*, const Type*> decl_types_;
  std::unordered_map<const Expr*, const Decl*> method_resolutions_;
  std::unordered_map<const Expr*, CallGenerics> call_generics_;
  std::unordered_map<const Expr*, PtrOp> ptr_ops_;
  std::unordered_map<const Stmt*, std::vector<const Symbol*>> resource_escapes_;
};

} // namespace dao

#endif // DAO_FRONTEND_TYPECHECK_TYPED_RESULTS_H
