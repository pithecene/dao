#include "pipeline.h"
#include "support/module_utils.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <set>
#include <utility>

namespace dao::playground {

namespace {

constexpr const char* kSyntheticHeader = "module playground\n";

} // namespace

auto parse_program_request(const nlohmann::json& request)
    -> std::expected<ProgramRequest, std::string> {
  ProgramRequest parsed;
  parsed.document = request["document"].get<std::string>();
  // A path names at most one file: it is the file's identity here and
  // its module identity in the program, and `document` must select
  // exactly one of them.
  std::set<std::string> paths;
  for (const auto& file : request["files"]) {
    auto path = file["path"].get<std::string>();
    if (!paths.insert(path).second) {
      return std::unexpected(std::format("duplicate file path '{}'", path));
    }
    parsed.files.push_back({.display_path = std::move(path),
                            .text = file["source"].get<std::string>(),
                            .is_prelude = false});
  }
  if (!paths.contains(parsed.document)) {
    return std::unexpected(std::format("document '{}' is not one of the files", parsed.document));
  }
  return parsed;
}

auto build_playground_program(const std::filesystem::path& repo_root,
                              ProgramRequest request,
                              EntryPolicy entry_policy) -> PlaygroundProgram {
  PlaygroundProgram prog;

  // Exactly one file carries the document's path (parse_program_request
  // rejects duplicates), so the header bookkeeping below and the `user`
  // selection after it name the same file.
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
  prog.program = build_program(std::move(inputs), {}, entry_policy);
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

auto document_offset(const nlohmann::json& request, const PlaygroundProgram& prog)
    -> std::expected<uint32_t, std::string> {
  const auto& value = request["offset"];
  const auto length = prog.document_length();
  auto reject = [&]() -> std::unexpected<std::string> {
    return std::unexpected(
        std::format("'offset' must be an integer from 0 to {} (the document's length)", length));
  };
  if (!value.is_number_integer()) {
    return reject();
  }
  if (value.is_number_unsigned()) {
    return value.get<uint64_t>() <= length
               ? std::expected<uint32_t, std::string>(static_cast<uint32_t>(value.get<uint64_t>()))
               : reject();
  }
  const auto signed_value = value.get<int64_t>();
  if (signed_value < 0 || signed_value > static_cast<int64_t>(length)) {
    return reject();
  }
  return static_cast<uint32_t>(signed_value);
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

void collect_program_diagnostics(nlohmann::json& out, const PlaygroundProgram& prog) {
  std::vector<Diagnostic> located;
  for (const auto& diag : prog.program.diagnostics) {
    if (diag.span.length == 0) {
      out.push_back(make_unlocated_diagnostic(diag.message, diag.severity));
    } else {
      located.push_back(diag);
    }
  }
  collect_diagnostics(out, prog, located);
}

void collect_assembly_diagnostics(nlohmann::json& out, const PlaygroundProgram& prog) {
  std::vector<Diagnostic> located;
  std::vector<Diagnostic> unlocated;
  for (const auto& file : prog.program.files) {
    std::ranges::copy(file->lex.diagnostics, std::back_inserter(located));
    std::ranges::copy(file->parse.diagnostics, std::back_inserter(located));
  }
  for (const auto& diag : prog.program.diagnostics) {
    (diag.span.length == 0 ? unlocated : located).push_back(diag);
  }
  // Files occupy disjoint ascending ranges of one offset space assigned
  // in file-id order, so sorting on the program offset is the contract's
  // file-then-offset order.  Stable: two diagnostics at one offset keep
  // the order the phase that produced them chose.
  std::ranges::stable_sort(located, {}, [](const Diagnostic& diag) { return diag.span.offset; });
  collect_diagnostics(out, prog, located);
  for (const auto& diag : unlocated) {
    out.push_back(make_unlocated_diagnostic(diag.message, diag.severity));
  }
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

auto make_unlocated_diagnostic(const std::string& message, Severity severity) -> nlohmann::json {
  return {
      {"severity", severity == Severity::Warning ? "warning" : "error"},
      {"file", ""},
      {"offset", 0},
      {"length", 0},
      {"line", 1},
      {"col", 1},
      {"message", message},
  };
}

} // namespace dao::playground
