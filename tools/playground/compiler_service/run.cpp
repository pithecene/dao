// NOLINTBEGIN(readability-magic-numbers)
#include "run.h"
#include "pipeline.h"

#include "backend/llvm/llvm_backend.h"
#include "ir/hir/hir_builder.h"
#include "ir/hir/hir_context.h"
#include "ir/mir/mir_builder.h"
#include "ir/mir/mir_context.h"
#include "ir/mir/mir_monomorphize.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/Program.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dao::playground {

namespace {

// Read entire file into string, for capturing process output.
auto slurp(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

auto run_reply(std::string stdout_text,
               std::string stderr_text,
               int exit_code,
               nlohmann::json diagnostics) -> Reply {
  return {.status = http_status::ok,
          .body = {
              {"stdout", std::move(stdout_text)},
              {"stderr", std::move(stderr_text)},
              {"exit_code", exit_code},
              {"diagnostics", std::move(diagnostics)},
          }};
}

auto compile_failed(nlohmann::json diagnostics) -> Reply {
  return run_reply("", "", -1, std::move(diagnostics));
}

auto compile_failed(nlohmann::json diagnostics, const std::string& fallback_message) -> Reply {
  if (diagnostics.empty()) {
    diagnostics.push_back(make_unlocated_diagnostic(fallback_message));
  }
  return compile_failed(std::move(diagnostics));
}

} // namespace

void init_run_support() {
  LlvmBackend::initialize_targets();
}

auto run_program(ProgramRequest inputs, const ServiceContext& ctx) -> Reply {
  nlohmann::json diagnostics = nlohmann::json::array();

  // Running needs an entry point, so here a missing one is an error
  // rather than the linker's `undefined main`.
  auto prog = build_playground_program(ctx.repo_root, std::move(inputs), EntryPolicy::Required);
  // Graph, lex, and parse diagnostics of every file as one §8.4-ordered
  // stream, reported before anything can return: a graph error must not
  // hide the parse error that explains it.
  collect_program_diagnostics(diagnostics, prog);
  if (prog.user == nullptr || has_error_severity(prog.program.diagnostics)) {
    return compile_failed(std::move(diagnostics));
  }
  // Any file failing to lex or parse stops the run.
  if (!prog.program.lexed_and_parsed_cleanly()) {
    return compile_failed(std::move(diagnostics), "prelude failed to parse");
  }

  auto resolve_result = resolve(prog.program);
  collect_diagnostics(diagnostics, prog, resolve_result.diagnostics);
  if (has_error_severity(resolve_result.diagnostics)) {
    return compile_failed(std::move(diagnostics));
  }

  TypeContext types;
  auto check_result = typecheck(prog.program, resolve_result, types);
  collect_diagnostics(diagnostics, prog, check_result.diagnostics);
  if (has_error_severity(check_result.diagnostics)) {
    return compile_failed(std::move(diagnostics));
  }

  HirContext hir_ctx;
  auto hir_result = build_hir(prog.program, resolve_result, check_result, hir_ctx);
  collect_diagnostics(diagnostics, prog, hir_result.diagnostics);
  if (hir_result.module == nullptr || has_error_severity(hir_result.diagnostics)) {
    return compile_failed(std::move(diagnostics), "HIR lowering failed without a diagnostic");
  }

  MirContext mir_ctx;
  auto mir_result = build_mir(*hir_result.module, mir_ctx, types);
  collect_diagnostics(diagnostics, prog, mir_result.diagnostics);
  bool mono_has_errors = false;
  if (mir_result.module != nullptr) {
    auto mono = monomorphize(*mir_result.module, mir_ctx, types, mir_result.generic_templates);
    collect_diagnostics(diagnostics, prog, mono.diagnostics);
    // Halt before LLVM lowering on monomorphization errors: generic
    // residue reaching the backend surfaces as an opaque LLVM
    // DataLayout assertion instead of a diagnostic.
    mono_has_errors = has_error_severity(mono.diagnostics);
  }
  if (mir_result.module == nullptr || has_error_severity(mir_result.diagnostics) ||
      mono_has_errors) {
    return compile_failed(std::move(diagnostics), "MIR lowering failed without a diagnostic");
  }

  // LLVM lowering.
  llvm::LLVMContext llvm_ctx;
  LlvmBackend backend(llvm_ctx);
  auto llvm_result = backend.lower(*mir_result.module, &prog.program.source_map);

  auto user_diags = without_prelude_warnings(llvm_result.diagnostics, prog);
  collect_diagnostics(diagnostics, prog, user_diags);
  if (llvm_result.module == nullptr || has_error_severity(user_diags)) {
    return compile_failed(std::move(diagnostics));
  }

  // Emit object file in a per-request temp directory.
  static std::atomic<uint64_t> request_id{0};
  auto run_id = std::to_string(request_id.fetch_add(1));
  auto tmp_dir = std::filesystem::temp_directory_path() / "dao_playground" / run_id;
  std::filesystem::create_directories(tmp_dir);
  auto obj_path = tmp_dir / "playground.o";
  auto exe_path = tmp_dir / "playground_exe";

  std::string emit_error;
  if (!LlvmBackend::emit_object(*llvm_result.module, obj_path.string(), emit_error)) {
    diagnostics.push_back(make_unlocated_diagnostic("emit object failed: " + emit_error));
    return compile_failed(std::move(diagnostics));
  }

  // Link: cc obj + runtime → executable.
  auto cc_path = llvm::sys::findProgramByName("cc");
  if (!cc_path) {
    std::filesystem::remove(obj_path);
    diagnostics.push_back(make_unlocated_diagnostic("cannot find 'cc' linker"));
    return compile_failed(std::move(diagnostics));
  }

  auto obj_str = obj_path.string();
  auto exe_str = exe_path.string();
  std::vector<llvm::StringRef> link_args = {*cc_path, obj_str, DAO_RUNTIME_LIB, "-o", exe_str};

  std::string link_error;
  int link_status = llvm::sys::ExecuteAndWait(*cc_path,
                                              link_args,
                                              /*Env=*/std::nullopt,
                                              /*Redirects=*/{},
                                              /*SecondsToWait=*/30,
                                              /*MemoryLimit=*/0,
                                              &link_error);
  std::filesystem::remove(obj_path);

  if (link_status != 0) {
    std::filesystem::remove(exe_path);
    std::string msg = "linking failed";
    if (!link_error.empty()) {
      msg += ": " + link_error;
    }
    diagnostics.push_back(make_unlocated_diagnostic(msg));
    return compile_failed(std::move(diagnostics));
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
  int exit_code = llvm::sys::ExecuteAndWait(exe_str,
                                            {exe_str},
                                            /*Env=*/std::nullopt,
                                            redirects,
                                            /*SecondsToWait=*/5,
                                            /*MemoryLimit=*/256 * 1024 * 1024,
                                            &exec_error);

  auto stdout_text = slurp(stdout_path);
  auto stderr_text = slurp(stderr_path);

  // Append execution error info if the process was killed.
  if (!exec_error.empty()) {
    if (!stderr_text.empty()) {
      stderr_text += "\n";
    }
    stderr_text += exec_error;
  }

  std::error_code ec;
  std::filesystem::remove_all(tmp_dir, ec);

  return run_reply(
      std::move(stdout_text), std::move(stderr_text), exit_code, std::move(diagnostics));
}

auto run(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto inputs = parse_program_request(request);
  if (!inputs) {
    return error_reply(http_status::bad_request, inputs.error());
  }
  auto document = inputs->document;
  auto reply = run_program(std::move(*inputs), ctx);
  reply.body["file"] = std::move(document);
  return reply;
}

} // namespace dao::playground
// NOLINTEND(readability-magic-numbers)
