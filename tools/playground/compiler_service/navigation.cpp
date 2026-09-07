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

auto null_reply() -> Reply {
  return {.status = http_status::ok, .body = nullptr};
}

auto list_reply(nlohmann::json items) -> Reply {
  return {.status = http_status::ok, .body = std::move(items)};
}

/// Pipeline plus the program offset of the token under the request's
/// editor offset — what every offset query starts from.
struct OffsetQuery {
  FrontendPipeline pipe;
  uint32_t token_offset = 0;
  std::string error; // why the request was unusable, else empty
};

auto query_at(const nlohmann::json& request, const ServiceContext& ctx) -> OffsetQuery {
  auto inputs = parse_program_request(request);
  if (!inputs) {
    return {.error = inputs.error()};
  }
  OffsetQuery query{.pipe = run_frontend_pipeline(ctx.repo_root, std::move(*inputs))};
  if (query.pipe.prog.user != nullptr) {
    auto offset = document_offset(request, query.pipe.prog);
    if (!offset) {
      query.error = offset.error();
      return query;
    }
    if (query.pipe.ok) {
      auto absolute = query.pipe.prog.to_program_offset(*offset);
      query.token_offset = token_start_at(absolute, query.pipe.prog.user->lex);
    }
  }
  return query;
}

} // namespace

auto hover(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto query = query_at(request, ctx);
  if (!query.error.empty()) {
    return error_reply(http_status::bad_request, query.error);
  }
  if (!query.pipe.ok) {
    return null_reply();
  }
  auto result = query_hover(query.token_offset, query.pipe.resolve_result, query.pipe.check_result);
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
  if (!query.error.empty()) {
    return error_reply(http_status::bad_request, query.error);
  }
  if (!query.pipe.ok) {
    return null_reply();
  }
  auto result = query_definition(query.token_offset, query.pipe.resolve_result);
  if (!result) {
    return null_reply();
  }
  // The definition may live in another file of the program (the
  // prelude today); the reply says which, and the consumer decides
  // whether it can show it.
  nlohmann::json body = {{"length", result->length}};
  add_position(body, query.pipe.prog, result->offset);
  return {.status = http_status::ok, .body = std::move(body)};
}

auto document_symbols(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto inputs = parse_program_request(request);
  if (!inputs) {
    return error_reply(http_status::bad_request, inputs.error());
  }
  auto pipe = run_frontend_pipeline(ctx.repo_root, std::move(*inputs));
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
        {"file", pipe.prog.user->display_path},
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
  if (!query.error.empty()) {
    return error_reply(http_status::bad_request, query.error);
  }
  if (!query.pipe.ok) {
    return list_reply(nlohmann::json::array());
  }
  nlohmann::json refs = nlohmann::json::array();
  for (const auto& ref : query_references(query.token_offset, query.pipe.resolve_result)) {
    nlohmann::json entry = {{"length", ref.span.length}, {"isDefinition", ref.is_definition}};
    add_position(entry, query.pipe.prog, ref.span.offset);
    refs.push_back(std::move(entry));
  }
  return list_reply(std::move(refs));
}

} // namespace dao::playground
