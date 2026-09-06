#include "pipeline.h"
#include "support/module_utils.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

namespace dao::playground {

namespace {

constexpr const char* kSyntheticHeader = "module playground\n";

} // namespace

auto parse_program_request(const nlohmann::json& request)
    -> std::expected<ProgramRequest, std::string> {
  ProgramRequest parsed;
  parsed.document = request["document"].get<std::string>();
  for (const auto& file : request["files"]) {
    parsed.files.push_back({.display_path = file["path"].get<std::string>(),
                            .text = file["source"].get<std::string>(),
                            .is_prelude = false});
  }
  const bool named = std::ranges::any_of(parsed.files, [&](const SourceInput& file) -> bool {
    return file.display_path == parsed.document;
  });
  if (!named) {
    return std::unexpected(std::format("document '{}' is not one of the files", parsed.document));
  }
  return parsed;
}

auto build_playground_program(const std::filesystem::path& repo_root, ProgramRequest request)
    -> PlaygroundProgram {
  PlaygroundProgram prog;

  auto inputs = load_prelude_inputs(repo_root / "stdlib");
  for (auto& file : request.files) {
    // Per CONTRACT_SYNTAX_SURFACE.md every source file begins with one
    // `module` declaration.  A scratch document usually has none; give
    // it a synthetic identity and remember how many bytes/lines to
    // subtract when reporting positions back to the editor.  Other
    // files of the program are taken as written.
    if (file.display_path == request.document && !starts_with_module(file.text)) {
      std::string with_header = kSyntheticHeader;
      prog.header_bytes = static_cast<uint32_t>(with_header.size());
      prog.header_lines = 1;
      with_header.append(file.text);
      file.text = std::move(with_header);
    }
    inputs.push_back(std::move(file));
  }
  prog.program = build_program(std::move(inputs));
  for (const auto& file : prog.program.files) {
    if (!file->is_prelude && file->display_path == request.document) {
      prog.user = file.get();
    }
  }
  return prog;
}

auto run_frontend_pipeline(const std::filesystem::path& repo_root, ProgramRequest request)
    -> FrontendPipeline {
  FrontendPipeline pipe;
  pipe.prog = build_playground_program(repo_root, std::move(request));
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
  out["offset"] =
      in_document ? prog.to_editor_offset(program_offset) : file->local_offset(program_offset);
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

void collect_diagnostics(nlohmann::json& out,
                         const PlaygroundProgram& prog,
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
