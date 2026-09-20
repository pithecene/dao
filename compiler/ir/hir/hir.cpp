#include "ir/hir/hir.h"
#include "ir/hir/hir_kind.h"

namespace dao {

// ---------------------------------------------------------------------------
// kind() — derived from active variant alternative.
// ---------------------------------------------------------------------------

auto HirDecl::kind() const -> HirKind {
  return std::visit(overloaded{
      [](const HirFunction&) { return HirKind::Function; },
      [](const HirClassDecl&) { return HirKind::ClassDecl; },
  }, payload);
}

auto HirStmt::kind() const -> HirKind {
  return std::visit(overloaded{
      [](const HirLet&) { return HirKind::Let; },
      [](const HirAssign&) { return HirKind::Assign; },
      [](const HirIf&) { return HirKind::If; },
      [](const HirWhile&) { return HirKind::While; },
      [](const HirFor&) { return HirKind::For; },
      [](const HirReturn&) { return HirKind::Return; },
      [](const HirYield&) { return HirKind::Yield; },
      [](const HirBreak&) { return HirKind::Break; },
      [](const HirExprStmt&) { return HirKind::ExprStmt; },
      [](const HirMode&) { return HirKind::Mode; },
      [](const HirResource&) { return HirKind::Resource; },
  }, payload);
}

auto HirExpr::kind() const -> HirKind {
  return std::visit(overloaded{
      [](const HirIntLiteral&) { return HirKind::IntLiteral; },
      [](const HirFloatLiteral&) { return HirKind::FloatLiteral; },
      [](const HirStringLiteral&) { return HirKind::StringLiteral; },
      [](const HirBoolLiteral&) { return HirKind::BoolLiteral; },
      [](const HirSymbolRef&) { return HirKind::SymbolRef; },
      [](const HirUnary&) { return HirKind::Unary; },
      [](const HirBinary&) { return HirKind::Binary; },
      [](const HirCall&) { return HirKind::Call; },
      [](const HirConstruct&) { return HirKind::Construct; },
      [](const HirEnumConstruct&) { return HirKind::EnumConstruct; },
      [](const HirEnumDiscriminant&) { return HirKind::EnumDiscriminant; },
      [](const HirEnumPayload&) { return HirKind::EnumPayload; },
      [](const HirField&) { return HirKind::Field; },
      [](const HirIndex&) { return HirKind::Index; },
      [](const HirPipe&) { return HirKind::Pipe; },
      [](const HirTry&) { return HirKind::Try; },
      [](const HirLambda&) { return HirKind::Lambda; },
      [](const HirPtrOp&) { return HirKind::PtrOp; },
  }, payload);
}

// ---------------------------------------------------------------------------
// HirKind name lookup.
// ---------------------------------------------------------------------------

auto hir_kind_name(HirKind kind) -> const char* {
  switch (kind) {
  case HirKind::Module:        return "Module";
  case HirKind::Function:      return "Function";
  case HirKind::ClassDecl:     return "ClassDecl";
  case HirKind::Let:           return "Let";
  case HirKind::Assign:        return "Assign";
  case HirKind::If:            return "If";
  case HirKind::While:         return "While";
  case HirKind::For:           return "For";
  case HirKind::Return:        return "Return";
  case HirKind::Yield:         return "Yield";
  case HirKind::Break:         return "Break";
  case HirKind::ExprStmt:      return "ExprStmt";
  case HirKind::Mode:          return "Mode";
  case HirKind::Resource:      return "Resource";
  case HirKind::IntLiteral:    return "IntLiteral";
  case HirKind::FloatLiteral:  return "FloatLiteral";
  case HirKind::StringLiteral: return "StringLiteral";
  case HirKind::BoolLiteral:   return "BoolLiteral";
  case HirKind::SymbolRef:     return "SymbolRef";
  case HirKind::Unary:         return "Unary";
  case HirKind::Binary:        return "Binary";
  case HirKind::Call:          return "Call";
  case HirKind::Construct:     return "Construct";
  case HirKind::EnumConstruct:     return "EnumConstruct";
  case HirKind::EnumDiscriminant:  return "EnumDiscriminant";
  case HirKind::EnumPayload:       return "EnumPayload";
  case HirKind::Field:         return "Field";
  case HirKind::Index:         return "Index";
  case HirKind::Pipe:          return "Pipe";
  case HirKind::Try:           return "Try";
  case HirKind::Lambda:        return "Lambda";
  case HirKind::PtrOp:         return "PtrOp";
  }
  return "<unknown>";
}

// ---------------------------------------------------------------------------
// Mode kind helpers.
// ---------------------------------------------------------------------------

auto hir_mode_kind_from_name(std::string_view name) -> HirModeKind {
  if (name == "unsafe") return HirModeKind::Unsafe;
  if (name == "gpu") return HirModeKind::Gpu;
  if (name == "parallel") return HirModeKind::Parallel;
  return HirModeKind::Other;
}

auto hir_mode_kind_name(HirModeKind kind) -> const char* {
  switch (kind) {
  case HirModeKind::Unsafe:   return "unsafe";
  case HirModeKind::Gpu:      return "gpu";
  case HirModeKind::Parallel: return "parallel";
  case HirModeKind::Other:    return "other";
  }
  return "<unknown>";
}

} // namespace dao
