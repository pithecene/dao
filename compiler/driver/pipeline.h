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

// ---------------------------------------------------------------------------
// Pipeline stage functions
// ---------------------------------------------------------------------------

// Single-file stages (no prelude): used by the lex/parse/ast dumps.
auto lex_file(const std::filesystem::path& path) -> LexedFile;
auto lex_and_parse(const std::filesystem::path& path) -> ParsedFile;

// Program stages: the prelude group (stdlib/core, stdlib/io) plus the
// user file, lexed and parsed into one program-wide offset space.
// Exits the process on lex/parse errors after printing them.
auto load_program(const std::filesystem::path& user_path) -> Program;
auto run_frontend(const std::filesystem::path& path) -> FrontendResult;
auto run_through_hir(const std::filesystem::path& path) -> HirResult;
auto run_through_mir(const std::filesystem::path& path) -> MirResult;
auto lower_to_llvm(const MirResult& mir, llvm::LLVMContext& llvm_ctx) -> LlvmBackendResult;

} // namespace dao

#endif // DAO_DRIVER_PIPELINE_H
