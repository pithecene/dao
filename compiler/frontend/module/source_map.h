#ifndef DAO_FRONTEND_MODULE_SOURCE_MAP_H
#define DAO_FRONTEND_MODULE_SOURCE_MAP_H

#include "frontend/diagnostics/source.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// Program-wide offset space (CONTRACT_MODULE_SYSTEM.md).
//
// Every source file of a program occupies a closed range of positions
// [base_offset, base_offset + size] in one uint32_t offset space: its
// bytes, plus one reserved position for its end-of-file, where the
// lexer emits its zero-length Eof token and where unexpected-end
// diagnostics land.  The next file begins immediately after that slot
// (base_{n+1} = base_n + size_n + 1), so a file's EOF is never another
// file's first byte and an empty file still owns one position.
//
// Span stays {offset, length}; nothing downstream needs a file id.  A
// SourceMap answers "which file, which line, which column" for any
// program-wide offset.
// ---------------------------------------------------------------------------

struct ModuleInfo;

struct SourceFile {
  uint32_t file_id = 0;
  std::string display_path;  // shown in diagnostics
  SourceBuffer buffer;
  uint32_t base_offset = 0;
  bool is_prelude = false;   // member of the prelude group (stdlib/core, stdlib/io)
  LexResult lex;
  ParseResult parse;
  ModuleInfo* module = nullptr; // set by the module graph; null when the file declares no module

  [[nodiscard]] auto file() const -> const FileNode* { return parse.file; }
  [[nodiscard]] auto eof_offset() const -> uint32_t {
    return base_offset + buffer.size();
  }
  [[nodiscard]] auto contains(uint32_t offset) const -> bool {
    return offset >= base_offset && offset <= eof_offset();
  }
  /// True if the whole span lies within this file.  The span's end is
  /// computed in 64 bits: offsets run up to 2^32 − 1, so a uint32_t
  /// `offset + length` could wrap and admit an out-of-file span.
  [[nodiscard]] auto contains_span(Span span) const -> bool {
    const uint64_t end = uint64_t{span.offset} + span.length;
    return span.offset >= base_offset && end <= eof_offset();
  }
  /// Convert a program-wide offset inside this file to a buffer-local one.
  [[nodiscard]] auto local_offset(uint32_t offset) const -> uint32_t {
    return offset - base_offset;
  }
};

struct SourceLocation {
  const SourceFile* file = nullptr;
  uint32_t line = 1;
  uint32_t col = 1;
};

// Maximum number of positions a program may occupy: every file consumes
// size + 1 (its bytes plus its EOF position), and the last position used
// must fit in uint32_t.
inline constexpr uint64_t kMaxProgramPositions = uint64_t{1} << 32;

/// Sum of (size + 1) over the files, or nullopt-equivalent false when the
/// program would not fit the offset space.  Pure; checked before any base
/// offset is assigned so no span is ever created outside the space.
[[nodiscard]] auto position_budget_fits(std::span<const uint64_t> sizes) -> bool;

/// Base offset of file n given the sizes of files 0..n-1.  Requires
/// position_budget_fits over all files to have passed.
[[nodiscard]] auto base_offset_for(std::span<const uint64_t> preceding_sizes) -> uint32_t;

class SourceMap {
public:
  /// Register a file.  Files must be added in ascending base_offset
  /// order with disjoint closed ranges (as produced by Program).
  void add(const SourceFile* file);

  [[nodiscard]] auto files() const -> std::span<const SourceFile* const> {
    return files_;
  }

  /// The file owning `offset`, or nullptr when the offset lies past the
  /// last file's EOF position.
  [[nodiscard]] auto file_for(uint32_t offset) const -> const SourceFile*;

  /// Line/column of `offset` in its owning file.  For a file's EOF
  /// position this is the last line, one column past the last character
  /// (1:1 for an empty file).  Asserts if no file owns the offset.
  [[nodiscard]] auto locate(uint32_t offset) const -> SourceLocation;

  /// The file that wholly contains `span`, or nullptr if the span starts
  /// in no file or runs past its file's EOF (overflow-safe).
  [[nodiscard]] auto owner_of(Span span) const -> const SourceFile*;

  /// Text covered by a span.  Spans produced by the lexer and parser
  /// always lie within one file; a span with no owner (see owner_of) is
  /// an internal error and asserts rather than returning empty or
  /// truncated text.
  [[nodiscard]] auto text(Span span) const -> std::string_view;

  /// True if `offset` lies in a prelude-group file.
  [[nodiscard]] auto is_prelude(uint32_t offset) const -> bool;

private:
  std::vector<const SourceFile*> files_; // sorted by base_offset
};

} // namespace dao

#endif // DAO_FRONTEND_MODULE_SOURCE_MAP_H
