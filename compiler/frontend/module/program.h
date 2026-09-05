#ifndef DAO_FRONTEND_MODULE_PROGRAM_H
#define DAO_FRONTEND_MODULE_PROGRAM_H

#include "frontend/ast/ast.h"
#include "frontend/diagnostics/diagnostic.h"
#include "frontend/module/source_map.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// Program — the set of source files compiled together.
//
// Files are lexed and parsed into one program-wide offset space
// (source_map.h).  Module identity, the import graph, and per-module
// scopes are not implemented yet: every file is declared into one
// shared scope, which is what the former concatenated buffer gave the
// passes, so behaviour is unchanged.
// ---------------------------------------------------------------------------

struct SourceInput {
  std::string display_path; // shown in diagnostics; a real path or e.g. "<playground>"
  std::string text;
  bool is_prelude = false;
};

struct Program {
  std::vector<std::unique_ptr<SourceFile>> files; // load order; prelude group first
  SourceMap source_map;
  std::vector<Diagnostic> diagnostics; // load-level: position budget

  Program() = default;
  Program(const Program&) = delete;
  auto operator=(const Program&) -> Program& = delete;
  Program(Program&&) noexcept = default;
  auto operator=(Program&&) noexcept -> Program& = default;
  ~Program() = default;

  /// Parsed roots of every file that produced one, in load order.
  [[nodiscard]] auto file_nodes() const -> std::vector<const FileNode*>;

  /// The non-prelude files, in load order.
  [[nodiscard]] auto user_files() const -> std::vector<const SourceFile*>;

  /// True when no file produced a lex or parse diagnostic.
  [[nodiscard]] auto lexed_and_parsed_cleanly() const -> bool;
};

/// Lex and parse every input into a Program.  Inputs are placed in the
/// offset space in the given order; the position budget is checked
/// before any base offset is assigned (position_budget_fits).  Does not
/// read the filesystem.
auto build_program(std::vector<SourceInput> inputs) -> Program;

/// Read a file into a SourceInput.  The display path is the path as
/// given, or — when `display_root` is non-empty and contains it — the
/// path relative to that root, so diagnostics stay unambiguous across
/// files and stable across machines.  Exits the process with a message
/// if the file cannot be opened, matching the driver's historical
/// behaviour.
auto read_source_input(const std::filesystem::path& path, bool is_prelude,
                       const std::filesystem::path& display_root = {}) -> SourceInput;

/// The prelude group (CONTRACT_MODULE_SYSTEM.md §7): every .dao file
/// under <stdlib_root>/core then <stdlib_root>/io, each directory in
/// sorted path order, displayed relative to the stdlib root's parent
/// (e.g. `stdlib/core/vector.dao`).  Missing directories are skipped.
auto load_prelude_inputs(const std::filesystem::path& stdlib_root) -> std::vector<SourceInput>;

} // namespace dao

#endif // DAO_FRONTEND_MODULE_PROGRAM_H
