// NOLINTBEGIN(readability-magic-numbers)
#include "analyze.h"
#include "pipeline.h"
#include "token_category.h"

#include "analysis/completion.h"
#include "analysis/document_symbols.h"
#include "analysis/goto_definition.h"
#include "analysis/hover.h"
#include "analysis/references.h"
#include "analysis/semantic_tokens.h"
#include "backend/llvm/llvm_backend.h"
#include "frontend/ast/ast_printer.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"
#include "ir/hir/hir_builder.h"
#include "ir/hir/hir_context.h"
#include "ir/hir/hir_printer.h"
#include "ir/mir/mir_builder.h"
#include "ir/mir/mir_context.h"
#include "ir/mir/mir_monomorphize.h"
#include "ir/mir/mir_printer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>
#include <string>

namespace dao::playground {

// ---------------------------------------------------------------------------
// Token serialization (editor buffer only; prelude files are not serialized)
// ---------------------------------------------------------------------------

namespace {

void build_token_array(nlohmann::json& out, const PlaygroundProgram& prog) {
  for (const auto& tok : prog.user->lex.tokens) {
    if (is_synthetic_token(tok.kind) || !prog.in_user_file(tok.span.offset) ||
        prog.user->local_offset(tok.span.offset) < prog.header_bytes) {
      continue; // synthetic tokens and the synthetic module header
    }
    auto loc = prog.program.source_map.locate(tok.span.offset);
    out.push_back({
        {"kind", token_kind_name(tok.kind)},
        {"category", token_category(tok.kind)},
        {"offset", prog.to_editor_offset(tok.span.offset)},
        {"length", tok.span.length},
        {"line", prog.editor_line(tok.span.offset)},
        {"col", loc.col},
        {"text", std::string(tok.text)},
    });
  }
}

void build_semantic_tokens(nlohmann::json& out,
                           const std::vector<SemanticToken>& sem_tokens,
                           const PlaygroundProgram& prog) {
  for (const auto& stok : sem_tokens) {
    if (!prog.in_user_file(stok.span.offset) ||
        prog.user->local_offset(stok.span.offset) < prog.header_bytes) {
      continue;
    }
    auto loc = prog.program.source_map.locate(stok.span.offset);
    out.push_back({
        {"kind", stok.kind},
        {"offset", prog.to_editor_offset(stok.span.offset)},
        {"length", stok.span.length},
        {"line", prog.editor_line(stok.span.offset)},
        {"col", loc.col},
    });
  }
}

auto has_error_severity(const std::vector<Diagnostic>& diags) -> bool {
  return std::ranges::any_of(
      diags, [](const auto& diag) -> bool { return diag.severity == Severity::Error; });
}

} // namespace

// ---------------------------------------------------------------------------
// Main handler
// ---------------------------------------------------------------------------

void handle_analyze(const httplib::Request& req, httplib::Response& res,
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
    res.set_content(R"({"error":"missing 'source' field"})", "application/json");
    return;
  }

  // --- Response accumulators ---
  nlohmann::json tokens = nlohmann::json::array();
  nlohmann::json diagnostics = nlohmann::json::array();
  nlohmann::json semantic_tokens_json = nlohmann::json::array();
  std::string ast_text;
  std::string hir_text;
  std::string mir_text;
  std::string llvm_ir_text;

  // --- Lex + parse (every file of the program) ---
  auto prog = build_playground_program(repo_root, request["source"].get<std::string>());
  if (prog.user == nullptr || !prog.program.diagnostics.empty()) {
    for (const auto& diag : prog.program.diagnostics) {
      diagnostics.push_back(make_internal_error(diag.message));
    }
    goto respond; // NOLINT(cppcoreguidelines-avoid-goto)
  }

  build_token_array(tokens, prog);
  for (const auto& file : prog.program.files) {
    collect_diagnostics(diagnostics, prog, file->lex.diagnostics);
  }
  if (!prog.user->lex.diagnostics.empty()) {
    goto respond; // NOLINT(cppcoreguidelines-avoid-goto)
  }

