#ifndef DAO_IR_MIR_MIR_BUILDER_H
#define DAO_IR_MIR_MIR_BUILDER_H

#include "frontend/diagnostics/diagnostic.h"
#include "frontend/types/type_context.h"
#include "ir/hir/hir.h"
#include "ir/mir/mir.h"
#include "ir/mir/mir_context.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// MirBuildResult — output of MIR construction.
// ---------------------------------------------------------------------------

struct MirBuildResult {
  MirModule* module = nullptr;
  std::vector<Diagnostic> diagnostics;

  /// Generic function templates: bodies lowered to MIR but excluded from
  /// module->functions.  The monomorphizer clones from these to produce
  /// concrete instantiations.  Keyed by declaration symbol.
  std::unordered_map<const Symbol*, MirFunction*> generic_templates;
};

// ---------------------------------------------------------------------------
// MirBuilder — lowers HIR into MIR.
// ---------------------------------------------------------------------------

class MirBuilder {
public:
  MirBuilder(MirContext& ctx, TypeContext& types);

  auto build(const HirProgram& program) -> MirBuildResult;

private:
  MirContext& ctx_;
  TypeContext& types_;
  std::vector<Diagnostic> diagnostics_;

  // --- Module-level state ---
  MirModule* current_module_ = nullptr;

  // --- Per-function state (reset for each function) ---
  MirFunction* current_fn_ = nullptr;
  MirBlock* current_block_ = nullptr;
  uint32_t next_value_id_ = 0;
  uint32_t next_block_id_ = 0;
  std::unordered_map<const Symbol*, LocalId> symbol_to_local_;

  // True while lowering a generic function body into a template.
  // Gates tolerance for unresolved field accesses on non-struct types
  // (concept method calls on generic type parameters).
  bool lowering_generic_template_ = false;

  // Generic templates accumulated during build().
  std::unordered_map<const Symbol*, MirFunction*> generic_templates_;

  // An outer binding a resource block stores to: copied into the
  // enclosing domain at the block's exits, when its flag says the block
  // stored to it on the path taken.
  struct EscapeFlag {
    LocalId binding;
    const Type* type;
    LocalId flag;
  };

  // Active mode/resource region stack for exit-on-return.
  struct ActiveRegion {
    MirPayload exit_payload;
    Span span;
    std::vector<EscapeFlag> escapes; // only a resource region has any
  };
  std::vector<ActiveRegion> active_regions_;

  // What a `return` carries out of the regions it leaves: the returned
  // value (none for a bare return) and its type.
  struct Carried {
    MirValueId* value = nullptr;
    const Type* type = nullptr;
  };

  // The prelude's `copy_out<T>` (types/type_ownership.h explains what
  // is copied): the call every escaping value goes through, expanded
  // per type by the monomorphizer.  Null when no prelude is loaded, in
  // which case nothing is copied (single-file tests without a prelude).
  const Symbol* copy_out_symbol_ = nullptr;
  const Type* copy_out_type_ = nullptr;

  // The loops being lowered, innermost last: where `break` branches to,
  // and how many regions were active when the loop was entered.  A
  // `break` exits only the regions entered inside the loop; the ones
  // enclosing it stay open, and are exited where their own blocks end.
  struct LoopExit {
    BlockId exit_block;
    size_t region_depth;
  };
  std::vector<LoopExit> loop_exit_stack_;

  // --- Function lowering ---
  auto lower_function(const HirFunction& fn, Span span) -> MirFunction*;

  // --- Statement lowering ---
  void lower_stmt(const HirStmt& stmt);

  // --- Expression lowering ---
  auto lower_expr_value(const HirExpr& expr) -> MirValueId;
  auto lower_expr_place(const HirExpr& expr) -> MirPlace;

  // --- Emit helpers ---
  // Value-producing: sets result, type (from expr), span, emits, returns id.
  auto emit_value(const HirExpr& expr, MirPayload payload) -> MirValueId;
  // Value-producing with explicit type and span.
  auto emit_value(const Type* type, Span span, MirPayload payload) -> MirValueId;
  // Side-effect only: no result, no type.
  void emit_effect(Span span, MirPayload payload);
  // Terminator: no result, no type.
  void emit_terminator(Span span, MirPayload payload);

  // --- Helpers ---
  auto fresh_value() -> MirValueId;
  auto fresh_block() -> MirBlock*;
  auto declare_local(const Symbol* sym, const Type* type, Span span,
                     bool is_param = false) -> LocalId;
  void emit(MirInst* inst);
  void switch_to_block(MirBlock* block);
  [[nodiscard]] auto block_terminated() const -> bool;

  /// Exit every active region, innermost first: a `return` (or `?`)
  /// leaves them all, and what it carries is copied out of each resource
  /// domain crossed.
  void emit_region_exits(Span span, Carried carried);
  /// Exit the active regions above `depth`, innermost first (a `break`
  /// leaves the ones its loop entered).
  void emit_region_exits_from(size_t depth, Span span);
  /// Leave one region: copy out what escapes it -- the bindings the block
  /// stored to, or, when a function return is what leaves (`carried` is
  /// given), the returned value -- then exit it.
  void emit_region_exit(const ActiveRegion& region, Span span, const Carried* carried);
  /// `copy_out<T>(value)`: a copy in the enclosing domain, or the value
  /// itself when its type owns nothing.
  auto emit_copy_out(MirValueId value, const Type* type, Span span) -> MirValueId;

  auto resolve_field_index(const Type* obj_type, std::string_view field_name)
      -> std::optional<uint32_t>;

  void error(Span span, std::string message);
};

// ---------------------------------------------------------------------------
// Top-level entry point.
// ---------------------------------------------------------------------------

auto build_mir(const HirProgram& program, MirContext& ctx, TypeContext& types) -> MirBuildResult;

} // namespace dao

#endif // DAO_IR_MIR_MIR_BUILDER_H
