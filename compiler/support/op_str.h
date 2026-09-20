#ifndef DAO_SUPPORT_OP_STR_H
#define DAO_SUPPORT_OP_STR_H

#include "frontend/ast/ast.h"

namespace dao {

/// Convert a BinaryOp enum to its source-level string representation.
inline auto binary_op_str(BinaryOp op) -> const char* {
  switch (op) {
  case BinaryOp::Add:    return "+";
  case BinaryOp::Sub:    return "-";
  case BinaryOp::Mul:    return "*";
  case BinaryOp::Div:    return "/";
  case BinaryOp::Mod:    return "%";
  case BinaryOp::EqEq:   return "==";
  case BinaryOp::BangEq: return "!=";
  case BinaryOp::Lt:     return "<";
  case BinaryOp::LtEq:   return "<=";
  case BinaryOp::Gt:     return ">";
  case BinaryOp::GtEq:   return ">=";
  case BinaryOp::And:    return "and";
  case BinaryOp::Or:     return "or";
  }
  return "?";
}

/// Convert a UnaryOp enum to its source-level string representation.
inline auto unary_op_str(UnaryOp op) -> const char* {
  switch (op) {
  case UnaryOp::Negate: return "-";
  case UnaryOp::Not:    return "!";
  }
  return "?";
}

} // namespace dao

#endif // DAO_SUPPORT_OP_STR_H
