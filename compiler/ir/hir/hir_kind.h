#ifndef DAO_IR_HIR_HIR_KIND_H
#define DAO_IR_HIR_HIR_KIND_H

#include <cstdint>

namespace dao {

// ---------------------------------------------------------------------------
// HIR node kind tags.
// ---------------------------------------------------------------------------

enum class HirKind : std::uint8_t {
  // Module
  Module,

  // Declarations
  Function,
  ClassDecl,

  // Statements
  Let,
  Assign,
  If,
  While,
  For,
  Return,
  Yield,
  Break,
  ExprStmt,
  Mode,
  Resource,

  // Expressions
  IntLiteral,
  FloatLiteral,
  StringLiteral,
  BoolLiteral,
  SymbolRef,
  Unary,
  Binary,
  Call,
  Construct,
  EnumConstruct,
  EnumDiscriminant,
  EnumPayload,
  Field,
  Index,
  Pipe,
  Try,
  Lambda,
  PtrOp,
};

auto hir_kind_name(HirKind kind) -> const char*;

} // namespace dao

#endif // DAO_IR_HIR_HIR_KIND_H
