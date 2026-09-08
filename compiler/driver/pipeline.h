#ifndef DAO_DRIVER_PIPELINE_H
#define DAO_DRIVER_PIPELINE_H

#include "frontend/lexer/lexer.h"
#include "frontend/module/program.h"
#include "frontend/module/source_map.h"
#include "frontend/parser/parser.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"
#include "ir/hir/hir_builder.h"
#include "ir/hir/hir_context.h"
#include "ir/mir/mir_builder.h"
#include "ir/mir/mir_context.h"
#include "backend/llvm/llvm_backend.h"

#include <llvm/IR/LLVMContext.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// Diagnostic helpers
// ---------------------------------------------------------------------------

// Print all diagnostics as errors, located through the source map as
// <file>:<line>:<col>. Returns true if any were printed.
auto print_error_diagnostics(const SourceMap& source_map,
                             std::span<const Diagnostic> diags) -> bool;

// Same, for a standalone buffer outside any program (lex/parse/ast dumps).
auto print_error_diagnostics(std::string_view filename, const SourceBuffer& source,
                             std::span<const Diagnostic> diags) -> bool;

// Print diagnostics with severity labels. Returns true if any errors.
auto print_diagnostics(const SourceMap& source_map,
                       std::span<const Diagnostic> diags) -> bool;

// ---------------------------------------------------------------------------
// Pipeline result structs
// ---------------------------------------------------------------------------

struct LexedFile {
  SourceBuffer source;
  LexResult lex_result;
};

struct ParsedFile {
  SourceBuffer source;
  LexResult lex_result;
  ParseResult parse_result;
};

struct FrontendResult {
  Program program;
  ResolveResult resolve;
  TypeContext types;
  TypeCheckResult typecheck;
};

struct HirResult {
  FrontendResult frontend;
  HirContext hir_ctx;
  HirBuildResult hir;
};

struct MirResult {
  HirResult hir_result;
  MirContext mir_ctx;
  MirBuildResult mir;
};

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

auto read_file(const std::filesystem::path& path) -> std::string;

/// Diagnostics in the order a reader walks the program: by offset,
/// which is by file and then by position within it
/// (CONTRACT_MODULE_SYSTEM.md §8.4).  Phases produce them in their own
/// order — resolution and lowering walk topological order, which is not
/// file order — so anything that prints them puts them in this order
/// first.  The printers below do it; a command that formats its own
/// output calls this.
auto in_program_order(std::span<const Diagnostic> diags) -> std::vector<Diagnostic>;

// ---------------------------------------------------------------------------
// Pipeline stage functions
// ---------------------------------------------------------------------------

// Single-file stages (no prelude): used by the lex/parse/ast dumps.
auto lex_file(const std::filesystem::path& path) -> LexedFile;
auto lex_and_parse(const std::filesystem::path& path) -> ParsedFile;

// What to compile: a root file whose imports drive discovery, or an
// explicit file set (`--source`, discovery off).  The prelude group is
// loaded from `options.stdlib_root` in both modes.
struct ProgramRequest {
  std::filesystem::path root;                 // root-file mode; empty in explicit mode
  std::vector<std::filesystem::path> sources; // explicit file-list mode
  ProgramOptions options;

  /// The file that names build outputs: the root, else the source whose
  /// path sorts first.  Never simply the first source — the same set in
  /// another order would then produce a differently named executable,
  /// and §8.4 makes output a function of the set, not of the order.
  /// The entry module names the program, but the file that carries it
  /// is not known until the graph is built; the smallest path is a
  /// stable stand-in that no permutation changes.
  [[nodiscard]] auto primary_file() const -> std::filesystem::path {
    if (!root.empty() || sources.empty()) {
      return root;
    }
    // The file each path NAMES, not the spelling it was given: `./b.dao`,
    // `b.dao`, and a symlink to it are one source, and the explicit
    // loader already deduplicates them.  Ranking the spellings would
    // leave the winner among equals to input order — and a symlink alias
    // has its own stem, so the same program would be built under two
    // different names (§8.4).
    std::vector<std::filesystem::path> named;
    named.reserve(sources.size());
    for (const auto& source : sources) {
      named.push_back(canonical_or_self(source));
    }
    return *std::ranges::min_element(named);
  }
};

// Program stages: the prelude group plus the requested files, lexed and
// parsed into one program-wide offset space with the module graph
// built.  Exits the process on load, graph, lex, or parse errors after
// printing them.
auto load_program(const ProgramRequest& request) -> Program;
auto run_frontend(const ProgramRequest& request) -> FrontendResult;
auto run_through_hir(const ProgramRequest& request) -> HirResult;
auto run_through_mir(const ProgramRequest& request) -> MirResult;
auto lower_to_llvm(const MirResult& mir, llvm::LLVMContext& llvm_ctx) -> LlvmBackendResult;

} // namespace dao

#endif // DAO_DRIVER_PIPELINE_H
