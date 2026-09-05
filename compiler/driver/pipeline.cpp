#include "driver/pipeline.h"

#include "ir/hir/hir_builder.h"
#include "ir/mir/mir_builder.h"
#include "ir/mir/mir_monomorphize.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path);
  if (!file) {
    std::cerr << "error: could not open: " << path << "\n";
    std::exit(EXIT_FAILURE);
  }
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// ---------------------------------------------------------------------------
// Diagnostic helpers
// ---------------------------------------------------------------------------

namespace {

void print_location(const SourceMap& source_map, Span span) {
  const auto* file = source_map.file_for(span.offset);
  if (file == nullptr) {
    std::cerr << "<program>";
    return;
  }
  auto loc = source_map.locate(span.offset);
  std::cerr << file->display_path << ":" << loc.line << ":" << loc.col;
}

} // namespace

auto print_error_diagnostics(const SourceMap& source_map,
                             std::span<const Diagnostic> diags) -> bool {
  for (const auto& diag : diags) {
    print_location(source_map, diag.span);
    std::cerr << ": error: " << diag.message << "\n";
  }
  return !diags.empty();
}

auto print_error_diagnostics(std::string_view filename, const SourceBuffer& source,
                             std::span<const Diagnostic> diags) -> bool {
  for (const auto& diag : diags) {
    auto loc = source.line_col(diag.span.offset);
    std::cerr << filename << ":" << loc.line << ":" << loc.col << ": error: " << diag.message
              << "\n";
  }
  return !diags.empty();
}

auto print_diagnostics(const SourceMap& source_map,
                       std::span<const Diagnostic> diags) -> bool {
  bool has_errors = false;
  for (const auto& diag : diags) {
    const auto* severity = diag.severity == Severity::Error ? "error" : "warning";
    print_location(source_map, diag.span);
    std::cerr << ": " << severity << ": " << diag.message << "\n";
    if (diag.severity == Severity::Error) {
      has_errors = true;
    }
  }
  return has_errors;
}

// ---------------------------------------------------------------------------
// Single-file stages
// ---------------------------------------------------------------------------

auto lex_file(const std::filesystem::path& path) -> LexedFile {
  auto contents = read_file(path);
  SourceBuffer source(path.generic_string(), std::move(contents));
  auto lex_result = lex(source);

  if (print_error_diagnostics(source.filename(), source, lex_result.diagnostics)) {
    std::exit(EXIT_FAILURE);
  }

  return {.source = std::move(source), .lex_result = std::move(lex_result)};
}

auto lex_and_parse(const std::filesystem::path& path) -> ParsedFile {
  auto lexed = lex_file(path);
  auto parse_result = parse(lexed.lex_result.tokens);

  if (print_error_diagnostics(lexed.source.filename(), lexed.source, parse_result.diagnostics)) {
    std::exit(EXIT_FAILURE);
  }

  return {.source = std::move(lexed.source),
          .lex_result = std::move(lexed.lex_result),
          .parse_result = std::move(parse_result)};
}

// ---------------------------------------------------------------------------
// Program stages
// ---------------------------------------------------------------------------

auto load_program(const ProgramRequest& request) -> Program {
  auto program = request.sources.empty()
                     ? load_program_from_root(request.root, request.options)
                     : load_program_from_files(request.sources, request.options);

  // Load and graph diagnostics: located ones (imports, module
  // declarations) print through the source map; the rest (position
  // budget, entry selection) have no location.
  bool has_errors = false;
  std::vector<Diagnostic> located;
  for (const auto& diag : program.diagnostics) {
    if (diag.span.length == 0) {
      std::cerr << "error: " << diag.message << "\n";
      has_errors = true;
    } else {
      located.push_back(diag);
    }
  }
  has_errors |= print_error_diagnostics(program.source_map, located);

  for (const auto& file : program.files) {
    has_errors |= print_error_diagnostics(program.source_map, file->lex.diagnostics);
    has_errors |= print_error_diagnostics(program.source_map, file->parse.diagnostics);
  }
  if (has_errors || !program.lexed_and_parsed_cleanly()) {
    std::exit(EXIT_FAILURE);
  }
  return program;
}

auto run_frontend(const ProgramRequest& request) -> FrontendResult {
  auto program = load_program(request);

  auto resolve_result = resolve(program);
  bool has_errors =
      print_error_diagnostics(program.source_map, resolve_result.diagnostics);

  TypeContext types;
  auto check_result = typecheck(program, resolve_result, types);
  has_errors |= print_diagnostics(program.source_map, check_result.diagnostics);

  if (has_errors) {
    std::exit(EXIT_FAILURE);
  }

  return {.program = std::move(program),
          .resolve = std::move(resolve_result),
          .types = std::move(types),
          .typecheck = std::move(check_result)};
}

auto run_through_hir(const ProgramRequest& request) -> HirResult {
  auto frontend = run_frontend(request);
  HirContext hir_ctx;
  auto hir = build_hir(frontend.program, frontend.resolve, frontend.typecheck, hir_ctx);

  bool has_errors = print_error_diagnostics(frontend.program.source_map, hir.diagnostics);
  if (hir.module == nullptr || has_errors) {
    std::exit(EXIT_FAILURE);
  }

  return {.frontend = std::move(frontend),
          .hir_ctx = std::move(hir_ctx),
          .hir = std::move(hir)};
}

auto run_through_mir(const ProgramRequest& request) -> MirResult {
  auto hir_result = run_through_hir(request);
  const auto& source_map = hir_result.frontend.program.source_map;
  MirContext mir_ctx;
  auto mir = build_mir(*hir_result.hir.module, mir_ctx, hir_result.frontend.types);

  bool has_errors = print_error_diagnostics(source_map, mir.diagnostics);
  if (mir.module == nullptr || has_errors) {
    std::exit(EXIT_FAILURE);
  }

  auto mono_result =
      monomorphize(*mir.module, mir_ctx, hir_result.frontend.types, mir.generic_templates);
  if (!mono_result.diagnostics.empty()) {
    bool mono_errors = print_error_diagnostics(source_map, mono_result.diagnostics);
    // Monomorphization emits errors for MIR concreteness invariant
    // violations (Task 28 §14.2).  These must halt the pipeline
    // before LLVM lowering — allowing generic residue through would
    // surface as an opaque LLVM DataLayout assertion on unsized
    // types, obscuring the root cause.
    if (mono_errors) {
      std::exit(EXIT_FAILURE);
    }
  }

  return {.hir_result = std::move(hir_result),
          .mir_ctx = std::move(mir_ctx),
          .mir = std::move(mir)};
}

auto lower_to_llvm(const MirResult& mir, llvm::LLVMContext& llvm_ctx) -> LlvmBackendResult {
  const auto& source_map = mir.hir_result.frontend.program.source_map;
  LlvmBackend backend(llvm_ctx);
  auto result = backend.lower(*mir.mir.module, &source_map);

  // Prelude warnings (dropped bodies of prelude functions that use
  // unsupported constructs) are not the user's concern.
  std::vector<Diagnostic> user_diags;
  for (const auto& diag : result.diagnostics) {
    if (diag.severity == Severity::Warning && source_map.is_prelude(diag.span.offset)) {
      continue;
    }
    user_diags.push_back(diag);
  }

  bool has_errors = print_diagnostics(source_map, user_diags);
  if (result.module == nullptr || has_errors) {
    std::exit(EXIT_FAILURE);
  }

  return result;
}

} // namespace dao
