#include "ir/mir/mir_builder.h"

#include "frontend/module/program.h"
#include "frontend/types/type.h"
#include "frontend/types/type_ownership.h"
#include "frontend/types/type_printer.h"
#include "support/variant.h"

#include <string>

namespace dao {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MirBuilder::MirBuilder(MirContext& ctx, TypeContext& types)
    : ctx_(ctx), types_(types) {}

// ---------------------------------------------------------------------------
// Field index resolution
// ---------------------------------------------------------------------------

auto MirBuilder::resolve_field_index(const Type* obj_type,
                                     std::string_view field_name)
    -> std::optional<uint32_t> {
  if (obj_type != nullptr && obj_type->kind() == TypeKind::Struct) {
    const auto* st = static_cast<const TypeStruct*>(obj_type);
    for (uint32_t idx = 0; idx < st->fields().size(); ++idx) {
      if (st->fields()[idx].name == field_name) {
        return idx;
      }
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Emit helpers
// ---------------------------------------------------------------------------

auto MirBuilder::emit_value(const HirExpr& expr, MirPayload payload)
    -> MirValueId {
  return emit_value(expr.type, expr.span, payload);
}

auto MirBuilder::emit_value(const Type* type, Span span, MirPayload payload)
    -> MirValueId {
  auto* inst = ctx_.alloc<MirInst>();
  inst->result = fresh_value();
  inst->type = type;
  inst->span = span;
  inst->payload = payload;
  emit(inst);
  return inst->result;
}

void MirBuilder::emit_effect(Span span, MirPayload payload) {
  auto* inst = ctx_.alloc<MirInst>();
  inst->span = span;
  inst->payload = payload;
  emit(inst);
}

void MirBuilder::emit_terminator(Span span, MirPayload payload) {
  auto* inst = ctx_.alloc<MirInst>();
  inst->span = span;
  inst->payload = payload;
  emit(inst);
}

// ---------------------------------------------------------------------------
// Generic detection
// ---------------------------------------------------------------------------

namespace {

/// Check if a Dao type (or any nested type) contains TypeGenericParam.
/// Used to detect functions on generic classes (e.g. HashMap<V>.length)
/// whose own declaration has no type params but whose signature references
/// the enclosing class's generic parameters.
auto type_has_generic_param(const Type* type) -> bool {
  if (type == nullptr) {
    return false;
  }
  switch (type->kind()) {
  case TypeKind::GenericParam:
    return true;
  case TypeKind::Function: {
    const auto* fn_type = static_cast<const TypeFunction*>(type);
    for (const auto* param : fn_type->param_types()) {
      if (type_has_generic_param(param)) {
        return true;
      }
    }
    return type_has_generic_param(fn_type->return_type());
  }
  case TypeKind::Pointer:
    return type_has_generic_param(
        static_cast<const TypePointer*>(type)->pointee());
  case TypeKind::Struct: {
    const auto* str = static_cast<const TypeStruct*>(type);
    for (const auto& field : str->fields()) {
      if (type_has_generic_param(field.type)) {
        return true;
      }
    }
    return false;
  }
  case TypeKind::Enum: {
    const auto* enm = static_cast<const TypeEnum*>(type);
    for (const auto& variant : enm->variants()) {
      for (const auto* payload : variant.payload_types) {
        if (type_has_generic_param(payload)) {
          return true;
        }
      }
    }
    return false;
  }
  case TypeKind::Generator:
    return type_has_generic_param(
        static_cast<const TypeGenerator*>(type)->yield_type());
  default:
    return false;
  }
}

/// A function is generic if either:
///  (a) its AST declaration has own type parameters (e.g. fn f<T>), or
///  (b) its lowered signature contains TypeGenericParam (e.g. methods
///      on generic classes where self: HashMap<V>).
auto hir_function_is_generic(const HirFunction& fn) -> bool {
  if (fn.has_type_params) {
    return true;
  }
  if (type_has_generic_param(fn.return_type)) {
    return true;
  }
  for (const auto& param : fn.params) {
    if (type_has_generic_param(param.type)) {
      return true;
    }
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Top-level
// ---------------------------------------------------------------------------

namespace {

/// Smallest span covering both (disjoint ranges of one offset space).
auto covering(Span lhs, Span rhs) -> Span {
  auto begin = std::min(lhs.offset, rhs.offset);
  auto end = std::max(lhs.offset + lhs.length, rhs.offset + rhs.length);
  return Span{.offset = begin, .length = end - begin};
}

} // namespace

auto MirBuilder::build(const HirProgram& program) -> MirBuildResult {
  // The prelude's copy_out, for the exits of resource blocks.
  copy_out_symbol_ = nullptr;
  copy_out_type_ = nullptr;
  for (const auto* module : program.modules) {
    if (module->module == nullptr || !module->module->is_prelude) {
      continue;
    }
    for (const auto* decl : module->declarations) {
      const auto* fn = std::get_if<HirFunction>(&decl->payload);
      if (fn == nullptr || fn->symbol == nullptr || fn->symbol->name != "copy_out" ||
          fn->params.size() != 1) {
        continue;
      }
      copy_out_symbol_ = fn->symbol;
      copy_out_type_ = types_.function_type({fn->params[0].type}, fn->return_type);
    }
  }
  auto* mir_mod = ctx_.alloc<MirModule>();
  current_module_ = mir_mod;
  generic_templates_.clear();

  // Every module's functions, in program order, into one MirModule.
  // Generic templates are keyed by symbol and so are program-wide.
  for (const auto* hir_module : program.modules) {
    mir_mod->span =
        mir_mod->span.length == 0 ? hir_module->span : covering(mir_mod->span, hir_module->span);
    for (const auto* decl : hir_module->declarations) {
      if (decl->is<HirFunction>()) {
        const auto& hir_fn = decl->as<HirFunction>();

        if (hir_function_is_generic(hir_fn)) {
          // Generic function: lower body into a template for the
          // monomorphizer to clone from, but do NOT add to the module's
          // function list.  The lowering_generic_template_ flag gates
          // tolerance for unresolved field accesses on generic type
          // parameters (concept method calls like x.to_string() where
          // x: T: Printable).
          lowering_generic_template_ = true;
          auto* mir_fn = lower_function(hir_fn, decl->span);
          lowering_generic_template_ = false;
          if (mir_fn != nullptr && mir_fn->symbol != nullptr) {
            generic_templates_[mir_fn->symbol] = mir_fn;
          }
        } else {
          // Monomorphic function: lower normally into the module.
          auto* mir_fn = lower_function(hir_fn, decl->span);
          if (mir_fn != nullptr) {
            mir_mod->functions.push_back(mir_fn);
          }
        }
      }
      // ClassDecl: no MIR function to produce; skip.
    }
  }

  return {.module = mir_mod,
          .diagnostics = std::move(diagnostics_),
          .generic_templates = std::move(generic_templates_)};
}

// ---------------------------------------------------------------------------
// Function lowering
// ---------------------------------------------------------------------------

auto MirBuilder::lower_function(const HirFunction& fn, Span span)
    -> MirFunction* {
  auto* mir_fn = ctx_.alloc<MirFunction>();
  mir_fn->symbol = fn.symbol;
  mir_fn->return_type = fn.return_type;
  mir_fn->span = span;
  mir_fn->is_extern = fn.is_extern;

  // Reset per-function state.
  current_fn_ = mir_fn;
  current_block_ = nullptr;
  next_value_id_ = 0;
  next_block_id_ = 0;
  symbol_to_local_.clear();
  active_regions_.clear();

  // Create parameter locals.
  for (const auto& param : fn.params) {
    declare_local(param.symbol, param.type, param.span, /*is_param=*/true);
  }

  // Extern declaration — no blocks to emit.
  if (fn.is_extern) {
    return mir_fn;
  }

  // Create entry block.
  auto* entry = fresh_block();
  switch_to_block(entry);

  // Lower body statements.
  for (const auto* stmt : fn.body) {
    lower_stmt(*stmt);
  }

  // Ensure the last block has a terminator.
  if (!block_terminated()) {
    emit_terminator(span, MirReturn{{}, false});
  }

  return mir_fn;
}

// ---------------------------------------------------------------------------
// Statement lowering
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void MirBuilder::lower_stmt(const HirStmt& stmt) {
  if (block_terminated()) {
    return; // Dead code after terminator.
  }

  std::visit(overloaded{
      [&](const HirLet& let_stmt) {
        auto local_id =
            declare_local(let_stmt.symbol, let_stmt.type, stmt.span);

        if (let_stmt.initializer != nullptr) {
          auto val = lower_expr_value(*let_stmt.initializer);
          auto* place = ctx_.alloc<MirPlace>();
          place->local = local_id;
          emit_effect(stmt.span, MirStore{place, val});
        }
      },
      [&](const HirAssign& assign) {
        auto place = lower_expr_place(*assign.target);
        auto val = lower_expr_value(*assign.value);
        emit_effect(stmt.span,
                    MirStore{ctx_.alloc<MirPlace>(place), val});
        // A store rooted at a binding a resource block copies out marks
        // it dirty for that block: its exit copies only what was stored.
        for (const auto& region : active_regions_) {
          for (const auto& escape : region.escapes) {
            if (escape.binding.id == place.local.id) {
              auto set = emit_value(types_.bool_type(), stmt.span, MirConstBool{true});
              auto* flag_place = ctx_.alloc<MirPlace>();
              flag_place->local = escape.flag;
              emit_effect(stmt.span, MirStore{flag_place, set});
            }
          }
        }
      },
      [&](const HirIf& hir_if) {
        auto cond_val = lower_expr_value(*hir_if.condition);

        auto* then_bb = fresh_block();
        bool has_else = !hir_if.else_body.empty();
        auto* else_bb = has_else ? fresh_block() : nullptr;

        MirBlock* merge_bb = nullptr;
        if (!has_else) {
          merge_bb = fresh_block();
          else_bb = merge_bb;
        }

        emit_terminator(stmt.span,
                        MirCondBr{cond_val, then_bb->id, else_bb->id});

        // Then block.
        switch_to_block(then_bb);
        for (const auto* s : hir_if.then_body) {
          lower_stmt(*s);
        }
        bool then_terminated = block_terminated();
        if (!then_terminated) {
          if (merge_bb == nullptr) {
            merge_bb = fresh_block();
          }
          emit_terminator(stmt.span, MirBr{merge_bb->id});
        }

        // Else block.
        if (has_else) {
          switch_to_block(else_bb);
          for (const auto* s : hir_if.else_body) {
            lower_stmt(*s);
          }
          if (!block_terminated()) {
            if (merge_bb == nullptr) {
              merge_bb = fresh_block();
            }
            emit_terminator(stmt.span, MirBr{merge_bb->id});
          }
        }

        if (merge_bb != nullptr) {
          switch_to_block(merge_bb);
        } else {
          current_block_ = nullptr;
        }
      },
      [&](const HirWhile& hir_while) {
        auto* cond_bb = fresh_block();
        auto* body_bb = fresh_block();
        auto* exit_bb = fresh_block();

        emit_terminator(stmt.span, MirBr{cond_bb->id});

        switch_to_block(cond_bb);
        auto cond_val = lower_expr_value(*hir_while.condition);
        emit_terminator(stmt.span,
                        MirCondBr{cond_val, body_bb->id, exit_bb->id});

        switch_to_block(body_bb);
        loop_exit_stack_.push_back({exit_bb->id, active_regions_.size()});
        for (const auto* s : hir_while.body) {
          lower_stmt(*s);
        }
        loop_exit_stack_.pop_back();
        if (!block_terminated()) {
          emit_terminator(stmt.span, MirBr{cond_bb->id});
        }

        switch_to_block(exit_bb);
      },
      [&](const HirFor& hir_for) {
        auto iter_src = lower_expr_value(*hir_for.iterable);
        auto iter_val = emit_value(
            hir_for.iterable->type, stmt.span, MirIterInit{iter_src});

        // Extract element type T from Generator<T>.
        const Type* var_type = hir_for.iterable->type;
        if (var_type != nullptr && var_type->kind() == TypeKind::Generator) {
          var_type =
              static_cast<const TypeGenerator*>(var_type)->yield_type();
        }
        auto var_local =
            declare_local(hir_for.var_symbol, var_type, stmt.span);

        auto* cond_bb = fresh_block();
        auto* body_bb = fresh_block();
        auto* exit_bb = fresh_block();

        emit_terminator(stmt.span, MirBr{cond_bb->id});

        switch_to_block(cond_bb);
        auto has_next_val = emit_value(
            types_.bool_type(), stmt.span, MirIterHasNext{iter_val});
        emit_terminator(stmt.span,
                        MirCondBr{has_next_val, body_bb->id, exit_bb->id});

        switch_to_block(body_bb);
        auto next_val = emit_value(
            var_type, stmt.span, MirIterNext{iter_val});

        auto* place = ctx_.alloc<MirPlace>();
        place->local = var_local;
        emit_effect(stmt.span, MirStore{place, next_val});

        // Push iterator cleanup so early returns inside the body
        // emit MirIterDestroy before the return terminator.  A `break`
        // does not: it branches to the exit block, which destroys the
        // iterator itself, so the loop's region depth is taken after
        // this push.
        active_regions_.push_back(
            ActiveRegion{MirIterDestroy{iter_val}, stmt.span});

        loop_exit_stack_.push_back({exit_bb->id, active_regions_.size()});
        for (const auto* s : hir_for.body) {
          lower_stmt(*s);
        }
        loop_exit_stack_.pop_back();
        if (!block_terminated()) {
          emit_terminator(stmt.span, MirBr{cond_bb->id});
        }

        active_regions_.pop_back();

        switch_to_block(exit_bb);
        // Free the generator frame on the normal loop-exit path.
        emit_effect(stmt.span, MirIterDestroy{iter_val});
      },
      [&](const HirBreak&) {
        if (!loop_exit_stack_.empty()) {
          // Leave the regions entered inside the loop (mode/resource
          // cleanup) before branching out of it.  A region enclosing
          // the loop is not left: its block continues after the loop.
          const auto& loop = loop_exit_stack_.back();
          emit_region_exits_from(loop.region_depth, stmt.span);
          emit_terminator(stmt.span, MirBr{loop.exit_block});
        }
      },
      [&](const HirYield& yield) {
        auto val = lower_expr_value(*yield.value);
        emit_effect(stmt.span, MirYieldInst{val});
      },
      [&](const HirReturn& ret) {
        MirValueId ret_val;
        bool has_val = ret.value != nullptr;
        if (has_val) {
          ret_val = lower_expr_value(*ret.value);
        }
        emit_region_exits(stmt.span,
                          {.value = has_val ? &ret_val : nullptr,
                           .type = has_val ? current_fn_->return_type : nullptr});
        emit_terminator(stmt.span, MirReturn{ret_val, has_val});
      },
      [&](const HirExprStmt& expr_stmt) {
        lower_expr_value(*expr_stmt.expr);
      },
      [&](const HirMode& mode) {
        emit_effect(stmt.span,
                    MirModeEnter{mode.mode, mode.mode_name});

        active_regions_.push_back(
            {.exit_payload = MirModeExit{mode.mode},
             .span = stmt.span});

        for (const auto* s : mode.body) {
          lower_stmt(*s);
        }

        active_regions_.pop_back();

        if (!block_terminated()) {
          emit_effect(stmt.span, MirModeExit{mode.mode});
        }
      },
      [&](const HirResource& res) {
        auto domain_handle = emit_value(
            types_.pointer_to(types_.void_type()), stmt.span,
            MirResourceEnter{res.resource_kind, res.resource_name});

        ActiveRegion region{
            .exit_payload = MirResourceExit{res.resource_kind, res.resource_name, domain_handle},
            .span = stmt.span,
            .escapes = {}};
        // One dirty flag per escaping binding, clear at entry; every
        // store rooted at the binding inside the block sets it.
        for (const auto* symbol : res.escapes) {
          auto local = symbol_to_local_.find(symbol);
          if (local == symbol_to_local_.end()) {
            continue;
          }
          const Type* type = current_fn_->locals[local->second.id].type;
          auto flag = declare_local(nullptr, types_.bool_type(), stmt.span);
          auto clear = emit_value(types_.bool_type(), stmt.span, MirConstBool{false});
          auto* flag_place = ctx_.alloc<MirPlace>();
          flag_place->local = flag;
          emit_effect(stmt.span, MirStore{flag_place, clear});
          region.escapes.push_back({.binding = local->second, .type = type, .flag = flag});
        }
        active_regions_.push_back(region);

        for (const auto* s : res.body) {
          lower_stmt(*s);
        }

        active_regions_.pop_back();

        if (!block_terminated()) {
          emit_region_exit(region, stmt.span, nullptr);
        }
      },
  }, stmt.payload);
}

// ---------------------------------------------------------------------------
// Expression lowering — value
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto MirBuilder::lower_expr_value(const HirExpr& expr) -> MirValueId {
  return std::visit(overloaded{
      [&](const HirIntLiteral& lit) -> MirValueId {
        return emit_value(expr, MirConstInt{lit.value});
      },
      [&](const HirFloatLiteral& lit) -> MirValueId {
        return emit_value(expr, MirConstFloat{lit.value});
      },
      [&](const HirBoolLiteral& lit) -> MirValueId {
        return emit_value(expr, MirConstBool{lit.value});
      },
      [&](const HirStringLiteral& lit) -> MirValueId {
        return emit_value(expr, MirConstString{lit.value});
      },
      [&](const HirSymbolRef& ref) -> MirValueId {
        if (ref.symbol != nullptr) {
          auto it = symbol_to_local_.find(ref.symbol);
          if (it != symbol_to_local_.end()) {
            auto* place = ctx_.alloc<MirPlace>();
            place->local = it->second;
            return emit_value(expr, MirLoad{place});
          }
        }
        return emit_value(expr, MirFnRef{ref.symbol});
      },
      [&](const HirUnary& un) -> MirValueId {
        if (un.op == UnaryOp::AddrOf) {
          auto place = lower_expr_place(*un.operand);
          return emit_value(expr, MirAddrOf{ctx_.alloc<MirPlace>(place)});
        }
        if (un.op == UnaryOp::Deref) {
          auto place = lower_expr_place(expr);
          return emit_value(expr, MirLoad{ctx_.alloc<MirPlace>(place)});
        }
        auto val = lower_expr_value(*un.operand);
        return emit_value(expr, MirUnary{un.op, val});
      },
      [&](const HirBinary& bin) -> MirValueId {
        auto left = lower_expr_value(*bin.left);
        auto right = lower_expr_value(*bin.right);
        return emit_value(expr, MirBinary{bin.op, left, right});
      },
      [&](const HirCall& call) -> MirValueId {
        auto callee_val = lower_expr_value(*call.callee);
        auto* args = ctx_.alloc<std::vector<MirValueId>>();
        for (const auto* arg : call.args) {
          args->push_back(lower_expr_value(*arg));
        }
        std::vector<const Type*>* type_args = nullptr;
        if (!call.explicit_type_args.empty()) {
          type_args = ctx_.alloc<std::vector<const Type*>>(
              call.explicit_type_args);
        }
        return emit_value(expr, MirCall{callee_val, args, type_args});
      },
      [&](const HirConstruct& ctor) -> MirValueId {
        auto* field_vals = ctx_.alloc<std::vector<MirValueId>>();
        for (const auto* arg : ctor.args) {
          field_vals->push_back(lower_expr_value(*arg));
        }
        return emit_value(expr, MirConstruct{ctor.struct_type, field_vals});
      },
      [&](const HirEnumConstruct& ctor) -> MirValueId {
        auto* payload_vals = ctx_.alloc<std::vector<MirValueId>>();
        for (const auto* arg : ctor.payload_args) {
          payload_vals->push_back(lower_expr_value(*arg));
        }
        return emit_value(expr, MirEnumConstruct{
            ctor.enum_type, ctor.variant_index, payload_vals});
      },
      [&](const HirEnumDiscriminant& disc) -> MirValueId {
        auto val = lower_expr_value(*disc.enum_value);
        return emit_value(expr, MirEnumDiscriminant{val});
      },
      [&](const HirEnumPayload& pay) -> MirValueId {
        auto val = lower_expr_value(*pay.enum_value);
        return emit_value(expr, MirEnumPayload{
            val, pay.variant_index, pay.field_index});
      },
      [&](const HirTry& try_node) -> MirValueId {
        // Lower operand (the Result/Option value).
        auto operand_val = lower_expr_value(*try_node.operand);
        const auto* enum_type = try_node.enum_type;
        bool is_result = enum_type != nullptr && enum_type->name() == "Result";

        // Extract discriminant (i32 — matches LLVM enum layout).
        auto discr = emit_value(types_.builtin(BuiltinKind::I32), expr.span,
                                MirEnumDiscriminant{operand_val});

        // Compare discriminant to 0 (the success variant: Ok/Some).
        auto zero = emit_value(types_.builtin(BuiltinKind::I32), expr.span,
                               MirConstInt{0});
        auto cmp = emit_value(types_.bool_type(), expr.span,
                              MirBinary{BinaryOp::EqEq, discr, zero});

        // Create basic blocks.
        auto* ok_bb = fresh_block();
        auto* err_bb = fresh_block();
        auto* merge_bb = fresh_block();

        emit_terminator(expr.span, MirCondBr{cmp, ok_bb->id, err_bb->id});

        // Error/None block: propagate the error or None.
        switch_to_block(err_bb);
        if (is_result) {
          // Extract Err payload from variant 1, field 0.
          auto err_payload = emit_value(
              enum_type->variants()[1].payload_types[0], expr.span,
              MirEnumPayload{operand_val, 1, 0});
          // Construct Result.Err(err_payload) with the function's return type.
          const auto* ret_enum_type = static_cast<const TypeEnum*>(
              current_fn_->return_type);
          auto* err_args = ctx_.alloc<std::vector<MirValueId>>();
          err_args->push_back(err_payload);
          auto err_result = emit_value(
              current_fn_->return_type, expr.span,
              MirEnumConstruct{ret_enum_type, 1, err_args});
          emit_region_exits(expr.span, {.value = &err_result, .type = current_fn_->return_type});
          emit_terminator(expr.span, MirReturn{err_result, true});
        } else {
          // Option: construct Option.None (variant 1, no payload).
          const auto* ret_enum_type = static_cast<const TypeEnum*>(
              current_fn_->return_type);
          auto* empty_args = ctx_.alloc<std::vector<MirValueId>>();
          auto none_result = emit_value(
              current_fn_->return_type, expr.span,
              MirEnumConstruct{ret_enum_type, 1, empty_args});
          emit_region_exits(expr.span, {.value = &none_result, .type = current_fn_->return_type});
          emit_terminator(expr.span, MirReturn{none_result, true});
        }

        // Ok/Some block: extract the success payload.
        switch_to_block(ok_bb);
        auto ok_payload = emit_value(
            expr.type, expr.span,
            MirEnumPayload{operand_val, 0, 0});
        emit_terminator(expr.span, MirBr{merge_bb->id});

        // Merge block: the value produced by ? is the ok payload.
        switch_to_block(merge_bb);
        return ok_payload;
      },
      [&](const HirPipe& pipe) -> MirValueId {
        auto left_val = lower_expr_value(*pipe.left);
        auto callee_val = lower_expr_value(*pipe.right);
        auto* args = ctx_.alloc<std::vector<MirValueId>>();
        args->push_back(left_val);
        return emit_value(expr, MirCall{callee_val, args});
      },
      [&](const HirField& field) -> MirValueId {
        auto obj = lower_expr_value(*field.object);
        auto field_idx = resolve_field_index(field.object->type, field.field_name);
        if (!field_idx) {
          if (lowering_generic_template_) {
            // Template body: tolerate unresolved field accesses on
            // generic type parameters (e.g., x.to_string() where
            // x: T: Printable).  The monomorphizer will clone this
            // template and substitute concrete types, at which point
            // the field access resolves normally.
          } else if (field.object->type != nullptr &&
                     field.object->type->kind() == TypeKind::Struct) {
            error(expr.span, "unresolved field '" + std::string(field.field_name) + "'");
          } else {
            error(expr.span, "unresolved field '" + std::string(field.field_name) + "'");
          }
        }
        return emit_value(expr, MirFieldAccess{
            obj, field.field_name, field_idx.value_or(0)});
      },
      [&](const HirIndex& idx) -> MirValueId {
        auto obj = lower_expr_value(*idx.object);
        MirValueId index_val;
        if (!idx.indices.empty()) {
          index_val = lower_expr_value(*idx.indices[0]);
        }
        return emit_value(expr, MirIndexAccess{obj, index_val});
      },
      [&](const HirLambda& lam) -> MirValueId {
        auto* lam_fn = ctx_.alloc<MirFunction>();
        lam_fn->span = expr.span;
        if (lam.body != nullptr && lam.body->type != nullptr) {
          lam_fn->return_type = lam.body->type;
        }

        // Save and reset builder state.
        auto* saved_fn = current_fn_;
        auto* saved_block = current_block_;
        auto saved_value_id = next_value_id_;
        auto saved_block_id = next_block_id_;
        auto saved_locals = symbol_to_local_;

        current_fn_ = lam_fn;
        current_block_ = nullptr;
        next_value_id_ = 0;
        next_block_id_ = 0;
        symbol_to_local_.clear();

        for (const auto& param : lam.params) {
          declare_local(param.symbol, param.type, param.span,
                        /*is_param=*/true);
        }

        auto* entry_bb = fresh_block();
        switch_to_block(entry_bb);

        if (lam.body != nullptr) {
          auto body_val = lower_expr_value(*lam.body);
          emit_terminator(lam.body->span, MirReturn{body_val, true});
        } else if (!block_terminated()) {
          emit_terminator(expr.span, MirReturn{{}, false});
        }

        // Restore builder state.
        current_fn_ = saved_fn;
        current_block_ = saved_block;
        next_value_id_ = saved_value_id;
        next_block_id_ = saved_block_id;
        symbol_to_local_ = std::move(saved_locals);

        if (current_module_ != nullptr) {
          current_module_->functions.push_back(lam_fn);
        }

        return emit_value(expr, MirLambdaInst{lam_fn});
      },
  }, expr.payload);
}

// ---------------------------------------------------------------------------
// Expression lowering — place
// ---------------------------------------------------------------------------

auto MirBuilder::lower_expr_place(const HirExpr& expr) -> MirPlace {
  return std::visit(overloaded{
      [&](const HirSymbolRef& ref) -> MirPlace {
        if (ref.symbol != nullptr) {
          auto it = symbol_to_local_.find(ref.symbol);
          if (it != symbol_to_local_.end()) {
            return {.local = it->second, .projections = {}};
          }
        }
        error(expr.span, "cannot resolve symbol to local for place");
        return {};
      },
      [&](const HirField& field) -> MirPlace {
        auto base = lower_expr_place(*field.object);
        auto field_idx = resolve_field_index(field.object->type, field.field_name);
        if (!field_idx) {
          if (lowering_generic_template_) {
            // Template body: tolerate (see value-lowering counterpart).
          } else {
            error(field.object->span, "unresolved field '" + std::string(field.field_name) + "'");
          }
        }
        base.projections.push_back(
            {.kind = MirProjectionKind::Field,
             .field_name = field.field_name,
             .field_index = field_idx.value_or(0),
             .index_value = {}});
        return base;
      },
      [&](const HirIndex& idx) -> MirPlace {
        auto base = lower_expr_place(*idx.object);
        MirValueId index_val;
        if (!idx.indices.empty()) {
          index_val = lower_expr_value(*idx.indices[0]);
        }
        base.projections.push_back(
            {.kind = MirProjectionKind::Index,
             .field_name = {},
             .field_index = 0,
             .index_value = index_val});
        return base;
      },
      [&](const HirUnary& un) -> MirPlace {
        if (un.op == UnaryOp::Deref) {
          // Try to lower as a place first (e.g., *local_var).
          // If that fails (e.g., *fn_call()), lower as a value,
          // store to a temp, and create a place with Deref.
          bool operand_is_place =
              std::holds_alternative<HirSymbolRef>(un.operand->payload) ||
              std::holds_alternative<HirField>(un.operand->payload) ||
              std::holds_alternative<HirUnary>(un.operand->payload);
          if (operand_is_place) {
            auto base = lower_expr_place(*un.operand);
            base.projections.push_back(
                {.kind = MirProjectionKind::Deref,
                 .field_name = {},
                 .field_index = 0,
                 .index_value = {}});
            return base;
          }
          // Operand is a value expression (call, etc.) — store to temp.
          auto val = lower_expr_value(*un.operand);
          auto tmp_id = declare_local(nullptr, un.operand->type, expr.span);
          auto* tmp_place = ctx_.alloc<MirPlace>();
          tmp_place->local = tmp_id;
          emit_effect(expr.span, MirStore{tmp_place, val});
          MirPlace result;
          result.local = tmp_id;
          result.projections.push_back(
              {.kind = MirProjectionKind::Deref,
               .field_name = {},
               .field_index = 0,
               .index_value = {}});
          return result;
        }
        error(expr.span, "expression is not a valid place");
        return {};
      },
      [&](const auto& /*unused*/) -> MirPlace {
        error(expr.span, "expression is not a valid place");
        return {};
      },
  }, expr.payload);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

auto MirBuilder::fresh_value() -> MirValueId {
  return {.id = next_value_id_++};
}

auto MirBuilder::fresh_block() -> MirBlock* {
  auto* block = ctx_.alloc<MirBlock>();
  block->id = {.id = next_block_id_++};
  current_fn_->blocks.push_back(block);
  return block;
}

auto MirBuilder::declare_local(const Symbol* sym, const Type* type,
                               Span span, bool is_param) -> LocalId {
  LocalId lid = {.id = static_cast<uint32_t>(current_fn_->locals.size())};
  current_fn_->locals.push_back(
      {.id = lid, .symbol = sym, .type = type, .span = span,
       .is_param = is_param});
  if (sym != nullptr) {
    symbol_to_local_[sym] = lid;
  }
  return lid;
}

void MirBuilder::emit(MirInst* inst) {
  if (current_block_ != nullptr) {
    current_block_->insts.push_back(inst);
  }
}

void MirBuilder::switch_to_block(MirBlock* block) {
  current_block_ = block;
}

auto MirBuilder::block_terminated() const -> bool {
  if (current_block_ == nullptr) {
    return true;
  }
  if (current_block_->insts.empty()) {
    return false;
  }
  return is_terminator(current_block_->insts.back()->kind());
}

void MirBuilder::emit_region_exits(Span span, Carried carried) {
  for (size_t i = active_regions_.size(); i > 0; --i) {
    emit_region_exit(active_regions_[i - 1], span, &carried);
  }
}

void MirBuilder::emit_region_exits_from(size_t depth, Span span) {
  for (size_t i = active_regions_.size(); i > depth; --i) {
    emit_region_exit(active_regions_[i - 1], span, nullptr);
  }
}

void MirBuilder::emit_region_exit(const ActiveRegion& region, Span span, const Carried* carried) {
  const auto* exit = std::get_if<MirResourceExit>(&region.exit_payload);
  if (exit != nullptr && exit->region_kind == "memory") {
    // What leaves the domain is copied one domain outward before the
    // exit reclaims it.  A block or loop being left copies each
    // escaping binding the block stored to; a function being left
    // copies only what it returns -- its bindings are not read again.
    if (carried != nullptr) {
      if (carried->value != nullptr && carried->type != nullptr) {
        *carried->value = emit_copy_out(*carried->value, carried->type, span);
      }
      emit_effect(span, region.exit_payload);
      return;
    }
    for (const auto& escape : region.escapes) {
      auto* flag_place = ctx_.alloc<MirPlace>();
      flag_place->local = escape.flag;
      auto dirty = emit_value(types_.bool_type(), span, MirLoad{flag_place});
      auto* copy_bb = fresh_block();
      auto* next_bb = fresh_block();
      emit_terminator(span, MirCondBr{dirty, copy_bb->id, next_bb->id});
      switch_to_block(copy_bb);
      auto* binding_place = ctx_.alloc<MirPlace>();
      binding_place->local = escape.binding;
      auto current = emit_value(escape.type, span, MirLoad{binding_place});
      auto copied = emit_copy_out(current, escape.type, span);
      auto* store_place = ctx_.alloc<MirPlace>();
      store_place->local = escape.binding;
      emit_effect(span, MirStore{store_place, copied});
      emit_terminator(span, MirBr{next_bb->id});
      switch_to_block(next_bb);
    }
  }
  emit_effect(span, region.exit_payload);
}

auto MirBuilder::emit_copy_out(MirValueId value, const Type* type, Span span) -> MirValueId {
  if (type == nullptr || !owns_heap_memory(type)) {
    return value;
  }
  if (copy_out_symbol_ == nullptr) {
    error(span, "internal: no prelude `copy_out` to copy a value out of a resource block");
    return value;
  }
  auto callee = emit_value(copy_out_type_, span, MirFnRef{copy_out_symbol_});
  auto* args = ctx_.alloc<std::vector<MirValueId>>();
  args->push_back(value);
  auto* type_args = ctx_.alloc<std::vector<const Type*>>();
  type_args->push_back(type);
  return emit_value(type, span, MirCall{callee, args, type_args});
}

void MirBuilder::error(Span span, std::string message) {
  diagnostics_.push_back(Diagnostic::error(span, std::move(message)));
}

// ---------------------------------------------------------------------------
// Free-function entry point
// ---------------------------------------------------------------------------

auto build_mir(const HirProgram& program, MirContext& ctx, TypeContext& types) -> MirBuildResult {
  MirBuilder builder(ctx, types);
  return builder.build(program);
}

} // namespace dao
