#ifndef DAO_ANALYSIS_DOCUMENT_SYMBOLS_H
#define DAO_ANALYSIS_DOCUMENT_SYMBOLS_H

#include "frontend/ast/ast.h"
#include "frontend/diagnostics/source.h"

#include <string>
#include <string_view>
#include <vector>

namespace dao {

struct DocumentSymbol {
  std::string name;
  std::string kind; // "function", "class", "field", "concept", "alias"
  Span span;        // full declaration span
  Span name_span;   // name identifier span
  std::vector<DocumentSymbol> children;
};

/// Collect document symbols from the AST.
/// Returns a hierarchical tree of declarations with their children
/// (e.g. class fields, concept methods).
/// Symbols declared in one file.  Callers pass the user's file; prelude
/// files are separate FileNodes and are simply not queried.
auto query_document_symbols(const FileNode& file) -> std::vector<DocumentSymbol>;

} // namespace dao

#endif // DAO_ANALYSIS_DOCUMENT_SYMBOLS_H
