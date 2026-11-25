#ifndef DAO_ANALYSIS_FORMATTER_H
#define DAO_ANALYSIS_FORMATTER_H

#include "frontend/diagnostics/diagnostic.h"
#include "frontend/diagnostics/source.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dao {

inline constexpr uint32_t kDefaultIndentWidth = 2;
inline constexpr uint32_t kDefaultMaxColumns = 100;

struct FormatOptions {
  uint32_t indent_width = kDefaultIndentWidth;
  uint32_t max_columns = kDefaultMaxColumns;
};

struct FormatResult {
  std::string output;
  std::vector<Diagnostic> errors;

  [[nodiscard]] auto ok() const -> bool {
    return errors.empty();
  }
};

/// Format a Dao source buffer into canonical form while preserving all `//`
/// comments and intentional single blank lines.
///
/// Fail-safe: if the buffer has lexical or syntax errors (or if internal
/// post-format AST/comment verification fails), returns those errors in
/// `FormatResult::errors` and leaves `output` empty so broken inputs are
/// never corrupted.
auto format_source(const SourceBuffer& source, const FormatOptions& options = {}) -> FormatResult;

/// Verify formatter invariants between `original` and `formatted`:
///   1. `formatted` lexes and parses cleanly.
///   2. `print_ast` and all supplementary AST fields (enum variant payloads,
///      match destructuring bindings, named call arguments, static receiver
///      type arguments) match `original`.
///   3. Every `//` comment from `original` is preserved in order.
///   4. Formatting `formatted` a second time produces identical bytes.
/// Returns an empty string when all invariants hold, or a description of the
/// first failure.
auto verify_format_invariants(const SourceBuffer& original,
                              std::string_view formatted,
                              const FormatOptions& options = {}) -> std::string;

} // namespace dao

#endif // DAO_ANALYSIS_FORMATTER_H
