#ifndef DAO_PLAYGROUND_TOKEN_CATEGORY_H
#define DAO_PLAYGROUND_TOKEN_CATEGORY_H

#include "frontend/lexer/token.h"

#include <string_view>

namespace dao::playground {

/// Map a TokenKind to a structural highlight category.
/// This is a lexical classification only — semantic classification
/// comes in Task 5.
inline auto token_category(TokenKind kind) -> std::string_view {
  switch (kind) {
  // Keywords
  case TokenKind::KwModule:
  case TokenKind::KwImport:
  case TokenKind::KwExtern:
  case TokenKind::KwFn:
  case TokenKind::KwClass:
  case TokenKind::KwEnum:
  case TokenKind::KwType:
  case TokenKind::KwLet:
  case TokenKind::KwIf:
  case TokenKind::KwElse:
  case TokenKind::KwWhile:
  case TokenKind::KwFor:
  case TokenKind::KwIn:
  case TokenKind::KwMatch:
  case TokenKind::KwBreak:
  case TokenKind::KwReturn:
  case TokenKind::KwYield:
  case TokenKind::KwMode:
  case TokenKind::KwResource:
  case TokenKind::KwAnd:
  case TokenKind::KwOr:
  case TokenKind::KwConcept:
  case TokenKind::KwDerived:
  case TokenKind::KwAs:
  case TokenKind::KwExtend:
  case TokenKind::KwDeny:
  case TokenKind::KwSelf:
  case TokenKind::KwWhere:
    return "keyword";

  // Keyword literals
  case TokenKind::KwTrue:
  case TokenKind::KwFalse:
    return "literal.bool";

  // Numeric literals
  case TokenKind::IntLiteral:
  case TokenKind::FloatLiteral:
    return "literal.number";

  // String literals
  case TokenKind::StringLiteral:
    return "literal.string";

  // Operators
  case TokenKind::Colon:
  case TokenKind::ColonColon:
  case TokenKind::Arrow:
  case TokenKind::FatArrow:
  case TokenKind::Eq:
  case TokenKind::EqEq:
  case TokenKind::BangEq:
  case TokenKind::Lt:
  case TokenKind::LtEq:
  case TokenKind::Gt:
  case TokenKind::GtEq:
  case TokenKind::Plus:
  case TokenKind::Minus:
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Percent:
  case TokenKind::Amp:
  case TokenKind::Bang:
  case TokenKind::Dot:
  case TokenKind::DotDot:
  case TokenKind::Comma:
  case TokenKind::Pipe:
  case TokenKind::PipeGt:
  case TokenKind::Question:
    return "operator";

  // Delimiters
  case TokenKind::LParen:
  case TokenKind::RParen:
  case TokenKind::LBracket:
  case TokenKind::RBracket:
    return "punctuation";

  // Identifier
  case TokenKind::Identifier:
    return "identifier";

  // Synthetic
  case TokenKind::Newline:
  case TokenKind::Indent:
  case TokenKind::Dedent:
  case TokenKind::Eof:
    return "synthetic";

  // Error
  case TokenKind::Error:
    return "error";
  }
  return "unknown";
}

/// Returns true for synthetic tokens that should be filtered from
/// the response (not useful for highlighting).
inline auto is_synthetic_token(TokenKind kind) -> bool {
  return kind == TokenKind::Newline || kind == TokenKind::Indent || kind == TokenKind::Dedent ||
         kind == TokenKind::Eof;
}

} // namespace dao::playground

#endif // DAO_PLAYGROUND_TOKEN_CATEGORY_H
