#include "navigation.h"
#include "pipeline.h"

#include "analysis/document_symbols.h"
#include "analysis/goto_definition.h"
#include "analysis/hover.h"
#include "analysis/references.h"

#include <functional>
#include <string>

namespace dao::playground {

namespace {

auto null_reply() -> Reply { return {.status = http_status::ok, .body = nullptr}; }

auto list_reply(nlohmann::json items) -> Reply {
  return {.status = http_status::ok, .body = std::move(items)};
}

/// Pipeline plus the program offset of the token under the request's
/// editor offset — what every offset query starts from.
struct OffsetQuery {
  FrontendPipeline pipe;
  uint32_t token_offset = 0;
};

auto query_at(const nlohmann::json& request, const ServiceContext& ctx) -> OffsetQuery {
  OffsetQuery query{.pipe = run_frontend_pipeline(ctx.repo_root, request["source"].get<std::string>())};
  if (query.pipe.ok) {
    auto absolute = query.pipe.prog.to_program_offset(request["offset"].get<uint32_t>());
    query.token_offset = token_start_at(absolute, query.pipe.prog.user->lex);
  }
  return query;
}

} // namespace

auto hover(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto query = query_at(request, ctx);
  if (!query.pipe.ok) {
    return null_reply();
  }
  auto result =
      query_hover(query.token_offset, query.pipe.resolve_result, query.pipe.check_result);
  if (!result) {
    return null_reply();
  }
  return {.status = http_status::ok,
          .body = {
              {"name", result->name},
              {"kind", result->symbol_kind},
              {"type", result->type},
          }};
}

auto goto_definition(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto query = query_at(request, ctx);
  if (!query.pipe.ok) {
    return null_reply();
  }
  auto result = query_definition(query.token_offset, query.pipe.resolve_result);
  // A definition outside the editor buffer (prelude) is not navigable
  // in the user's source.
  if (!result || !query.pipe.prog.in_user_file(result->offset)) {
    return null_reply();
  }
  const auto& prog = query.pipe.prog;
  auto loc = prog.program.source_map.locate(result->offset);
  return {.status = http_status::ok,
          .body = {
              {"offset", prog.to_editor_offset(result->offset)},
              {"length", result->length},
              {"line", prog.editor_line(result->offset)},
              {"col", loc.col},
          }};
}

auto document_symbols(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto pipe = run_frontend_pipeline(ctx.repo_root, request["source"].get<std::string>());
  if (!pipe.ok || pipe.prog.user->file() == nullptr) {
    return list_reply(nlohmann::json::array());
  }

  std::function<nlohmann::json(const DocumentSymbol&)> to_json;
  to_json = [&](const DocumentSymbol& sym) -> nlohmann::json {
    nlohmann::json children = nlohmann::json::array();
    for (const auto& child : sym.children) {
      children.push_back(to_json(child));
    }
    return {
        {"name", sym.name},
        {"kind", sym.kind},
        {"offset", pipe.prog.to_editor_offset(sym.span.offset)},
        {"length", sym.span.length},
        {"children", children},
    };
  };

  nlohmann::json symbols = nlohmann::json::array();
  for (const auto& sym : query_document_symbols(*pipe.prog.user->file())) {
    symbols.push_back(to_json(sym));
  }
  return list_reply(std::move(symbols));
}

auto references(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto query = query_at(request, ctx);
  if (!query.pipe.ok) {
    return list_reply(nlohmann::json::array());
  }
  const auto& prog = query.pipe.prog;
  nlohmann::json refs = nlohmann::json::array();
  for (const auto& ref : query_references(query.token_offset, query.pipe.resolve_result)) {
    if (!prog.in_user_file(ref.span.offset)) {
      continue; // prelude references are not navigable from the buffer
    }
    refs.push_back({
        {"offset", prog.to_editor_offset(ref.span.offset)},
        {"length", ref.span.length},
        {"isDefinition", ref.is_definition},
    });
  }
  return list_reply(std::move(refs));
}

} // namespace dao::playground