  {
    for (const auto& file : prog.program.files) {
      collect_diagnostics(diagnostics, prog, file->parse.diagnostics);
    }
    if (prog.user->file() == nullptr) {
      goto respond; // NOLINT(cppcoreguidelines-avoid-goto)
    }

    bool has_parse_errors = !prog.user->parse.diagnostics.empty();

    // Always emit the partial AST when a file was produced, even if
    // there are parse errors — error recovery nodes appear as
    // placeholders and the user can see the surviving structure.
    {
      std::ostringstream ast_out;
      print_ast(ast_out, *prog.user->file());
      ast_text = ast_out.str();
    }

    // --- Resolve ---
    // Continue even with parse errors — error recovery nodes are
    // tolerated by the resolver and produce partial results.
    auto resolve_result = resolve(prog.program);
    collect_diagnostics(diagnostics, prog, resolve_result.diagnostics);

    // Semantic tokens — always classify when lex/parse succeeded.
    auto sem_tokens =
        classify_tokens(prog.user->lex.tokens, prog.user->file(), &resolve_result);
    build_semantic_tokens(semantic_tokens_json, sem_tokens, prog);

    if (has_user_error(resolve_result.diagnostics, prog)) {
      goto respond; // NOLINT(cppcoreguidelines-avoid-goto)
    }

    // --- Typecheck ---
    // Continue even with parse errors — the type checker skips error
    // nodes silently, producing partial type information.
    TypeContext types;
    auto check_result = typecheck(prog.program, resolve_result, types);
    collect_diagnostics(diagnostics, prog, check_result.diagnostics);

    bool has_tc_errors =
        std::ranges::any_of(check_result.diagnostics, [&prog](const auto& diag) -> bool {
          return prog.in_user_file(diag.span.offset) && diag.severity == Severity::Error;
        });
    if (has_tc_errors) {
      goto respond; // NOLINT(cppcoreguidelines-avoid-goto)
    }

    // Stop lowering if parse had errors — partial ASTs with error
    // nodes are fine for resolve/typecheck/tooling but not for
    // HIR/MIR/LLVM lowering.
    if (has_parse_errors) {
      goto respond; // NOLINT(cppcoreguidelines-avoid-goto)
    }

    // --- HIR ---
    HirContext hir_ctx;
    auto hir_result = build_hir(prog.program, resolve_result, check_result, hir_ctx);
    collect_diagnostics(diagnostics, prog, hir_result.diagnostics);

    if (hir_result.module == nullptr) {
      if (!has_user_error(hir_result.diagnostics, prog)) {
        diagnostics.push_back(
            make_internal_error("HIR lowering failed (possible prelude error)"));
      }
      goto respond; // NOLINT(cppcoreguidelines-avoid-goto)
    }

    std::ostringstream hir_out;
    print_hir(hir_out, *hir_result.module);
    hir_text = hir_out.str();

    // --- MIR ---
    MirContext mir_ctx;
    auto mir_result = build_mir(*hir_result.module, mir_ctx, types);
    collect_diagnostics(diagnostics, prog, mir_result.diagnostics);

    if (mir_result.module == nullptr) {
      if (!has_user_error(mir_result.diagnostics, prog)) {
        diagnostics.push_back(
            make_internal_error("MIR lowering failed (possible prelude error)"));
      }
      goto respond; // NOLINT(cppcoreguidelines-avoid-goto)
    }

    auto mono_result = monomorphize(*mir_result.module, mir_ctx, types,
                                    mir_result.generic_templates);
    collect_diagnostics(diagnostics, prog, mono_result.diagnostics);

    std::ostringstream mir_out;
    print_mir(mir_out, *mir_result.module);
    mir_text = mir_out.str();

    // Stop before LLVM lowering on any mono error, including those
    // originating in the prelude.  MIR concreteness invariant
    // violations (Task 28 §14.2) are internal errors that must halt
    // unconditionally — the LLVM DataLayout assertion would fire all
    // the same.  MIR dump is still produced above for inspection.
    if (has_error_severity(mono_result.diagnostics)) {
      goto respond; // NOLINT(cppcoreguidelines-avoid-goto)
    }

    // --- LLVM IR ---
    llvm::LLVMContext llvm_ctx;
    LlvmBackend llvm_backend(llvm_ctx);
    auto llvm_result = llvm_backend.lower(*mir_result.module, &prog.program.source_map);
    collect_diagnostics(diagnostics, prog, llvm_result.diagnostics);

    if (llvm_result.module != nullptr &&
        !has_user_error(llvm_result.diagnostics, prog)) {
      std::ostringstream llvm_out;
      LlvmBackend::print_ir(llvm_out, *llvm_result.module);
      llvm_ir_text = llvm_out.str();
    }
  }

respond:
  nlohmann::json response = {
      {"tokens", tokens},
      {"semanticTokens", semantic_tokens_json},
      {"ast", ast_text},
      {"hir", hir_text},
      {"mir", mir_text},
      {"llvm_ir", llvm_ir_text},
      {"diagnostics", diagnostics},
  };

