#ifndef DAO_ANALYSIS_SEMANTIC_TOKENS_H
#define DAO_ANALYSIS_SEMANTIC_TOKENS_H

#include "frontend/ast/ast.h"
#include "frontend/diagnostics/source.h"
#include "frontend/lexer/lexer.h"
#include "frontend/resolve/resolve.h"

#include <string_view>
#include <vector>

namespace dao {

/// A single semantic token with its category from the frozen taxonomy
/// in CONTRACT_LANGUAGE_TOOLING.md.
struct SemanticToken {
  Span span;
  std::string_view kind; // e.g. "keyword.fn", "decl.function", "type.builtin"
};

/// Classify all tokens in a source file into semantic categories.
///
/// This combines lexical classification (keywords, literals, operators)
/// with structural classification from the AST (declarations, types,
/// parameters, mode/resource names). Structural classifications take
/// priority over lexical ones for the same span.
///
/// When a ResolveResult is provided, identifiers that resolved to
/// symbols gain resolve-driven classifications (use.variable.param,
/// use.variable.local, use.function, use.module, decl.module).
///
/// Each comment the lexer recorded is classified `comment`.
///
/// The returned vector is sorted by span offset.
auto classify_tokens(const LexResult& lexed,
                     const FileNode* file,
                     const ResolveResult* resolve_result = nullptr)
    -> std::vector<SemanticToken>;

} // namespace dao

#endif // DAO_ANALYSIS_SEMANTIC_TOKENS_H
