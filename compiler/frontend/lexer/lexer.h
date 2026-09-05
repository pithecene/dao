#ifndef DAO_FRONTEND_LEXER_LEXER_H
#define DAO_FRONTEND_LEXER_LEXER_H

#include "frontend/diagnostics/diagnostic.h"
#include "frontend/diagnostics/source.h"
#include "frontend/lexer/token.h"

#include <vector>

namespace dao {

struct LexResult {
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;
};

// Lex a source buffer. Every emitted span (tokens and diagnostics) is
// offset by `base_offset`, which places this buffer inside a
// program-wide offset space (CONTRACT_MODULE_SYSTEM.md; Task 31 §9).
// Token text views still point into `source`.
auto lex(const SourceBuffer& source, uint32_t base_offset = 0) -> LexResult;

} // namespace dao

#endif // DAO_FRONTEND_LEXER_LEXER_H
