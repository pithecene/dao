#include "driver/pipeline.h"

#include "analysis/semantic_tokens.h"
#include "frontend/ast/ast_printer.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frontend/resolve/resolve.h"
#include "backend/llvm/llvm_backend.h"
#include "ir/hir/hir_printer.h"
#include "ir/mir/mir_printer.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

/// lex/parse/ast run before module structure exists: one file, no set.
auto single_file(const dao::ProgramRequest& request) -> const std::filesystem::path& {
  if (!request.sources.empty()) {
    std::cerr << "error: this command takes a single file, not --source inputs\n";
    std::exit(EXIT_FAILURE);
  }
  return request.root;
}

// Debug-only token dump. Output format is not stable and must not be
// relied upon by tests, tooling, or documentation.
void cmd_lex(const dao::ProgramRequest& request) {
  const auto& path = single_file(request);
  auto contents = dao::read_file(path);
  dao::SourceBuffer source(path.generic_string(), std::move(contents));
  auto result = dao::lex(source);

  for (const auto& tok : result.tokens) {
    auto loc = source.line_col(tok.span.offset);
    std::cout << loc.line << ":" << loc.col << " " << dao::token_kind_name(tok.kind);
    if (!tok.text.empty() && tok.kind != dao::TokenKind::Newline &&
        tok.kind != dao::TokenKind::Eof) {
      if (tok.kind == dao::TokenKind::StringLiteral) {
        std::cout << " " << tok.text;
      } else {
        std::cout << " \"" << tok.text << "\"";
      }
    }
    std::cout << "\n";
  }

  if (dao::print_error_diagnostics(source.filename(), source, result.diagnostics)) {
    std::exit(EXIT_FAILURE);
  }
}

// Debug-only parse diagnostic dump. Output format is not stable.
void cmd_parse(const dao::ProgramRequest& request) {
  const auto& path = single_file(request);
  auto result = dao::lex_and_parse(path);
  if (result.parse_result.file != nullptr) {
    std::cout << "File: " << result.parse_result.file->imports.size() << " imports, "
              << result.parse_result.file->declarations.size() << " declarations\n";
  }
}

// Pretty-print AST. Output is deterministic and suitable for golden-file testing.
void cmd_ast(const dao::ProgramRequest& request) {
  const auto& path = single_file(request);
  auto result = dao::lex_and_parse(path);
  if (result.parse_result.file != nullptr) {
    dao::print_ast(std::cout, *result.parse_result.file);
  }
}

// Emit semantic token classification for the user file. Output is
// deterministic. Prelude files are separate program files and are not
// printed.
void cmd_tokens(const dao::ProgramRequest& request) {
  auto program = dao::load_program(request);

  // Run name resolution for resolve-driven classifications.
  auto resolve_result = dao::resolve(program);

  // Every user module; prelude tokens are not the user's concern.
  for (const auto* user : program.user_files()) {
    if (program.user_files().size() > 1) {
      std::cout << "== " << user->display_path << "\n";
    }
    auto sem_tokens = dao::classify_tokens(user->lex.tokens, user->file(), &resolve_result);
    for (const auto& tok : sem_tokens) {
      auto loc = program.source_map.locate(tok.span.offset);
      auto text = program.source_map.text(tok.span);
      std::cout << loc.line << ":" << loc.col << " " << tok.kind << " " << text << "\n";
    }
  }
}

// Run name resolution and print results for the user file.
void cmd_resolve(const dao::ProgramRequest& request) {
  auto program = dao::load_program(request);
  const auto& source_map = program.source_map;
  auto resolve_result = dao::resolve(program);

  // Print declared symbols (user files only).
  std::cout << "Symbols:\n";
  for (const auto& sym : resolve_result.context.symbols()) {
    // Builtins have no span; prelude declarations are not the user's.
    if (sym->decl_span.length == 0 || source_map.is_prelude(sym->decl_span.offset)) {
      continue;
    }
    std::cout << "  " << dao::symbol_kind_name(sym->kind) << " " << sym->name;
    if (sym->decl_span.length > 0) {
      auto decl_loc = source_map.locate(sym->decl_span.offset);
      std::cout << " [" << decl_loc.line << ":" << decl_loc.col << "]";
    }
    std::cout << "\n";
  }

  // Print uses in user files (resolved references).
  std::cout << "\nUses:\n";
  for (const auto& [offset, sym] : resolve_result.uses) {
    if (source_map.is_prelude(offset)) {
      continue;
    }
    auto loc = source_map.locate(offset);
    std::cout << "  " << loc.line << ":" << loc.col << " "
              << source_map.text(
                     dao::Span{.offset = offset,
                               .length = static_cast<uint32_t>(sym->name.size())})
              << " -> " << dao::symbol_kind_name(sym->kind) << " " << sym->name;
    if (sym->decl_span.length > 0) {
      auto decl_loc = source_map.locate(sym->decl_span.offset);
      std::cout << " [" << decl_loc.line << ":" << decl_loc.col << "]";
    }
    std::cout << "\n";
  }

  // Print diagnostics in user files (to stdout -- this is a debug dump command).
  bool has_user_diags = false;
  for (const auto& diag : resolve_result.diagnostics) {
    if (source_map.is_prelude(diag.span.offset)) {
      continue;
    }
    if (!has_user_diags) {
      std::cout << "\nDiagnostics:\n";
      has_user_diags = true;
    }
    auto loc = source_map.locate(diag.span.offset);
    std::cout << "  " << loc.file->display_path << ":" << loc.line << ":" << loc.col
              << ": error: " << diag.message << "\n";
  }
}

