#include "pipeline.h"
#include "support/module_utils.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace dao::playground {

namespace {

constexpr const char* kSyntheticHeader = "module playground\n";

} // namespace

auto build_playground_program(const std::filesystem::path& repo_root, std::string user_source)
    -> PlaygroundProgram {
  PlaygroundProgram prog;

  // Per CONTRACT_SYNTAX_SURFACE.md every source file begins with one
  // `module` declaration.  A scratch buffer usually has none; give it
  // a synthetic identity and remember how many bytes/lines to subtract
  // when reporting positions back to the editor.
  if (!starts_with_module(user_source)) {
    std::string with_header = kSyntheticHeader;
    prog.header_bytes = static_cast<uint32_t>(with_header.size());
    prog.header_lines = 1;
    with_header.append(user_source);
    user_source = std::move(with_header);
  }

  auto inputs = load_prelude_inputs(repo_root / "stdlib");
  inputs.push_back({.display_path = std::string(kDocumentPath),
                    .text = std::move(user_source),
                    .is_prelude = false});
  prog.program = build_program(std::move(inputs));
  prog.user = prog.program.files.empty() ? nullptr : prog.program.files.back().get();
  return prog;
}

auto run_frontend_pipeline(const std::filesystem::path& repo_root, std::string user_source)
    -> FrontendPipeline {
  FrontendPipeline pipe;
  pipe.prog = build_playground_program(repo_root, std::move(user_source));
  if (pipe.prog.user == nullptr || !pipe.prog.program.diagnostics.empty()) {
    return pipe;
  }
  // The buffer is whatever the user has typed so far and usually does
  // not parse; the parser recovers and the resolver and checker work on
  // the partial tree, so navigation and completion answer while typing.
  // Only a buffer that produced no tree, or a prelude file that failed,
  // stops the pipeline.
  for (const auto& file : pipe.prog.program.files) {
    bool broken =
        file->parse.file == nullptr ||
        (file->is_prelude && (!file->lex.diagnostics.empty() || !file->parse.diagnostics.empty()));
    if (broken) {
      return pipe;
    }
  }
  pipe.resolve_result = resolve(pipe.prog.program);
  pipe.check_result = typecheck(pipe.prog.program, pipe.resolve_result, pipe.types);
  pipe.ok = true;
  return pipe;
}

auto token_start_at(uint32_t offset, const LexResult& lex) -> uint32_t {
  auto it = std::ranges::find_if(lex.tokens, [offset](const Token& tok) -> bool {
    return tok.span.offset <= offset && offset < tok.span.offset + tok.span.length;
  });
  return it == lex.tokens.end() ? offset : it->span.offset;
}

auto has_error_severity(const std::vector<Diagnostic>& diags) -> bool {
  return std::ranges::any_of(
      diags, [](const Diagnostic& diag) -> bool { return diag.severity == Severity::Error; });
}

auto has_user_error(const std::vector<Diagnostic>& diags, const PlaygroundProgram& prog) -> bool {
  return std::ranges::any_of(diags, [&prog](const Diagnostic& diag) -> bool {
    return diag.severity == Severity::Error && prog.in_user_file(diag.span.offset);
  });
}

void add_position(nlohmann::json& out, const PlaygroundProgram& prog, uint32_t program_offset) {
  const auto* file = prog.program.source_map.file_for(program_offset);
  if (file == nullptr) {
    out["file"] = "";
    out["offset"] = 0;
    out["line"] = 1;
    out["col"] = 1;
    return;
  }
  auto loc = prog.program.source_map.locate(program_offset);
  const bool in_document = file == prog.user;
  out["file"] = file->display_path;
  out["offset"] = in_document ? prog.to_editor_offset(program_offset)
                              : file->local_offset(program_offset);
  out["line"] = in_document ? prog.editor_line(program_offset) : loc.line;
  out["col"] = loc.col;
}

auto without_prelude_warnings(const std::vector<Diagnostic>& diags, const PlaygroundProgram& prog)
    -> std::vector<Diagnostic> {
  std::vector<Diagnostic> kept;
  std::ranges::copy_if(diags, std::back_inserter(kept), [&prog](const Diagnostic& diag) -> bool {
    return !(diag.severity == Severity::Warning &&
             prog.program.source_map.is_prelude(diag.span.offset));
  });
  return kept;
}

void collect_diagnostics(nlohmann::json& out, const PlaygroundProgram& prog,
                         const std::vector<Diagnostic>& diags) {
  for (const auto& diag : diags) {
    nlohmann::json entry = {
        {"severity", diag.severity == Severity::Warning ? "warning" : "error"},
        {"length", diag.span.length},
        {"message", diag.message},
    };
    add_position(entry, prog, diag.span.offset);
    out.push_back(std::move(entry));
  }
}

auto make_internal_error(const std::string& message) -> nlohmann::json {
  return {
      {"severity", "error"},
      {"file", ""},
      {"offset", 0},
      {"length", 0},
      {"line", 1},
      {"col", 1},
      {"message", message},
  };
}

} // namespace dao::playground
