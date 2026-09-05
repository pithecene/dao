// NOLINTBEGIN(readability-magic-numbers)
#include "pipeline.h"
#include "run.h"

#include "backend/llvm/llvm_backend.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"
#include "ir/hir/hir_builder.h"
#include "ir/hir/hir_context.h"
#include "ir/mir/mir_builder.h"
#include "ir/mir/mir_context.h"
#include "ir/mir/mir_monomorphize.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/Program.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace dao::playground {

namespace {

// Read entire file into string, for capturing process output.
auto slurp(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}

void respond_failure(httplib::Response& res, const nlohmann::json& diagnostics) {
  nlohmann::json response = {
      {"stdout", ""},
      {"stderr", ""},
      {"exit_code", -1},
      {"diagnostics", diagnostics},
  };
  res.set_content(response.dump(), "application/json");
}

auto has_error_severity(const std::vector<Diagnostic>& diags) -> bool {
  return std::ranges::any_of(
      diags, [](const auto& diag) -> bool { return diag.severity == Severity::Error; });
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void init_run_support() { LlvmBackend::initialize_targets(); }

void handle_run(const httplib::Request& req, httplib::Response& res,
                const std::filesystem::path& repo_root) {
  nlohmann::json request;
  try {
    request = nlohmann::json::parse(req.body);
  } catch (const nlohmann::json::parse_error&) {
    res.status = 400;
    res.set_content(R"({"error":"invalid JSON"})", "application/json");
    return;
  }

  if (!request.contains("source") || !request["source"].is_string()) {
    res.status = 400;
    res.set_content(R"({"error":"missing 'source' field"})",
                    "application/json");
    return;
  }

  nlohmann::json diagnostics = nlohmann::json::array();

  auto prog = build_playground_program(repo_root, request["source"].get<std::string>());
  if (prog.user == nullptr || !prog.program.diagnostics.empty()) {
    for (const auto& diag : prog.program.diagnostics) {
      if (diag.span.length == 0) {
        diagnostics.push_back(make_internal_error(diag.message));
      }
    }
    collect_diagnostics(diagnostics, prog, prog.program.diagnostics);
    respond_failure(res, diagnostics);
    return;
  }

  // Lex/parse diagnostics of every file; only the editor buffer's are
  // reported, but any file failing to parse stops the run.
  bool lex_parse_failed = false;
  for (const auto& file : prog.program.files) {
    collect_diagnostics(diagnostics, prog, file->lex.diagnostics);
    collect_diagnostics(diagnostics, prog, file->parse.diagnostics);
    lex_parse_failed |= !file->lex.diagnostics.empty() ||
                        !file->parse.diagnostics.empty() || file->parse.file == nullptr;
  }
  if (lex_parse_failed) {
    if (diagnostics.empty()) {
      diagnostics.push_back(make_internal_error("prelude failed to parse"));
    }
    respond_failure(res, diagnostics);
    return;
  }

  auto resolve_result = resolve(prog.program);
  collect_diagnostics(diagnostics, prog, resolve_result.diagnostics);
  if (has_user_error(resolve_result.diagnostics, prog)) {
    respond_failure(res, diagnostics);
    return;
  }

  TypeContext types;
  auto check_result = typecheck(prog.program, resolve_result, types);
  collect_diagnostics(diagnostics, prog, check_result.diagnostics);
  bool has_user_type_errors =
      std::ranges::any_of(check_result.diagnostics, [&prog](const auto& diag) -> bool {
        return prog.in_user_file(diag.span.offset) && diag.severity == Severity::Error;
      });
  if (has_user_type_errors) {
    respond_failure(res, diagnostics);
    return;
  }

  HirContext hir_ctx;
  auto hir_result = build_hir(prog.program, resolve_result, check_result, hir_ctx);
  collect_diagnostics(diagnostics, prog, hir_result.diagnostics);
  if (hir_result.program == nullptr) {
    if (diagnostics.empty()) {
      diagnostics.push_back(
          make_internal_error("HIR lowering failed (possible prelude error)"));
    }
    respond_failure(res, diagnostics);
    return;
  }

  MirContext mir_ctx;
  auto mir_result = build_mir(*hir_result.program, mir_ctx, types);
  collect_diagnostics(diagnostics, prog, mir_result.diagnostics);
  bool mono_has_errors = false;
  if (mir_result.module != nullptr) {
    auto mono = monomorphize(*mir_result.module, mir_ctx, types,
                             mir_result.generic_templates);
    collect_diagnostics(diagnostics, prog, mono.diagnostics);
    // Halt before LLVM lowering on mono errors (e.g. MIR
    // concreteness invariant violations — Task 28 §14.2).
    // Allowing generic residue through surfaces as an opaque LLVM
    // DataLayout assertion.
    mono_has_errors = has_error_severity(mono.diagnostics);
  }
  if (mir_result.module == nullptr || mono_has_errors) {
    if (diagnostics.empty()) {
      diagnostics.push_back(
          make_internal_error("MIR lowering failed (possible prelude error)"));
    }
    respond_failure(res, diagnostics);
    return;
  }

  // LLVM lowering.
  llvm::LLVMContext llvm_ctx;
  LlvmBackend backend(llvm_ctx);
  auto llvm_result =
      backend.lower(*mir_result.module, &prog.program.source_map, prog.program.entry);

  // Filter prelude-origin warnings (same as driver).
  std::vector<Diagnostic> user_diags;
  for (const auto& diag : llvm_result.diagnostics) {
    if (diag.severity == Severity::Warning &&
        prog.program.source_map.is_prelude(diag.span.offset)) {
      continue;
    }
    user_diags.push_back(diag);
  }
  collect_diagnostics(diagnostics, prog, user_diags);

  if (llvm_result.module == nullptr || has_error_severity(user_diags)) {
    respond_failure(res, diagnostics);
    return;
  }

  // Emit object file in a per-request temp directory.
  static std::atomic<uint64_t> request_id{0};
  auto run_id = std::to_string(request_id.fetch_add(1));
  auto tmp_dir =
      std::filesystem::temp_directory_path() / "dao_playground" / run_id;
  std::filesystem::create_directories(tmp_dir);
  auto obj_path = tmp_dir / "playground.o";
  auto exe_path = tmp_dir / "playground_exe";

  std::string emit_error;
  if (!LlvmBackend::emit_object(*llvm_result.module,
                                 obj_path.string(), emit_error)) {
    diagnostics.push_back(make_internal_error("emit object failed: " + emit_error));
    respond_failure(res, diagnostics);
    return;
  }

  // Link: cc obj + runtime → executable.
  auto cc_path = llvm::sys::findProgramByName("cc");
  if (!cc_path) {
    std::filesystem::remove(obj_path);
    diagnostics.push_back(make_internal_error("cannot find 'cc' linker"));
    respond_failure(res, diagnostics);
    return;
  }

  auto obj_str = obj_path.string();
  auto exe_str = exe_path.string();
  std::vector<llvm::StringRef> link_args = {
      *cc_path, obj_str, DAO_RUNTIME_LIB, "-o", exe_str,
  };

  std::string link_error;
  int link_status = llvm::sys::ExecuteAndWait(
      *cc_path, link_args, /*Env=*/std::nullopt,
      /*Redirects=*/{}, /*SecondsToWait=*/30, /*MemoryLimit=*/0,
      &link_error);
  std::filesystem::remove(obj_path);

  if (link_status != 0) {
    std::filesystem::remove(exe_path);
    std::string msg = "linking failed";
    if (!link_error.empty()) {
      msg += ": " + link_error;
    }
    diagnostics.push_back(make_internal_error(msg));
    respond_failure(res, diagnostics);
    return;
  }

  // Execute the program with stdout/stderr capture and timeout.
  auto stdout_path = tmp_dir / "stdout.txt";
  auto stderr_path = tmp_dir / "stderr.txt";

  // Redirects: stdin=none, stdout=file, stderr=file.
  // StringRef does not own data — keep string temporaries alive.
  auto stdout_str = stdout_path.string();
  auto stderr_str = stderr_path.string();
  std::array<std::optional<llvm::StringRef>, 3> redirects = {{
      llvm::StringRef(""),
      llvm::StringRef(stdout_str),
      llvm::StringRef(stderr_str),
  }};

  std::string exec_error;
  int exit_code = llvm::sys::ExecuteAndWait(
      exe_str, {exe_str}, /*Env=*/std::nullopt,
      redirects, /*SecondsToWait=*/5,
      /*MemoryLimit=*/256 * 1024 * 1024, &exec_error);

  auto stdout_text = slurp(stdout_path);
  auto stderr_text = slurp(stderr_path);

  // Append execution error info if process was killed.
  if (!exec_error.empty()) {
    if (!stderr_text.empty()) {
      stderr_text += "\n";
    }
    stderr_text += exec_error;
  }

  // Clean up per-request directory.
  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);

  nlohmann::json response = {
      {"stdout", stdout_text},
      {"stderr", stderr_text},
      {"exit_code", exit_code},
      {"diagnostics", diagnostics},
  };
  res.set_content(response.dump(), "application/json");
}

} // namespace dao::playground
// NOLINTEND(readability-magic-numbers)