// Run type checking and print diagnostics.
void cmd_check(const dao::ProgramRequest& request) {
  dao::run_frontend(request);
  std::cout << "ok\n";
}

// Build and print HIR. Output is deterministic.
void cmd_hir(const dao::ProgramRequest& request) {
  auto result = dao::run_through_hir(request);
  if (result.hir.program != nullptr) {
    dao::print_hir(std::cout, *result.hir.program);
  }
}

// Build and print MIR. Output is deterministic.
void cmd_mir(const dao::ProgramRequest& request) {
  auto result = dao::run_through_mir(request);
  if (result.mir.module != nullptr) {
    dao::print_mir(std::cout, *result.mir.module);
  }
}

// Build and emit LLVM IR. Output is deterministic.
void cmd_llvm_ir(const dao::ProgramRequest& request) {
  // Initialize targets so the module gets a correct DataLayout
  // for ABI-sensitive lowering (struct coercion, alignment).
  dao::LlvmBackend::initialize_targets();

  auto mir = dao::run_through_mir(request);
  llvm::LLVMContext llvm_ctx;
  auto llvm_result = dao::lower_to_llvm(mir, llvm_ctx);
  dao::LlvmBackend::print_ir(std::cout, *llvm_result.module);
}

// Compile a .dao file to a native executable.
// Extra link inputs (object files, -l flags, -L flags) are forwarded
// to the system linker.
void cmd_build(const dao::ProgramRequest& request,
               std::span<const std::string> link_extras = {}) {
  const auto& path = request.primary_file();
  // Initialize targets before lowering so the module gets a correct
  // DataLayout for ABI-sensitive struct coercion.
  dao::LlvmBackend::initialize_targets();

  auto mir = dao::run_through_mir(request);
  llvm::LLVMContext llvm_ctx;
  auto llvm_result = dao::lower_to_llvm(mir, llvm_ctx);

  // A uniquely named object per build: concurrent builds of same-named
  // files (a test suite, two shells) must not overwrite each other's.
  llvm::SmallString<128> obj_buffer;
  if (auto ec = llvm::sys::fs::createTemporaryFile(path.stem().string(), "o", obj_buffer)) {
    std::cerr << "error: cannot create temporary object file: " << ec.message() << "\n";
    std::exit(EXIT_FAILURE);
  }
  std::filesystem::path obj_path(obj_buffer.str().str());
  std::string emit_error;
  if (!dao::LlvmBackend::emit_object(*llvm_result.module,
                                      obj_path.string(), emit_error)) {
    std::cerr << "error: " << emit_error << "\n";
    std::filesystem::remove(obj_path);
    std::exit(EXIT_FAILURE);
  }

  // Link with system cc: object + runtime library -> executable.
  auto output_path = path.parent_path() / path.stem();

  auto cc_path = llvm::sys::findProgramByName("cc");
  if (!cc_path) {
    std::cerr << "error: cannot find 'cc' linker: "
              << cc_path.getError().message() << "\n";
    std::filesystem::remove(obj_path);
    std::exit(EXIT_FAILURE);
  }

  // StringRef does not own data -- keep string temporaries alive.
  auto obj_str = obj_path.string();
  auto out_str = output_path.string();
  std::vector<llvm::StringRef> args = {
      *cc_path,
      obj_str,
      DAO_RUNTIME_LIB,
      "-o",
      out_str,
  };

  // Append extra link inputs (object files, -l flags, -L flags).
  for (const auto& extra : link_extras) {
    args.push_back(extra);
  }

  std::string link_error;
  int link_status = llvm::sys::ExecuteAndWait(
      *cc_path, args, /*Env=*/std::nullopt, /*Redirects=*/{},
      /*SecondsToWait=*/0, /*MemoryLimit=*/0, &link_error);
  std::filesystem::remove(obj_path);

  if (link_status != 0) {
    std::cerr << "error: linking failed";
    if (!link_error.empty()) {
      std::cerr << ": " << link_error;
    }
    std::cerr << "\n";
    std::exit(EXIT_FAILURE);
  }

  std::cout << output_path.string() << "\n";
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

using CommandFn = void (*)(const dao::ProgramRequest&);

struct Command {
  std::string_view name;
  CommandFn handler;
};

constexpr auto commands = std::array{
    Command{.name = "lex", .handler = cmd_lex},
    Command{.name = "parse", .handler = cmd_parse},
    Command{.name = "ast", .handler = cmd_ast},
    Command{.name = "tokens", .handler = cmd_tokens},
    Command{.name = "resolve", .handler = cmd_resolve},
    Command{.name = "check", .handler = cmd_check},
    Command{.name = "hir", .handler = cmd_hir},
    Command{.name = "mir", .handler = cmd_mir},
    Command{.name = "llvm-ir", .handler = cmd_llvm_ir},
};

void print_usage() {
  std::cerr << "usage: daoc <command> <root.dao> [--module-root DIR]... [--stdlib-root DIR]\n"
            << "       daoc <command> --source a.dao [--source b.dao]... [--entry a::b]\n"
            << "       daoc build <inputs as above> [link-inputs...]\n"
            << "commands: lex, parse, ast, tokens, resolve, check, hir, mir, llvm-ir, build\n";
}

auto require_file(const std::filesystem::path& path) -> void {
  if (!std::filesystem::exists(path)) {
    std::cerr << "error: file not found: " << path << "\n";
    std::exit(EXIT_FAILURE);
  }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
  if (argc < 2) {
    print_usage();
    return EXIT_FAILURE;
  }
  std::string_view command(argv[1]);

  // daoc <file> -- read and exit (Task 0 compat)
  const bool known_command =
      command == "build" || std::ranges::any_of(commands, [&](const Command& c) {
        return c.name == command;
      });
  if (!known_command) {
    std::filesystem::path path(command);
    if (argc == 2 && std::filesystem::exists(path)) {
      dao::read_file(path);
      return EXIT_SUCCESS;
    }
    print_usage();
    return EXIT_FAILURE;
  }

  dao::ProgramRequest request{
      .options = {.stdlib_root = std::filesystem::path(DAO_SOURCE_DIR) / "stdlib"}};
  std::vector<std::string> extras; // build: link inputs after the sources
  for (int i = 2; i < argc; ++i) {
    std::string_view arg(argv[i]);
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "error: " << arg << " needs a value\n";
        std::exit(EXIT_FAILURE);
      }
      return argv[++i];
    };
    if (arg == "--module-root") {
      request.options.module_roots.emplace_back(value());
    } else if (arg == "--stdlib-root") {
      request.options.stdlib_root = value();
    } else if (arg == "--source") {
      request.sources.emplace_back(value());
    } else if (arg == "--entry") {
      request.options.entry = value();
    } else if (arg.starts_with("--")) {
      std::cerr << "error: unknown option " << arg << "\n";
      return EXIT_FAILURE;
    } else if (request.root.empty() && extras.empty()) {
      request.root = arg;
    } else {
      extras.emplace_back(arg);
    }
  }

  if (request.root.empty() && request.sources.empty()) {
    print_usage();
    return EXIT_FAILURE;
  }
  // A root file and an explicit file set describe two different programs
  // (CONTRACT_MODULE_SYSTEM.md §8.2 vs §8.3); taking one and dropping the
  // other would compile something the command line did not ask for.
  if (!request.root.empty() && !request.sources.empty()) {
    std::cerr << "error: give a root file or --source inputs, not both: '" << request.root.string()
              << "' with --source " << request.sources.front().string() << "\n";
    return EXIT_FAILURE;
  }
  if (!request.root.empty()) {
    require_file(request.root);
  }
  for (const auto& source : request.sources) {
    require_file(source);
  }

  if (command == "build") {
    cmd_build(request, extras);
    return EXIT_SUCCESS;
  }
  if (!extras.empty()) {
    std::cerr << "error: unexpected argument: " << extras.front() << "\n";
    return EXIT_FAILURE;
  }
  for (const auto& [name, handler] : commands) {
    if (command == name) {
      handler(request);
      return EXIT_SUCCESS;
    }
  }
  return EXIT_FAILURE;
}
