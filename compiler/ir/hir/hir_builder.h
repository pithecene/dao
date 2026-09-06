#ifndef DAO_IR_HIR_HIR_BUILDER_H
#define DAO_IR_HIR_HIR_BUILDER_H

#include "frontend/ast/ast.h"
#include "frontend/diagnostics/diagnostic.h"
#include "frontend/module/program.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"
#include "ir/hir/hir.h"
#include "ir/hir/hir_context.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// HirBuildResult — output of HIR construction.
// ---------------------------------------------------------------------------

struct HirBuildResult {
  HirModule* module = nullptr;
  std::vector<Diagnostic> diagnostics;
};

// ---------------------------------------------------------------------------
// HirBuilder — lowers checked AST into HIR.
//
// Consumes AST + resolve + typed results.
// Does not redo parsing, resolution, or type checking.
// ---------------------------------------------------------------------------

class HirBuilder {
public:
  HirBuilder(HirContext& ctx, const ResolveResult& resolve,
             const TypeCheckResult& typed);

  // Lower every file's declarations into one HirModule, in load order.
  // Per-module HIR is not implemented yet.
  auto build(std::span<const FileNode* const> files) -> HirBuildResult;

private:
  HirContext& ctx_;
  const ResolveResult& resolve_;
  const TypeCheckResult& typed_;
  std::vector<Diagnostic> diagnostics_;
  std::vector<HirDecl*> extend_decls_; // extend methods lowered as functions

  // decl_span.offset -> Symbol* for declaration-site lookups.
  std::unordered_map<uint32_t, const Symbol*> decl_symbols_;

  // --- Declaration lowering ---

  auto lower_decl(const Decl* decl) -> HirDecl*;
  auto lower_function(const Decl* decl) -> HirDecl*;
  auto lower_class(const Decl* decl) -> HirDecl*;

  // --- Statement lowering ---

  auto lower_stmt(const Stmt* stmt) -> HirStmt*;
  auto lower_body(const std::vector<Stmt*>& body) -> std::vector<HirStmt*>;
  void lower_match_into(const Stmt* stmt, std::vector<HirStmt*>& out);

  // --- Expression lowering ---

  auto lower_expr(const Expr* expr) -> HirExpr*;
  auto variant_value(const TypeEnum* enum_type, std::string_view variant_name, Span span)
      -> HirExpr*;

  // --- Helpers ---

  auto find_symbol_at_decl(uint32_t offset) -> const Symbol*;
  auto find_symbol_at_use(uint32_t offset) -> const Symbol*;
  auto expr_type(const Expr* expr) -> const Type*;

  void error(Span span, std::string message);
};

// ---------------------------------------------------------------------------
// Top-level entry point.
// ---------------------------------------------------------------------------

auto build_hir(const Program& program, const ResolveResult& resolve,
               const TypeCheckResult& typed, HirContext& ctx)
    -> HirBuildResult;
auto build_hir(std::span<const FileNode* const> files, const ResolveResult& resolve,
               const TypeCheckResult& typed, HirContext& ctx)
    -> HirBuildResult;

// Single-file convenience for tests.
auto build_hir(const FileNode& file, const ResolveResult& resolve,
               const TypeCheckResult& typed, HirContext& ctx)
    -> HirBuildResult;

} // namespace dao

#endif // DAO_IR_HIR_HIR_BUILDER_H
