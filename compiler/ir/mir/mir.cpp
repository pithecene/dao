#include "ir/mir/mir.h"
#include "ir/mir/mir_kind.h"

namespace dao {

// ---------------------------------------------------------------------------
// MirInst::kind() — derived from the active variant alternative.
// Variant order matches MirInstKind enum order exactly.
// ---------------------------------------------------------------------------

auto MirInst::kind() const -> MirInstKind {
  return std::visit(overloaded{
      [](const MirConstInt&)      { return MirInstKind::ConstInt; },
      [](const MirConstFloat&)    { return MirInstKind::ConstFloat; },
      [](const MirConstBool&)     { return MirInstKind::ConstBool; },
      [](const MirConstString&)   { return MirInstKind::ConstString; },
      [](const MirUnary&)         { return MirInstKind::Unary; },
      [](const MirBinary&)        { return MirInstKind::Binary; },
      [](const MirStore&)         { return MirInstKind::Store; },
      [](const MirLoad&)          { return MirInstKind::Load; },
      [](const MirPtrOp&)         { return MirInstKind::PtrOp; },
      [](const MirFieldAccess&)   { return MirInstKind::FieldAccess; },
      [](const MirIndexAccess&)   { return MirInstKind::IndexAccess; },
      [](const MirFnRef&)         { return MirInstKind::FnRef; },
      [](const MirCall&)          { return MirInstKind::Call; },
      [](const MirConstruct&)         { return MirInstKind::Construct; },
      [](const MirEnumConstruct&)    { return MirInstKind::EnumConstruct; },
      [](const MirEnumDiscriminant&) { return MirInstKind::EnumDiscriminant; },
      [](const MirEnumPayload&)      { return MirInstKind::EnumPayload; },
      [](const MirIterInit&)         { return MirInstKind::IterInit; },
      [](const MirIterHasNext&)   { return MirInstKind::IterHasNext; },
      [](const MirIterNext&)      { return MirInstKind::IterNext; },
      [](const MirIterDestroy&)   { return MirInstKind::IterDestroy; },
      [](const MirYieldInst&)     { return MirInstKind::Yield; },
      [](const MirModeEnter&)     { return MirInstKind::ModeEnter; },
      [](const MirModeExit&)      { return MirInstKind::ModeExit; },
      [](const MirResourceEnter&) { return MirInstKind::ResourceEnter; },
      [](const MirResourceExit&)  { return MirInstKind::ResourceExit; },
      [](const MirLambdaInst&)    { return MirInstKind::Lambda; },
      [](const MirBr&)            { return MirInstKind::Br; },
      [](const MirCondBr&)        { return MirInstKind::CondBr; },
      [](const MirReturn&)        { return MirInstKind::Return; },
  }, payload);
}

// ---------------------------------------------------------------------------
// MirInstKind utilities
// ---------------------------------------------------------------------------

auto mir_inst_kind_name(MirInstKind kind) -> const char* {
  switch (kind) {
  case MirInstKind::ConstInt:       return "const_int";
  case MirInstKind::ConstFloat:     return "const_float";
  case MirInstKind::ConstBool:      return "const_bool";
  case MirInstKind::ConstString:    return "const_string";
  case MirInstKind::Unary:          return "unary";
  case MirInstKind::Binary:         return "binary";
  case MirInstKind::Store:          return "store";
  case MirInstKind::Load:           return "load";
  case MirInstKind::PtrOp:          return "ptr_op";
  case MirInstKind::FieldAccess:    return "field";
  case MirInstKind::IndexAccess:    return "index";
  case MirInstKind::FnRef:          return "fn_ref";
  case MirInstKind::Call:           return "call";
  case MirInstKind::Construct:         return "construct";
  case MirInstKind::EnumConstruct:    return "enum_construct";
  case MirInstKind::EnumDiscriminant: return "enum_discriminant";
  case MirInstKind::EnumPayload:      return "enum_payload";
  case MirInstKind::IterInit:         return "iter_init";
  case MirInstKind::IterHasNext:    return "iter_has_next";
  case MirInstKind::IterNext:       return "iter_next";
  case MirInstKind::IterDestroy:   return "iter_destroy";
  case MirInstKind::Yield:          return "yield";
  case MirInstKind::ModeEnter:      return "mode_enter";
  case MirInstKind::ModeExit:       return "mode_exit";
  case MirInstKind::ResourceEnter:  return "resource_enter";
  case MirInstKind::ResourceExit:   return "resource_exit";
  case MirInstKind::Lambda:         return "lambda";
  case MirInstKind::Br:             return "br";
  case MirInstKind::CondBr:         return "cond_br";
  case MirInstKind::Return:         return "return";
  }
  return "<unknown>";
}

auto is_terminator(MirInstKind kind) -> bool {
  switch (kind) {
  case MirInstKind::Br:
  case MirInstKind::CondBr:
  case MirInstKind::Return:
    return true;
  default:
    return false;
  }
}

} // namespace dao
