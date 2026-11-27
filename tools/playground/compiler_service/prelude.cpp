#include "prelude.h"

#include "analysis/document_symbols.h"
#include "analysis/semantic_tokens.h"
#include "frontend/module/program.h"
#include "frontend/resolve/resolve.h"
#include "pipeline.h"

#include <llvm/Config/llvm-config.h>

#include <algorithm>
#include <functional>
#include <string>
#include <utility>

#ifndef DAO_GIT_COMMIT
#define DAO_GIT_COMMIT "unknown"
#endif

namespace dao::playground {

namespace {

auto is_prelude_request_path(const std::string& path) -> bool {
  const bool in_prelude_root = path.starts_with("stdlib/core/") || path.starts_with("stdlib/io/");
  return in_prelude_root && path.ends_with(".dao") && path.find("..") == std::string::npos &&
         path.find('\\') == std::string::npos;
}

} // namespace

auto service_version(const nlohmann::json& /*request*/, const ServiceContext& ctx) -> Reply {
  nlohmann::json prelude_files = nlohmann::json::array();
  for (const auto& input : load_prelude_inputs(ctx.repo_root / "stdlib")) {
    prelude_files.push_back(input.display_path);
  }
  return {.status = http_status::ok,
          .body = {
              {"commit", std::string(DAO_GIT_COMMIT)},
              {"llvmVersion", std::string(LLVM_VERSION_STRING)},
              {"targetTriple", std::string(LLVM_HOST_TRIPLE)},
              {"preludeFiles", std::move(prelude_files)},
          }};
}

auto prelude_source(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto path = request["path"].get<std::string>();
  if (!is_prelude_request_path(path)) {
    return error_reply(http_status::bad_request, "invalid prelude file path");
  }

  auto inputs = load_prelude_inputs(ctx.repo_root / "stdlib");
  const bool exists = std::ranges::any_of(
      inputs, [&path](const SourceInput& input) -> bool { return input.display_path == path; });
  if (!exists) {
    return error_reply(http_status::not_found, "prelude file not found");
  }

  auto program = build_program(std::move(inputs), {}, EntryPolicy::Optional);
  const SourceFile* target = nullptr;
  for (const auto& file : program.files) {
    if (file->display_path == path) {
      target = file.get();
      break;
    }
  }
  if (target == nullptr || target->file() == nullptr) {
    return error_reply(http_status::not_found, "prelude file failed to parse");
  }

  auto resolved = resolve(program);
  // Positions in the editor's unit, within the target file.
  const Utf16Index utf16(target->buffer.contents());
  auto editor_span = [&](Span span) -> Span {
    return utf16.to_utf16({.offset = target->local_offset(span.offset), .length = span.length});
  };
  nlohmann::json semantic_tokens = nlohmann::json::array();
  for (const auto& stok : classify_tokens(target->lex.tokens, target->file(), &resolved)) {
    if (!target->contains_span(stok.span)) {
      continue;
    }
    auto loc = program.source_map.locate(stok.span.offset);
    semantic_tokens.push_back({
        {"kind", stok.kind},
        {"offset", editor_span(stok.span).offset},
        {"length", editor_span(stok.span).length},
        {"line", loc.line},
        {"col", utf16.column(target->local_offset(stok.span.offset), loc.col)},
    });
  }

  std::function<nlohmann::json(const DocumentSymbol&)> symbol_to_json;
  symbol_to_json = [&](const DocumentSymbol& sym) -> nlohmann::json {
    nlohmann::json children = nlohmann::json::array();
    for (const auto& child : sym.children) {
      children.push_back(symbol_to_json(child));
    }
    return {
        {"name", sym.name},
        {"kind", sym.kind},
        {"file", target->display_path},
        {"offset", editor_span(sym.span).offset},
        {"length", editor_span(sym.span).length},
        {"children", std::move(children)},
    };
  };

  nlohmann::json symbols = nlohmann::json::array();
  for (const auto& sym : query_document_symbols(*target->file())) {
    symbols.push_back(symbol_to_json(sym));
  }

  std::string module_display = target->module != nullptr ? target->module->display : "";
  return {.status = http_status::ok,
          .body = {
              {"file", target->display_path},
              {"module", std::move(module_display)},
              {"source", std::string(target->buffer.contents())},
              {"semanticTokens", std::move(semantic_tokens)},
              {"symbols", std::move(symbols)},
          }};
}

} // namespace dao::playground
