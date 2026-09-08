#include "completions.h"
#include "pipeline.h"

#include "analysis/completion.h"

#include <string>
#include <string_view>
#include <vector>

namespace dao::playground {

namespace {

/// Position just after the last non-blank character before `offset`
/// (buffer-local), or 0 when there is none.
auto scan_back_past_whitespace(std::string_view contents, uint32_t offset) -> uint32_t {
  auto pos = offset;
  while (pos > 0 && (contents[pos - 1] == ' ' || contents[pos - 1] == '\t')) {
    --pos;
  }
  return pos;
}

/// Semantic type of a symbol from the typed results.  Locals and
/// parameters have receiver types; other symbol kinds do not.
auto resolve_receiver_type(const Symbol* sym, const TypeCheckResult& typed) -> const Type* {
  if (sym == nullptr || sym->decl == nullptr) {
    return nullptr;
  }
  switch (sym->kind) {
  case SymbolKind::Local:
    return typed.typed.local_type(sym->decl_as_stmt());
  case SymbolKind::Param: {
    const auto* fn_decl = sym->decl_as_decl();
    const auto* fn_type = typed.typed.decl_type(fn_decl);
    if (fn_type == nullptr || fn_type->kind() != TypeKind::Function ||
        !fn_decl->is<FunctionDecl>()) {
      return nullptr;
    }
    const auto* func = static_cast<const TypeFunction*>(fn_type);
    const auto& decl = fn_decl->as<FunctionDecl>();
    for (size_t idx = 0; idx < decl.params.size(); ++idx) {
      if (decl.params[idx].name == sym->name && idx < func->param_types().size()) {
        return func->param_types()[idx];
      }
    }
    return nullptr;
  }
  default:
    return nullptr;
  }
}

/// Type of the outermost typed expression ending exactly at `dot_pos`
/// (a program offset).  Covers receivers that are not plain
/// identifiers: `make_point().`, `p.x.`, `arr[i].`.
auto find_expr_type_before_dot(uint32_t dot_pos, const TypeCheckResult& typed) -> const Type* {
  const Type* best = nullptr;
  uint32_t best_length = 0;
  for (const auto& [expr, type] : typed.typed.expr_types()) {
    auto end = expr->span.offset + expr->span.length;
    if (end == dot_pos && type != nullptr && (best == nullptr || expr->span.length > best_length)) {
      best = type;
      best_length = expr->span.length;
    }
  }
  return best;
}

/// Receiver type when the program offset follows `.` (ignoring blanks),
/// or nullptr when this is not a member completion.
auto dot_receiver(uint32_t absolute_offset, const FrontendPipeline& pipe) -> const Type* {
  const auto& user = *pipe.prog.user;
  auto contents = user.buffer.contents();
  if (!user.contains(absolute_offset) || absolute_offset == user.base_offset) {
    return nullptr;
  }
  auto local = user.local_offset(absolute_offset);
  auto pos = scan_back_past_whitespace(contents, local);
  if (pos == 0 || contents[pos - 1] != '.') {
    return nullptr;
  }

  // Fast path: a plain identifier receiver (local or parameter).
  auto pre_dot = scan_back_past_whitespace(contents, pos - 1);
  auto token_off = token_start_at(user.base_offset + (pre_dot > 0 ? pre_dot - 1 : 0), user.lex);
  if (auto use_it = pipe.resolve_result.uses.find(token_off);
      use_it != pipe.resolve_result.uses.end()) {
    if (const auto* sym_type = resolve_receiver_type(use_it->second, pipe.check_result)) {
      return sym_type;
    }
  }

  // General path: the typed expression that ends at the dot.
  return find_expr_type_before_dot(user.base_offset + (pos - 1), pipe.check_result);
}

} // namespace

auto completions(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto inputs = parse_program_request(request);
  if (!inputs) {
    return error_reply(http_status::bad_request, inputs.error());
  }
  auto pipe = run_frontend_pipeline(ctx.repo_root, std::move(*inputs));
  if (pipe.prog.user != nullptr) {
    if (auto offset = document_offset(request, pipe.prog); !offset) {
      return error_reply(http_status::bad_request, offset.error());
    }
  }
  if (!pipe.ok) {
    return {.status = http_status::ok, .body = nlohmann::json::array()};
  }

  auto absolute_offset = pipe.prog.to_program_offset(*document_offset(request, pipe.prog));
  const auto* receiver = dot_receiver(absolute_offset, pipe);
  // The method set is the querying module's: what the document's own
  // module can call at this position.
  const auto* at = pipe.prog.program.source_map.file_for(absolute_offset);
  const ModuleInfo* from_module = at != nullptr ? at->module : nullptr;
  auto items = receiver != nullptr
                   ? query_dot_completions(receiver, pipe.check_result, from_module)
                   : query_completions(absolute_offset, pipe.resolve_result, pipe.check_result);

  nlohmann::json body = nlohmann::json::array();
  for (const auto& item : items) {
    body.push_back({
        {"label", item.label},
        {"kind", item.kind},
        {"type", item.type},
    });
  }
  return {.status = http_status::ok, .body = std::move(body)};
}

} // namespace dao::playground