  res.set_content(response.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// Shared lightweight pipeline for hover/goto-def (lex → parse → resolve → typecheck)
// ---------------------------------------------------------------------------

namespace {

struct LightPipeline {
  PlaygroundProgram prog;
  ResolveResult resolve_result;
  TypeCheckResult check_result;
  TypeContext types;
  bool ok = false;
};

auto run_light_pipeline(const nlohmann::json& request,
                        const std::filesystem::path& repo_root)
    -> LightPipeline {
  LightPipeline pipe;
  pipe.prog = build_playground_program(repo_root, request["source"].get<std::string>());
  if (pipe.prog.user == nullptr || !pipe.prog.program.diagnostics.empty() ||
      !pipe.prog.program.lexed_and_parsed_cleanly()) {
    return pipe;
  }

  pipe.resolve_result = resolve(pipe.prog.program);
  pipe.check_result = typecheck(pipe.prog.program, pipe.resolve_result, pipe.types);
  pipe.ok = true;
  return pipe;
}

/// Find the token span offset that contains the given program offset.
/// Returns the token's span.offset, or the offset itself if no token found.
auto find_token_offset(uint32_t offset, const LexResult& lex_result)
    -> uint32_t {
  for (const auto& tok : lex_result.tokens) {
    if (tok.span.offset <= offset &&
        offset < tok.span.offset + tok.span.length) {
      return tok.span.offset;
    }
  }
  return offset;
}

} // namespace

// ---------------------------------------------------------------------------
// Hover
// ---------------------------------------------------------------------------

void handle_hover(const httplib::Request& req, httplib::Response& res,
                  const std::filesystem::path& repo_root) {
  nlohmann::json request;
  try {
    request = nlohmann::json::parse(req.body);
  } catch (const nlohmann::json::parse_error&) {
    res.status = 400;
    res.set_content(R"({"error":"invalid JSON"})", "application/json");
    return;
  }

  if (!request.contains("source") || !request.contains("offset")) {
    res.status = 400;
    res.set_content(R"({"error":"missing 'source' or 'offset'"})",
                    "application/json");
    return;
  }

  auto user_offset = request["offset"].get<uint32_t>();
  auto pipe = run_light_pipeline(request, repo_root);

  if (!pipe.ok) {
    res.set_content("null", "application/json");
    return;
  }

  // Find the token at this offset and use its span start for lookup.
  auto absolute_offset = pipe.prog.to_program_offset(user_offset);
  auto token_offset = find_token_offset(absolute_offset, pipe.prog.user->lex);

  auto result = dao::query_hover(token_offset, pipe.resolve_result,
                                 pipe.check_result);
  if (!result) {
    res.set_content("null", "application/json");
    return;
  }

  nlohmann::json response = {
      {"name", result->name},
      {"kind", result->symbol_kind},
      {"type", result->type},
  };
  res.set_content(response.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// Go to definition
// ---------------------------------------------------------------------------

void handle_goto_def(const httplib::Request& req, httplib::Response& res,
                     const std::filesystem::path& repo_root) {
  nlohmann::json request;
  try {
    request = nlohmann::json::parse(req.body);
  } catch (const nlohmann::json::parse_error&) {
    res.status = 400;
    res.set_content(R"({"error":"invalid JSON"})", "application/json");
    return;
  }

  if (!request.contains("source") || !request.contains("offset")) {
    res.status = 400;
    res.set_content(R"({"error":"missing 'source' or 'offset'"})",
                    "application/json");
    return;
  }

  auto user_offset = request["offset"].get<uint32_t>();
  auto pipe = run_light_pipeline(request, repo_root);

  if (!pipe.ok) {
    res.set_content("null", "application/json");
    return;
  }

  auto absolute_offset = pipe.prog.to_program_offset(user_offset);
  auto token_offset = find_token_offset(absolute_offset, pipe.prog.user->lex);

  auto result = dao::query_definition(token_offset, pipe.resolve_result);
  if (!result) {
    res.set_content("null", "application/json");
    return;
  }

  // A definition outside the editor buffer (prelude) is not navigable
  // in the user's source — return null.
  if (!pipe.prog.in_user_file(result->offset)) {
    res.set_content("null", "application/json");
    return;
  }

  auto loc = pipe.prog.program.source_map.locate(result->offset);
  nlohmann::json response = {
      {"offset", pipe.prog.to_editor_offset(result->offset)},
      {"length", result->length},
      {"line", pipe.prog.editor_line(result->offset)},
      {"col", loc.col},
  };
  res.set_content(response.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// Document symbols
// ---------------------------------------------------------------------------

void handle_document_symbols(const httplib::Request& req,
                              httplib::Response& res,
                              const std::filesystem::path& repo_root) {
  nlohmann::json request;
  try {
    request = nlohmann::json::parse(req.body);
  } catch (const nlohmann::json::parse_error&) {
    res.status = 400;
    res.set_content(R"({"error":"invalid JSON"})", "application/json");
    return;
  }

  if (!request.contains("source")) {
    res.status = 400;
    res.set_content(R"({"error":"missing 'source'"})", "application/json");
    return;
  }

  auto pipe = run_light_pipeline(request, repo_root);

  if (!pipe.ok || pipe.prog.user->file() == nullptr) {
    res.set_content("[]", "application/json");
    return;
  }

  auto symbols = dao::query_document_symbols(*pipe.prog.user->file());

  // Build JSON response.
  std::function<nlohmann::json(const dao::DocumentSymbol&)> to_json;
  to_json = [&](const dao::DocumentSymbol& sym) -> nlohmann::json {
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

  nlohmann::json response = nlohmann::json::array();
  for (const auto& sym : symbols) {
    response.push_back(to_json(sym));
  }
  res.set_content(response.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

void handle_references(const httplib::Request& req, httplib::Response& res,
                        const std::filesystem::path& repo_root) {
  nlohmann::json request;
  try {
    request = nlohmann::json::parse(req.body);
  } catch (const nlohmann::json::parse_error&) {
    res.status = 400;
    res.set_content(R"({"error":"invalid JSON"})", "application/json");
    return;
  }

  if (!request.contains("source") || !request.contains("offset")) {
    res.status = 400;
    res.set_content(R"({"error":"missing 'source' or 'offset'"})",
                    "application/json");
    return;
  }

  auto user_offset = request["offset"].get<uint32_t>();
  auto pipe = run_light_pipeline(request, repo_root);

  if (!pipe.ok) {
    res.set_content("[]", "application/json");
    return;
  }

  auto absolute_offset = pipe.prog.to_program_offset(user_offset);
  auto token_offset = find_token_offset(absolute_offset, pipe.prog.user->lex);

  auto results = dao::query_references(token_offset, pipe.resolve_result);

  nlohmann::json response = nlohmann::json::array();
  for (const auto& ref : results) {
    // Skip references outside the editor buffer (prelude).
    if (!pipe.prog.in_user_file(ref.span.offset)) {
      continue;
    }
    response.push_back({
        {"offset", pipe.prog.to_editor_offset(ref.span.offset)},
        {"length", ref.span.length},
        {"isDefinition", ref.is_definition},
    });
  }
  res.set_content(response.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// Completions
// ---------------------------------------------------------------------------

/// Scan backward from a buffer-local offset to find the non-whitespace
/// character.  Returns the position (1-indexed into contents), or 0 if
/// none found.
auto scan_back_past_whitespace(std::string_view contents, uint32_t offset)
    -> uint32_t {
  auto pos = offset;
  while (pos > 0 && (contents[pos - 1] == ' ' || contents[pos - 1] == '\t')) {
    --pos;
  }
  return pos;
}

/// Resolve the semantic type of a symbol from typed results.
/// Handles locals, params, functions, and types.
auto resolve_receiver_type(const dao::Symbol* sym,
                            const dao::TypeCheckResult& typed)
    -> const dao::Type* {
  if (sym == nullptr || sym->decl == nullptr) {
    return nullptr;
  }
  switch (sym->kind) {
  case dao::SymbolKind::Local:
    return typed.typed.local_type(sym->decl_as_stmt());
  case dao::SymbolKind::Param: {
    const auto* fn_decl = sym->decl_as_decl();
    const auto* fn_type = typed.typed.decl_type(fn_decl);
    if (fn_type == nullptr || fn_type->kind() != dao::TypeKind::Function) {
      return nullptr;
    }
    const auto* func = static_cast<const dao::TypeFunction*>(fn_type);
    if (!fn_decl->is<dao::FunctionDecl>()) {
      return nullptr;
    }
    const auto& decl = fn_decl->as<dao::FunctionDecl>();
    for (size_t idx = 0; idx < decl.params.size(); ++idx) {
      if (decl.params[idx].name == sym->name &&
          idx < func->param_types().size()) {
        return func->param_types()[idx];
      }
    }
    return nullptr;
  }
  default:
    return nullptr;
  }
}

/// Try to find the type of the expression ending just before dot_pos
/// (a program offset) by scanning the typed expression map. This
/// handles general expressions like make_point()., p.x., and arr[i].
/// where the receiver is not a simple identifier in the symbol table.
auto find_expr_type_before_dot(uint32_t dot_pos,
                                 const dao::TypeCheckResult& typed)
    -> const dao::Type* {
  const dao::Type* best = nullptr;
  uint32_t best_length = 0;

  for (const auto& [expr, type] : typed.typed.expr_types()) {
    auto end = expr->span.offset + expr->span.length;
    if (end == dot_pos && type != nullptr) {
      // If multiple expressions end at the same position, prefer the
      // outermost one (largest span) — it's the complete sub-expression.
      if (best == nullptr || expr->span.length > best_length) {
        best = type;
        best_length = expr->span.length;
      }
    }
  }

  return best;
}

/// Try to detect and resolve dot completion at the given program offset.
/// Returns the receiver type if a dot trigger is found, or nullptr.
auto try_resolve_dot_receiver(uint32_t absolute_offset,
                               const LightPipeline& pipe)
    -> const dao::Type* {
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

  // Fast path: simple identifier receiver (locals, params).
  auto pre_dot = scan_back_past_whitespace(contents, pos - 1);
  auto token_off = find_token_offset(
      user.base_offset + (pre_dot > 0 ? pre_dot - 1 : 0), user.lex);

  auto use_it = pipe.resolve_result.uses.find(token_off);
  if (use_it != pipe.resolve_result.uses.end()) {
    const auto* sym_type =
        resolve_receiver_type(use_it->second, pipe.check_result);
    if (sym_type != nullptr) {
      return sym_type;
    }
  }

  // Slow path: scan typed expressions for one ending at the dot.
  // Handles call expressions, field chains, index expressions, etc.
  auto dot_pos = user.base_offset + (pos - 1);
  return find_expr_type_before_dot(dot_pos, pipe.check_result);
}

void handle_completions(const httplib::Request& req, httplib::Response& res,
                         const std::filesystem::path& repo_root) {
  nlohmann::json request;
  try {
    request = nlohmann::json::parse(req.body);
  } catch (const nlohmann::json::parse_error&) {
    res.status = 400;
    res.set_content(R"({"error":"invalid JSON"})", "application/json");
    return;
  }

  if (!request.contains("source") || !request.contains("offset")) {
    res.status = 400;
    res.set_content(R"({"error":"missing 'source' or 'offset'"})",
                    "application/json");
    return;
  }

  auto user_offset = request["offset"].get<uint32_t>();
  auto pipe = run_light_pipeline(request, repo_root);

  if (!pipe.ok) {
    res.set_content("[]", "application/json");
    return;
  }

  auto absolute_offset = pipe.prog.to_program_offset(user_offset);
  std::vector<dao::CompletionItem> items;

  const auto* receiver = try_resolve_dot_receiver(absolute_offset, pipe);
  if (receiver != nullptr) {
    items = dao::query_dot_completions(receiver, pipe.check_result);
  } else {
    items = dao::query_completions(absolute_offset, pipe.resolve_result,
                                    pipe.check_result);
  }

  nlohmann::json response = nlohmann::json::array();
  for (const auto& item : items) {
    response.push_back({
        {"label", item.label},
        {"kind", item.kind},
        {"type", item.type},
    });
  }
  res.set_content(response.dump(), "application/json");
}

// NOLINTEND(readability-magic-numbers)

} // namespace dao::playground
