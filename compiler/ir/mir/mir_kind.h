#ifndef DAO_IR_MIR_MIR_KIND_H
#define DAO_IR_MIR_MIR_KIND_H

#include <cstdint>

namespace dao {

enum class MirInstKind : std::uint8_t {
  // Constants
  ConstInt,
  ConstFloat,
  ConstBool,
  ConstString,

  // Arithmetic / logic
  Unary,
  Binary,

  // Memory / storage
  Store,
  Load,
  PtrOp,

  // Aggregate access (value-producing)
  FieldAccess,
  IndexAccess,

  // Function reference (direct reference to a named function symbol)
  FnRef,

  // Calls
  Call,

  // Construction
  Construct,

  // Enum operations
  EnumConstruct,
  EnumDiscriminant,
  EnumPayload,

  // Iteration (narrow model)
  IterInit,
  IterHasNext,
  IterNext,
  IterDestroy,

  // Coroutine
  Yield,

  // Mode/resource regions
  ModeEnter,
  ModeExit,
  ResourceEnter,
  ResourceExit,

  // Lambda
  Lambda,

  // Terminators
  Br,
  CondBr,
  Return,
};

auto mir_inst_kind_name(MirInstKind kind) -> const char*;
auto is_terminator(MirInstKind kind) -> bool;

} // namespace dao

#endif // DAO_IR_MIR_MIR_KIND_H
