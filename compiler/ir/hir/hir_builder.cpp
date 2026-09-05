#include "ir/hir/hir_builder.h"

#include "frontend/types/type_printer.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string>

namespace dao {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

HirBuilder::HirBuilder(HirContext& ctx, const ResolveResult& resolve,
                       const TypeCheckResult& typed)
    : ctx_(ctx), resolve_(resolve), typed_(typed) {
  for (const auto& sym : resolve_.context.symbols()) {
    if (sym->decl_span.length > 0) {
      decl_symbols_[sym->decl_span.offset] = sym.get();
    }
  }
}

// ---------------------------------------------------------------------------
// Top-level
// ---------------------------------------------------------------------------

auto HirBuilder::build(std::span<const FileNode* const> files) -> HirBuildResult {
  std::vector<HirDecl*> decls;
  Span module_span{};
  bool first = true;
  for (const auto* file : files) {
    // The module span covers every file (disjoint ranges of one
    // program-wide offset space).
    if (first) {
      module_span = file->span;
      first = false;
    } else {
      auto begin = std::min(module_span.offset, file->span.offset);
      auto end = std::max(module_span.offset + module_span.length,
                          file->span.offset + file->span.length);
      module_span = Span{.offset = begin, .length = end - begin};
    }
    for (const auto* decl : file->declarations) {
      auto* hir_decl = lower_decl(decl);
      if (hir_decl != nullptr) {
        decls.push_back(hir_decl);
      }
    }
  }

  // Append extend methods lowered as standalone functions.
  for (auto* ext_decl : extend_decls_) {
    decls.push_back(ext_decl);
  }

  auto* mod = ctx_.alloc<HirModule>(module_span, std::move(decls));
  return {.module = mod, .diagnostics = std::move(diagnostics_)};
}

// ---------------------------------------------------------------------------
// Declaration lowering
// ---------------------------------------------------------------------------

auto HirBuilder::lower_decl(const Decl* decl) -> HirDecl* {
  switch (decl->kind()) {
  case NodeKind::FunctionDecl:
    return lower_function(decl);
  case NodeKind::ClassDecl:
    return lower_class(decl);
  case NodeKind::ExtendDecl: {
    // Lower each extend method as a standalone function.
    // These are emitted as real functions so method dispatch can
    // reference them via MirFnRef.
    const auto& ext = decl->as<ExtendDecl>();
    for (const auto* method : ext.methods) {
      auto* hir_fn = lower_function(method);
      if (hir_fn != nullptr) {
        extend_decls_.push_back(hir_fn);
      }
    }
    return nullptr; // extend itself isn't a declaration
  }
  default:
    return nullptr;
  }
}

auto HirBuilder::lower_function(const Decl* decl) -> HirDecl* {
  const auto& fn = decl->as<FunctionDecl>();
  const auto* sym = find_symbol_at_decl(fn.name_span.offset);

  // Build params.
  std::vector<HirParam> hir_params;
  for (const auto& param : fn.params) {
    const auto* param_sym = find_symbol_at_decl(param.name_span.offset);
    const Type* param_type = nullptr;

    if (param_sym != nullptr) {
      const auto* fn_type_raw = typed_.typed.decl_type(decl);
      if (fn_type_raw != nullptr &&
          fn_type_raw->kind() == TypeKind::Function) {
        const auto* fn_type =
            static_cast<const TypeFunction*>(fn_type_raw);
        auto idx = static_cast<size_t>(&param - fn.params.data());
        if (idx < fn_type->param_types().size()) {
          param_type = fn_type->param_types()[idx];
        }
      }
    }

    hir_params.push_back({param_sym, param_type, param.name_span});
  }

  // Return type.
  const Type* ret_type = nullptr;
  const auto* fn_type_raw = typed_.typed.decl_type(decl);
  if (fn_type_raw != nullptr && fn_type_raw->kind() == TypeKind::Function) {
    ret_type = static_cast<const TypeFunction*>(fn_type_raw)->return_type();
  }

  // Body: extern functions have no body to lower.
  std::vector<HirStmt*> hir_body;
  if (fn.is_extern) {
    // No body for extern declarations.
  } else if (fn.is_expr_bodied()) {
    auto* expr = lower_expr(fn.expr_body);
    if (expr != nullptr) {
      auto* ret = ctx_.alloc<HirStmt>(fn.expr_body->span,
                                       HirReturn{expr});
      hir_body.push_back(ret);
    }
  } else {
    hir_body = lower_body(fn.body);
  }

  return ctx_.alloc<HirDecl>(
      decl->span,
      HirFunction{sym, std::move(hir_params), ret_type,
                  std::move(hir_body), fn.is_extern,
                  /*has_type_params=*/!fn.type_params.empty()});
}

auto HirBuilder::lower_class(const Decl* decl) -> HirDecl* {
  const auto& st = decl->as<ClassDecl>();
  const auto* sym = find_symbol_at_decl(st.name_span.offset);

  const TypeStruct* struct_type = nullptr;
  const auto* decl_type = typed_.typed.decl_type(decl);
  if (decl_type != nullptr && decl_type->kind() == TypeKind::Struct) {
    struct_type = static_cast<const TypeStruct*>(decl_type);
  }

  // Lower direct class methods as standalone functions.
  for (const auto* method : st.methods) {
    auto* hir_fn = lower_function(method);
    if (hir_fn != nullptr) {
      extend_decls_.push_back(hir_fn);
    }
  }

  // Lower conformance-block methods as standalone functions.
  for (const auto& conf : st.conformances) {
    for (const auto* method : conf.methods) {
      auto* hir_fn = lower_function(method);
      if (hir_fn != nullptr) {
        extend_decls_.push_back(hir_fn);
      }
    }
  }

  return ctx_.alloc<HirDecl>(decl->span, HirClassDecl{sym, struct_type});
}

// ---------------------------------------------------------------------------
// Statement lowering
// ---------------------------------------------------------------------------

auto HirBuilder::lower_body(const std::vector<Stmt*>& body)
    -> std::vector<HirStmt*> {
  std::vector<HirStmt*> result;
  for (const auto* stmt : body) {
    if (stmt->kind() == NodeKind::MatchStatement) {
      lower_match_into(stmt, result);
    } else {
      auto* hir = lower_stmt(stmt);
      if (hir != nullptr) {
        result.push_back(hir);
      }
    }
  }
  return result;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto HirBuilder::lower_stmt(const Stmt* stmt) -> HirStmt* {
  switch (stmt->kind()) {
  case NodeKind::LetStatement: {
    const auto& let = stmt->as<LetStatement>();
    const auto* sym = find_symbol_at_decl(let.name_span.offset);
    const auto* type = typed_.typed.local_type(stmt);
    HirExpr* init = nullptr;
    if (let.initializer != nullptr) {
      init = lower_expr(let.initializer);
    }
    return ctx_.alloc<HirStmt>(stmt->span, HirLet{sym, type, init});
  }

  case NodeKind::Assignment: {
    const auto& assign = stmt->as<Assignment>();
    auto* target = lower_expr(assign.target);
    auto* value = lower_expr(assign.value);
    return ctx_.alloc<HirStmt>(stmt->span, HirAssign{target, value});
  }

  case NodeKind::IfStatement: {
    const auto& ifn = stmt->as<IfStatement>();
    auto* cond = lower_expr(ifn.condition);
    auto then_body = lower_body(ifn.then_body);
    auto else_body = lower_body(ifn.else_body);
    return ctx_.alloc<HirStmt>(
        stmt->span,
        HirIf{cond, std::move(then_body), std::move(else_body)});
  }

  case NodeKind::WhileStatement: {
    const auto& wh = stmt->as<WhileStatement>();
    auto* cond = lower_expr(wh.condition);
    auto body = lower_body(wh.body);
    return ctx_.alloc<HirStmt>(stmt->span,
                                HirWhile{cond, std::move(body)});
  }

  case NodeKind::ForStatement: {
    const auto& fo = stmt->as<ForStatement>();
    const auto* var_sym = find_symbol_at_decl(fo.var_span.offset);
    auto* iterable = lower_expr(fo.iterable);
    auto body = lower_body(fo.body);
    return ctx_.alloc<HirStmt>(
        stmt->span, HirFor{var_sym, iterable, std::move(body)});
  }

  case NodeKind::ModeBlock: {
    const auto& mb = stmt->as<ModeBlock>();
    auto mode = hir_mode_kind_from_name(mb.mode_name);
    auto body = lower_body(mb.body);
    return ctx_.alloc<HirStmt>(
        stmt->span, HirMode{mode, mb.mode_name, std::move(body)});
  }

  case NodeKind::ResourceBlock: {
    const auto& rb = stmt->as<ResourceBlock>();
    auto body = lower_body(rb.body);
    return ctx_.alloc<HirStmt>(
        stmt->span,
        HirResource{rb.resource_kind, rb.resource_name,
                    std::move(body)});
  }

  case NodeKind::YieldStatement: {
    const auto& yield = stmt->as<YieldStatement>();
    auto* value = lower_expr(yield.value);
    return ctx_.alloc<HirStmt>(stmt->span, HirYield{value});
  }

  case NodeKind::MatchStatement: {
    // Match is lowered by lower_match_into() which emits multiple
    // statements (let binding + if/else chain). When reached here
    // from a context that expects a single statement (e.g. match
    // inside if-else), wrap in a single-element body.
    std::vector<HirStmt*> stmts;
    lower_match_into(stmt, stmts);
    // Return the last statement (the if/else chain).
    return stmts.empty() ? nullptr : stmts.back();
  }

  case NodeKind::BreakStatement:
    return ctx_.alloc<HirStmt>(stmt->span, HirBreak{});

  case NodeKind::ReturnStatement: {
    const auto& ret = stmt->as<ReturnStatement>();
    HirExpr* value = nullptr;
    if (ret.value != nullptr) {
      value = lower_expr(ret.value);
    }
    return ctx_.alloc<HirStmt>(stmt->span, HirReturn{value});
  }

  case NodeKind::ExpressionStatement: {
    const auto& es = stmt->as<ExpressionStatement>();
    auto* expr = lower_expr(es.expr);
    return ctx_.alloc<HirStmt>(stmt->span, HirExprStmt{expr});
  }

  default:
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Expression lowering
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Match lowering — emits a let binding for the scrutinee followed by
// a chain of if/else comparisons referencing the bound variable.
// ---------------------------------------------------------------------------

void HirBuilder::lower_match_into(const Stmt* stmt,
                                  std::vector<HirStmt*>& out) {
  const auto& match = stmt->as<MatchStmt>();
  auto* scrutinee_expr = lower_expr(match.scrutinee);
  const auto* scrutinee_type = expr_type(match.scrutinee);

  // Create a synthetic symbol for the scrutinee temporary.
  auto* scrutinee_sym = ctx_.alloc<Symbol>(Symbol{
      .kind = SymbolKind::Local,
      .name = "__match_scrutinee",
      .decl_span = stmt->span,
      .decl = nullptr});

  // Emit: let __match_scrutinee = <scrutinee>
  out.push_back(ctx_.alloc<HirStmt>(
      stmt->span,
      HirLet{scrutinee_sym, scrutinee_type, scrutinee_expr}));

  // Build a reference expression for the bound scrutinee.
  auto* scrutinee_ref = ctx_.alloc<HirExpr>(
      stmt->span, scrutinee_type, HirSymbolRef{scrutinee_sym});

  // Determine if the enum has any payload variants — if so, compare
  // against the discriminant rather than the whole value.
  bool has_payload_variants = false;
  const TypeEnum* enum_type = nullptr;
  if (scrutinee_type != nullptr &&
      scrutinee_type->kind() == TypeKind::Enum) {
    enum_type = static_cast<const TypeEnum*>(scrutinee_type);
    for (const auto& variant : enum_type->variants()) {
      if (!variant.payload_types.empty()) {
        has_payload_variants = true;
        break;
      }
    }
  }

  // For payload-bearing enums, compare against extracted discriminant.
  HirExpr* compare_value = scrutinee_ref;
  if (has_payload_variants) {
    compare_value = ctx_.alloc<HirExpr>(
        stmt->span, nullptr, HirEnumDiscriminant{scrutinee_ref});
  }

  // Build the if/else chain from last arm to first.
  HirStmt* chain = nullptr;
  for (auto it = match.arms.rbegin(); it != match.arms.rend(); ++it) {
    auto* pattern = lower_expr(it->pattern);

    // For payload-bearing enums, the pattern is already an integer
    // (variant index) from lower_expr on the FieldExpr.
    auto arm_body = lower_body(it->body);

    // Prepend payload extraction let-bindings for arms with bindings.
    // Extract the variant name from either FieldExpr (Enum.Variant) or
    // QualifiedName (Enum::Variant) patterns.
    std::string_view pattern_variant_name;
    if (it->pattern->is<FieldExpr>()) {
      pattern_variant_name = it->pattern->as<FieldExpr>().field;
    } else if (it->pattern->is<QualifiedName>()) {
      const auto& qn = it->pattern->as<QualifiedName>();
      if (qn.segments.size() >= 2) {
        pattern_variant_name = qn.segments.back();
      }
    } else if (it->pattern->is<CallExpr>()) {
      // Enum::Variant(bindings) parsed as CallExpr with QualifiedName callee
      const auto& call = it->pattern->as<CallExpr>();
      if (call.callee->is<FieldExpr>()) {
        pattern_variant_name = call.callee->as<FieldExpr>().field;
      } else if (call.callee->is<QualifiedName>()) {
        const auto& qn = call.callee->as<QualifiedName>();
        if (qn.segments.size() >= 2) {
          pattern_variant_name = qn.segments.back();
        }
      }
    }
    if (!it->bindings.empty() && enum_type != nullptr &&
        !pattern_variant_name.empty()) {
      for (size_t vi = 0; vi < enum_type->variants().size(); ++vi) {
        if (enum_type->variants()[vi].name != pattern_variant_name) {
          continue;
        }
        const auto& variant = enum_type->variants()[vi];
        // Insert payload extraction lets at the front of the arm body.
        std::vector<HirStmt*> bindings_and_body;
        for (size_t fi = 0; fi < it->bindings.size(); ++fi) {
          // Find the symbol registered by the resolver for this binding.
          const Symbol* binding_sym = nullptr;
          for (const auto& sym_ptr : resolve_.context.symbols()) {
            if (sym_ptr->name == it->bindings[fi] &&
                sym_ptr->decl_span.offset ==
                    it->binding_spans[fi].offset) {
              binding_sym = sym_ptr.get();
              break;
            }
          }
          if (binding_sym == nullptr) {
            continue;
          }
          const Type* field_type =
              fi < variant.payload_types.size()
                  ? variant.payload_types[fi]
                  : nullptr;
          auto* extract_expr = ctx_.alloc<HirExpr>(
              stmt->span, field_type,
              HirEnumPayload{scrutinee_ref,
                             static_cast<uint32_t>(vi),
                             static_cast<uint32_t>(fi)});
          bindings_and_body.push_back(ctx_.alloc<HirStmt>(
              stmt->span,
              HirLet{binding_sym, field_type, extract_expr}));
        }
        for (auto* s : arm_body) {
          bindings_and_body.push_back(s);
        }
        arm_body = std::move(bindings_and_body);
        break;
      }
    }

    // Emit HirLet for `as` binding: `Pattern as name:` binds the
    // scrutinee value to `name` in the arm body.
    if (!it->as_binding.empty()) {
      const Symbol* as_sym = nullptr;
      for (const auto& sym_ptr : resolve_.context.symbols()) {
        if (sym_ptr->name == it->as_binding &&
            sym_ptr->decl_span.offset == it->as_binding_span.offset) {
          as_sym = sym_ptr.get();
          break;
        }
      }
      if (as_sym != nullptr) {
        auto* as_ref = ctx_.alloc<HirExpr>(
            stmt->span, scrutinee_type, HirSymbolRef{scrutinee_sym});
        auto* as_let = ctx_.alloc<HirStmt>(
            stmt->span,
            HirLet{as_sym, scrutinee_type, as_ref});
        arm_body.insert(arm_body.begin(), as_let);
      }
    }

    auto* cond = ctx_.alloc<HirExpr>(
        stmt->span, nullptr,
        HirBinary{BinaryOp::EqEq, compare_value, pattern});
    std::vector<HirStmt*> else_body;
    if (chain != nullptr) {
      else_body.push_back(chain);
    }
    chain = ctx_.alloc<HirStmt>(
        stmt->span,
        HirIf{cond, std::move(arm_body), std::move(else_body)});
  }
  if (chain != nullptr) {
    out.push_back(chain);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto HirBuilder::lower_expr(const Expr* expr) -> HirExpr* {
  if (expr == nullptr) {
    return nullptr;
  }

  const auto* type = expr_type(expr);
  auto span = expr->span;

  switch (expr->kind()) {
  case NodeKind::IntLiteral: {
    const auto& lit = expr->as<IntLiteral>();
    int64_t val = 0;
    auto text = lit.text;
    std::from_chars(text.data(), text.data() + text.size(), val);
    return ctx_.alloc<HirExpr>(span, type, HirIntLiteral{val});
  }

  case NodeKind::FloatLiteral: {
    const auto& lit = expr->as<FloatLiteral>();
    double val = std::strtod(std::string(lit.text).c_str(), nullptr);
    return ctx_.alloc<HirExpr>(span, type, HirFloatLiteral{val});
  }

  case NodeKind::StringLiteral: {
    const auto& lit = expr->as<StringLiteral>();
    return ctx_.alloc<HirExpr>(span, type, HirStringLiteral{lit.text});
  }

  case NodeKind::BoolLiteral: {
    const auto& lit = expr->as<BoolLiteral>();
    return ctx_.alloc<HirExpr>(span, type, HirBoolLiteral{lit.value});
  }

  case NodeKind::Identifier: {
    const auto* sym = find_symbol_at_use(expr->span.offset);
    return ctx_.alloc<HirExpr>(span, type, HirSymbolRef{sym});
  }

  case NodeKind::QualifiedName: {
    // QualifiedName may be:
    // 1. Enum variant access (Enum::Variant) — lower to discriminant
    // 2. Static method call (Type::method) — lower to symbol ref
    const auto& qn = expr->as<QualifiedName>();
    if (type != nullptr && type->kind() == TypeKind::Enum &&
        qn.segments.size() >= 2) {
      const auto* enum_type = static_cast<const TypeEnum*>(type);
      auto variant_name = qn.segments.back();
      for (size_t i = 0; i < enum_type->variants().size(); ++i) {
        if (enum_type->variants()[i].name == variant_name) {
          return ctx_.alloc<HirExpr>(
              span, type, HirIntLiteral{static_cast<int64_t>(i)});
        }
      }
    }
    const auto* sym = find_symbol_at_use(expr->span.offset);
    return ctx_.alloc<HirExpr>(span, type, HirSymbolRef{sym});
  }

  case NodeKind::UnaryExpr: {
    const auto& un = expr->as<UnaryExpr>();
    auto* operand = lower_expr(un.operand);
    return ctx_.alloc<HirExpr>(span, type, HirUnary{un.op, operand});
  }

  case NodeKind::BinaryExpr: {
    const auto& bin = expr->as<BinaryExpr>();
    auto* left = lower_expr(bin.left);
    auto* right = lower_expr(bin.right);
    return ctx_.alloc<HirExpr>(span, type,
                                HirBinary{bin.op, left, right});
  }

  case NodeKind::CallExpr: {
    const auto& call = expr->as<CallExpr>();

    // Constructor call: callee must be a Type symbol whose type is a
    // struct, not any expression that happens to have struct type.
    const auto* callee_type = expr_type(call.callee);
    if (callee_type != nullptr && callee_type->kind() == TypeKind::Struct &&
        call.callee->is<IdentifierExpr>()) {
      const auto* sym = find_symbol_at_use(call.callee->span.offset);
      if (sym != nullptr && sym->kind == SymbolKind::Type) {
        const auto* struct_type =
            static_cast<const TypeStruct*>(callee_type);
        std::vector<HirExpr*> args;
        for (const auto* arg : call.args) {
          args.push_back(lower_expr(arg));
        }
        return ctx_.alloc<HirExpr>(
            span, type, HirConstruct{struct_type, std::move(args)});
      }
    }

    // Enum variant construction: Enum.Variant(42) or Enum::Variant(42)
    // Use the CallExpr's type (which may be instantiated for generic enums)
    // rather than the callee's type (which may be the uninstantiated generic).
    if (callee_type != nullptr && callee_type->kind() == TypeKind::Enum) {
      std::string_view variant_field;
      if (call.callee->is<FieldExpr>()) {
        variant_field = call.callee->as<FieldExpr>().field;
      } else if (call.callee->is<QualifiedName>()) {
        const auto& qn = call.callee->as<QualifiedName>();
        if (qn.segments.size() >= 2) {
          variant_field = qn.segments.back();
        }
      }
      if (!variant_field.empty()) {
        const auto* enum_type =
            (type != nullptr && type->kind() == TypeKind::Enum)
                ? static_cast<const TypeEnum*>(type)
                : static_cast<const TypeEnum*>(callee_type);
        for (size_t i = 0; i < enum_type->variants().size(); ++i) {
          if (enum_type->variants()[i].name == variant_field &&
              !enum_type->variants()[i].payload_types.empty()) {
            std::vector<HirExpr*> payload_args;
            for (const auto* arg : call.args) {
              payload_args.push_back(lower_expr(arg));
            }
            return ctx_.alloc<HirExpr>(
                span, type,
                HirEnumConstruct{enum_type, static_cast<uint32_t>(i),
                                 std::move(payload_args)});
          }
        }
      }
    }

    // Method call desugaring: x.method(args) → method(x, args)
    // when the type checker resolved the FieldExpr as a method.
    if (call.callee->is<FieldExpr>()) {
      const auto* method_decl = typed_.typed.method_resolution(call.callee);
      if (method_decl != nullptr) {
        const auto& field = call.callee->as<FieldExpr>();
        // Look up the method function's symbol by matching the Decl*.
        const Symbol* method_sym = nullptr;
        for (const auto& sym_ptr : resolve_.context.symbols()) {
          if (sym_ptr->kind == SymbolKind::Function && sym_ptr->decl == method_decl) {
            method_sym = sym_ptr.get();
            break;
          }
        }
        // Fallback: for concept methods on generic type parameters,
        // the concept method Decl* won't match any resolver symbol
        // (only concrete extend methods have symbols). Search by
        // mangled name "TypeName.methodName" instead.
        if (method_sym == nullptr) {
          auto* obj_type = typed_.typed.expr_type(field.object);
          if (obj_type != nullptr) {
            auto mangled = std::string(print_type(obj_type)) + "." + std::string(field.field);
            for (const auto& sym_ptr : resolve_.context.symbols()) {
              if (sym_ptr->kind == SymbolKind::Function && sym_ptr->name == mangled) {
                method_sym = sym_ptr.get();
                break;
              }
            }
          }
        }
        if (method_sym != nullptr) {
          auto* callee_ref = ctx_.alloc<HirExpr>(
              call.callee->span, expr_type(call.callee),
              HirSymbolRef{method_sym});
          std::vector<HirExpr*> args;
          args.push_back(lower_expr(field.object)); // self
          for (const auto* arg : call.args) {
            args.push_back(lower_expr(arg));
          }
          return ctx_.alloc<HirExpr>(span, type,
                                      HirCall{callee_ref, std::move(args)});
        }
        // Concept method on a generic type parameter: no concrete
        // symbol exists because concept methods don't have resolver
        // symbols (only concrete extend methods do).  Falls through
        // to the normal call path, which lowers the FieldExpr callee
        // as an HirField.
        //
        // The MIR builder lowers this into a generic template body
        // (not the main module).  The unresolved field access is
        // tolerated during template lowering and resolved when the
        // monomorphizer clones and specializes the function with
        // concrete type arguments (Task 28).
      }
    }

    // Normal function call.
    auto* callee = lower_expr(call.callee);
    std::vector<HirExpr*> args;
    for (const auto* arg : call.args) {
      args.push_back(lower_expr(arg));
    }

    // Propagate explicit type arguments resolved by the type checker.
    std::vector<const Type*> explicit_type_args;
    const auto* resolved_ta = typed_.typed.call_type_args(expr);
    if (resolved_ta != nullptr) {
      explicit_type_args = *resolved_ta;
    }
    return ctx_.alloc<HirExpr>(span, type,
                                HirCall{callee, std::move(args),
                                        std::move(explicit_type_args)});
  }

  case NodeKind::PipeExpr: {
    const auto& pipe = expr->as<PipeExpr>();
    auto* left = lower_expr(pipe.left);
    auto* right = lower_expr(pipe.right);
    return ctx_.alloc<HirExpr>(span, type, HirPipe{left, right});
  }

  case NodeKind::TryExpr: {
    const auto& try_expr = expr->as<TryExpr>();
    auto* operand = lower_expr(try_expr.operand);
    const TypeEnum* enum_type = nullptr;
    if (operand != nullptr && operand->type != nullptr &&
        operand->type->kind() == TypeKind::Enum) {
      enum_type = static_cast<const TypeEnum*>(operand->type);
    }
    return ctx_.alloc<HirExpr>(span, type, HirTry{operand, enum_type});
  }

  case NodeKind::FieldExpr: {
    const auto& field = expr->as<FieldExpr>();
    // Enum variant access: lower to integer constant with enum type.
    if (type != nullptr && type->kind() == TypeKind::Enum) {
      const auto* enum_type = static_cast<const TypeEnum*>(type);
      for (size_t i = 0; i < enum_type->variants().size(); ++i) {
        if (enum_type->variants()[i].name == field.field) {
          return ctx_.alloc<HirExpr>(span, type,
                                      HirIntLiteral{static_cast<int64_t>(i)});
        }
      }
    }
    auto* object = lower_expr(field.object);
    return ctx_.alloc<HirExpr>(span, type,
                                HirField{object, field.field});
  }

  case NodeKind::IndexExpr: {
    const auto& idx = expr->as<IndexExpr>();
    auto* object = lower_expr(idx.object);
    std::vector<HirExpr*> indices;
    for (const auto* i : idx.indices) {
      indices.push_back(lower_expr(i));
    }
    return ctx_.alloc<HirExpr>(span, type,
                                HirIndex{object, std::move(indices)});
  }

  case NodeKind::Lambda: {
    const auto& lam = expr->as<LambdaExpr>();
    std::vector<HirParam> params;
    const TypeFunction* fn_type = nullptr;
    if (type != nullptr && type->kind() == TypeKind::Function) {
      fn_type = static_cast<const TypeFunction*>(type);
    }
    for (size_t i = 0; i < lam.params.size(); ++i) {
      const auto& [name, param_span] = lam.params[i];
      const auto* param_sym = find_symbol_at_decl(param_span.offset);
      const Type* param_type = nullptr;
      if (fn_type != nullptr && i < fn_type->param_types().size()) {
        param_type = fn_type->param_types()[i];
      }
      params.push_back({param_sym, param_type, param_span});
    }
    auto* body = lower_expr(lam.body);
    return ctx_.alloc<HirExpr>(span, type,
                                HirLambda{std::move(params), body});
  }

  case NodeKind::ErrorExpr:
    // Recovery placeholder — skip silently.
    return nullptr;
  default:
    error(expr->span, "unsupported expression in HIR builder");
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

auto HirBuilder::find_symbol_at_decl(uint32_t offset) -> const Symbol* {
  auto it = decl_symbols_.find(offset);
  return it != decl_symbols_.end() ? it->second : nullptr;
}

auto HirBuilder::find_symbol_at_use(uint32_t offset) -> const Symbol* {
  auto it = resolve_.uses.find(offset);
  return it != resolve_.uses.end() ? it->second : nullptr;
}

auto HirBuilder::expr_type(const Expr* expr) -> const Type* {
  return typed_.typed.expr_type(expr);
}

void HirBuilder::error(Span span, std::string message) {
  diagnostics_.push_back(Diagnostic::error(span, std::move(message)));
}

// ---------------------------------------------------------------------------
// Free-function entry point
// ---------------------------------------------------------------------------

auto build_hir(std::span<const FileNode* const> files, const ResolveResult& resolve,
               const TypeCheckResult& typed, HirContext& ctx)
    -> HirBuildResult {
  HirBuilder builder(ctx, resolve, typed);
  return builder.build(files);
}

auto build_hir(const Program& program, const ResolveResult& resolve,
               const TypeCheckResult& typed, HirContext& ctx)
    -> HirBuildResult {
  auto nodes = program.file_nodes();
  return build_hir(nodes, resolve, typed, ctx);
}

auto build_hir(const FileNode& file, const ResolveResult& resolve,
               const TypeCheckResult& typed, HirContext& ctx)
    -> HirBuildResult {
  const FileNode* files[] = {&file};
  return build_hir(std::span<const FileNode* const>(files), resolve, typed, ctx);
}

} // namespace dao
