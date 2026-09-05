#include "pipeline.h"
#include "support/module_utils.h"

#include <algorithm>
#include <utility>

namespace dao::playground {

namespace {

constexpr const char* kSyntheticHeader = "module playground\n";

} // namespace

auto build_playground_program(const std::filesystem::path& repo_root,
                              std::string user_source) -> PlaygroundProgram {
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
  inputs.push_back({.display_path = "<playground>",
                    .text = std::move(user_source),
                    .is_prelude = false});
  prog.program = build_program(std::move(inputs));
  auto user_files = prog.program.user_files();
  prog.user = user_files.empty() ? nullptr : user_files.front();
  return prog;
}

auto has_user_error(const std::vector<Diagnostic>& diags, const PlaygroundProgram& prog) -> bool {
  return std::ranges::any_of(diags, [&prog](const auto& diag) -> bool {
    return prog.in_user_file(diag.span.offset);
  });
}

void collect_diagnostics(nlohmann::json& out, const PlaygroundProgram& prog,
                         const std::vector<Diagnostic>& diags) {
  for (const auto& diag : diags) {
    if (!prog.in_user_file(diag.span.offset)) {
      continue;
    }
    auto loc = prog.program.source_map.locate(diag.span.offset);
    out.push_back({
        {"severity", diag.severity == Severity::Warning ? "warning" : "error"},
        {"offset", prog.to_editor_offset(diag.span.offset)},
        {"length", diag.span.length},
        {"line", prog.editor_line(diag.span.offset)},
        {"col", loc.col},
        {"message", diag.message},
    });
  }
}

auto make_internal_error(const std::string& message) -> nlohmann::json {
  return {
      {"severity", "error"},
      {"offset", 0},
      {"length", 0},
      {"line", 1},
      {"col", 1},
      {"message", message},
  };
}

} // namespace dao::playground
