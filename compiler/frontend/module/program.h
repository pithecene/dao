#ifndef DAO_FRONTEND_MODULE_PROGRAM_H
#define DAO_FRONTEND_MODULE_PROGRAM_H

#include "frontend/ast/ast.h"
#include "frontend/diagnostics/diagnostic.h"
#include "frontend/module/source_map.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// Program — the set of source files compiled together, their modules,
// and the import graph over them (CONTRACT_MODULE_SYSTEM.md §2, §3, §8,
// §9).
//
// Files are lexed and parsed into one program-wide offset space
// (source_map.h) with the prelude group first and each group in lexical
// order of display path, so file ids, diagnostics order, and every
// downstream output are independent of the order inputs were supplied
// in.  Each file's `module` declaration is
// its identity; imports are edges; modules are ordered so that every
// module follows the modules it imports.  Per-module scopes and
// cross-module resolution are later slices: the passes still declare
// every file into one shared scope.
// ---------------------------------------------------------------------------

struct SourceInput {
  std::string display_path; // shown in diagnostics; a real path or e.g. "<playground>"
  std::string text;
  bool is_prelude = false;
};

struct ModuleInfo {
  // The module's identity is its display name: `module_display` builds it
  // from the declaration's segments, and every consumer — the graph, the
  // resolver, diagnostics, symbol mangling — asks for it by that name.  An
  // id and a second copy of the segments had no reader.
  std::string display; // "a::b::c"
  SourceFile* file = nullptr;
  bool is_prelude = false;
  bool declares_main = false;       // a top-level `fn main`
  std::vector<ModuleInfo*> imports; // resolved edges in declaration order, duplicates removed
};

/// Options common to the loaders that read the filesystem.
struct ProgramOptions {
  std::filesystem::path stdlib_root;               // prelude group source; empty loads no prelude
  std::vector<std::filesystem::path> module_roots; // root-file mode: searched after the root's directory
  std::optional<std::string> entry;                // explicit-set mode: entry module by display name
};

struct Program {
  std::vector<std::unique_ptr<SourceFile>> files;   // prelude group, then lexical by display path
  std::vector<std::unique_ptr<ModuleInfo>> modules; // the order their files appear in `files`
  std::vector<ModuleInfo*> topo_order;              // imported modules before importers
  ModuleInfo* entry = nullptr;                      // §7.7; null when no rule selects one
  SourceMap source_map;
  std::vector<Diagnostic> diagnostics; // load level (position budget) and graph level (§8.5)

  Program() = default;
  Program(const Program&) = delete;
  auto operator=(const Program&) -> Program& = delete;
  Program(Program&&) noexcept = default;
  auto operator=(Program&&) noexcept -> Program& = default;
  ~Program() = default;

  /// Parsed roots of every file that produced one, in `files` order.
  [[nodiscard]] auto file_nodes() const -> std::vector<const FileNode*>;

  /// The non-prelude files, in `files` order.
  [[nodiscard]] auto user_files() const -> std::vector<const SourceFile*>;

  /// True when no file produced a lex or parse diagnostic.
  [[nodiscard]] auto lexed_and_parsed_cleanly() const -> bool;

  /// The module with this display identity, or null.
  [[nodiscard]] auto module_named(std::string_view display) const -> ModuleInfo*;
};

/// How much a program cares that no module declares `fn main`.
/// A delivered file set must have an entry (CONTRACT_MODULE_SYSTEM.md
/// §8.3); an editor buffer wants to be told why it cannot be run; a
/// fragment lowered for a test or for tooling does not care.
enum class EntryPolicy : std::uint8_t {
  Optional, // a fragment: no diagnostic
  Advisory, // an editor buffer: a warning, which does not stop analysis
  Required, // an explicit file set: an error
};

/// In-memory mode (§8.1): lex, parse, and build the module graph over
/// exactly these inputs; imports of modules outside the set are
/// diagnosed, never searched.  The entry module is `entry` when given,
/// else the unique non-prelude module declaring `fn main` (§7.7).  Does
/// not read the filesystem.
auto build_program(std::vector<SourceInput> inputs,
                   std::optional<std::string> entry = {},
                   EntryPolicy entry_policy = EntryPolicy::Optional) -> Program;

/// Root-file mode (§8.1–§8.3): the prelude group, the root file, and
/// every module reachable from it by imports.  `import a::b::c` is
/// satisfied by the first `<root>/a/b/c.dao` over the root file's
/// directory, `options.module_roots`, and the stdlib root; the located
/// file must declare `a::b::c`.  The entry module is the root's.
auto load_program_from_root(const std::filesystem::path& root_file, const ProgramOptions& options)
    -> Program;

/// Explicit file-list mode (§8.1): the prelude group plus exactly these
/// files; discovery is off.  Entry per `options.entry` or the unique
/// `fn main`.
auto load_program_from_files(const std::vector<std::filesystem::path>& files,
                             const ProgramOptions& options) -> Program;

/// Read a file into a SourceInput.  The display path is the path as
/// given, or — when `display_root` is non-empty and contains it — the
/// path relative to that root, so diagnostics stay unambiguous across
/// files and stable across machines.  Exits the process with a message
/// if the file cannot be opened, matching the driver's historical
/// behaviour.
auto read_source_input(const std::filesystem::path& path, bool is_prelude,
                       const std::filesystem::path& display_root = {}) -> SourceInput;

/// The prelude group's files (CONTRACT_MODULE_SYSTEM.md §7): every .dao
/// file under <stdlib_root>/core then <stdlib_root>/io, each directory
/// in sorted path order.  Missing directories are skipped.
auto prelude_files(const std::filesystem::path& stdlib_root) -> std::vector<std::filesystem::path>;

/// The prelude group as inputs, displayed relative to the stdlib root's
/// parent (e.g. `stdlib/core/vector.dao`).
auto load_prelude_inputs(const std::filesystem::path& stdlib_root) -> std::vector<SourceInput>;

} // namespace dao

#endif // DAO_FRONTEND_MODULE_PROGRAM_H
