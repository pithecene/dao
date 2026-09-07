#include "analyze.h"
#include "pipeline.h"
#include "token_category.h"

#include "analysis/semantic_tokens.h"
#include "backend/llvm/llvm_backend.h"
#include "frontend/ast/ast_printer.h"
#include "ir/hir/hir_builder.h"
#include "ir/hir/hir_context.h"
#include "ir/hir/hir_printer.h"
#include "ir/mir/mir_builder.h"
#include "ir/mir/mir_context.h"
#include "ir/mir/mir_monomorphize.h"
#include "ir/mir/mir_printer.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <string>

namespace dao::playground {

namespace {

// ---------------------------------------------------------------------------
// Response accumulator.  Every phase appends to it; the reply is built
// from whatever was reached when the pipeline stopped.
// ---------------------------------------------------------------------------

struct AnalyzeOutput {
  nlohmann::json tokens = nlohmann::json::array();
  nlohmann::json semantic_tokens = nlohmann::json::array();
  nlohmann::json diagnostics = nlohmann::json::array();
  std::string file;   // the document's path, as the request named it
  std::string module; // the document's module name, once parsed
  std::string ast;
  std::string hir;
  std::string mir;
  std::string llvm_ir;

  [[nodiscard]] auto reply() const -> Reply {
    return {.status = http_status::ok,
            .body = {
                {"file", file},
                {"module", module},
                {"tokens", tokens},
                {"semanticTokens", semantic_tokens},
                {"ast", ast},
                {"hir", hir},
                {"mir", mir},
                {"llvm_ir", llvm_ir},
                {"diagnostics", diagnostics},
            }};
  }
};

// ---------------------------------------------------------------------------
// Token serialization (editor buffer only; prelude files are not serialized)
// ---------------------------------------------------------------------------

void add_lexical_tokens(AnalyzeOutput& out, const PlaygroundProgram& prog) {
  for (const auto& tok : prog.user->lex.tokens) {
    if (is_synthetic_token(tok.kind) || !prog.in_editor_text(tok.span.offset)) {
      continue;
    }
    auto loc = prog.program.source_map.locate(tok.span.offset);
    out.tokens.push_back({
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

void add_semantic_tokens(AnalyzeOutput& out,
                         const std::vector<SemanticToken>& sem_tokens,
                         const PlaygroundProgram& prog) {
  for (const auto& stok : sem_tokens) {
    if (!prog.in_editor_text(stok.span.offset)) {
      continue;
    }
    auto loc = prog.program.source_map.locate(stok.span.offset);
    out.semantic_tokens.push_back({
        {"kind", stok.kind},
        {"offset", prog.to_editor_offset(stok.span.offset)},
        {"length", stok.span.length},
        {"line", prog.editor_line(stok.span.offset)},
        {"col", loc.col},
    });
  }
}

// ---------------------------------------------------------------------------
// IR views restricted to the editor buffer's declarations.  Monomorphized
// instantiations keep their template's span, so a user generic's
// instantiations stay visible and prelude instantiations stay hidden.
// ---------------------------------------------------------------------------

auto user_declarations(const HirModule& module, const PlaygroundProgram& prog) -> HirModule {
  HirModule view{.span = module.span, .declarations = {}};
  std::ranges::copy_if(
      module.declarations,
      std::back_inserter(view.declarations),
      [&prog](const HirDecl* decl) -> bool { return prog.in_user_file(decl->span.offset); });
  return view;
}

auto user_functions(const MirModule& module, const PlaygroundProgram& prog) -> MirModule {
  MirModule view{.functions = {}, .span = module.span};
  std::ranges::copy_if(
      module.functions, std::back_inserter(view.functions), [&prog](const MirFunction* fn) -> bool {
        return prog.in_user_file(fn->span.offset);
      });
  return view;
}

/// Predicate over LLVM function names selecting the editor buffer's
/// functions.  LLVM names are the MIR symbol names; a generator's resume
/// function carries a `.resume` suffix.
auto user_function_filter(const MirModule& user_mir) -> std::function<bool(std::string_view)> {
  std::set<std::string, std::less<>> names;
  for (const auto* fn : user_mir.functions) {
    names.emplace(fn->symbol->name);
  }
  return [names = std::move(names)](std::string_view name) -> bool {
    constexpr std::string_view resume_suffix = ".resume";
    if (name.ends_with(resume_suffix)) {
      name.remove_suffix(resume_suffix.size());
    }
    return names.contains(name);
  };
}

/// `a::b` for the buffer's module declaration, or "" when it has none.
auto module_name(const FileNode& file) -> std::string {
  std::string name;
  if (file.module_decl == nullptr) {
    return name;
  }
  for (auto segment : file.module_decl->path.segments) {
    if (!name.empty()) {
      name += "::";
    }
    name += segment;
  }
  return name;
}

template <typename Printable> auto printed(Printable&& print) -> std::string {
  std::ostringstream out;
  print(out);
  return out.str();
}

} // namespace

// ---------------------------------------------------------------------------
// Route
// ---------------------------------------------------------------------------

auto analyze(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  AnalyzeOutput out;
  const bool include_prelude = request.value("includePrelude", false);

  // --- Lex + parse (every file of the program) ---
  auto inputs = parse_program_request(request);
  if (!inputs) {
    return error_reply(http_status::bad_request, inputs.error());
  }
  out.file = inputs->document;
  // Advisory: a buffer with no `fn main` is analysable, and the warning
  // is why Run will not work.
  auto prog = build_playground_program(ctx.repo_root, std::move(*inputs), EntryPolicy::Advisory);
  if (prog.user == nullptr || has_error_severity(prog.program.diagnostics)) {
    // Report what each file said first: a module that is "not found" is
    // usually a file that did not parse, and that parse error is the
    // diagnostic worth showing.
    for (const auto& file : prog.program.files) {
      collect_diagnostics(out.diagnostics, prog, file->lex.diagnostics);
      collect_diagnostics(out.diagnostics, prog, file->parse.diagnostics);
    }
    collect_program_diagnostics(out.diagnostics, prog);
    return out.reply();
  }
  // Assembly succeeded but may still have something to say (no entry
  // module, for one), and that must not be lost.
  collect_program_diagnostics(out.diagnostics, prog);

  add_lexical_tokens(out, prog);
  for (const auto& file : prog.program.files) {
    collect_diagnostics(out.diagnostics, prog, file->lex.diagnostics);
  }
  if (!prog.user->lex.diagnostics.empty()) {
    return out.reply();
  }

  for (const auto& file : prog.program.files) {
    collect_diagnostics(out.diagnostics, prog, file->parse.diagnostics);
  }
  if (prog.user->file() == nullptr) {
    return out.reply();
  }
  // Lowering needs every file lexed and parsed cleanly; the document's
  // own parse errors are tolerated up to here so its AST, tokens, and
  // partial resolution still answer.
  const bool has_parse_errors = !prog.program.lexed_and_parsed_cleanly();
  out.module = module_name(*prog.user->file());

  // Always emit the partial AST when a file was produced, even with
  // parse errors — error recovery nodes appear as placeholders and the
  // user can see the surviving structure.
  out.ast = printed([&](std::ostream& os) { print_ast(os, *prog.user->file()); });

  // --- Resolve ---
  // Continues past parse errors: the resolver tolerates error recovery
  // nodes and produces partial results.
  auto resolve_result = resolve(prog.program);
  collect_diagnostics(out.diagnostics, prog, resolve_result.diagnostics);

  // Semantic tokens — always classified once lex/parse produced a file.
  add_semantic_tokens(
      out, classify_tokens(prog.user->lex.tokens, prog.user->file(), &resolve_result), prog);

  if (has_error_severity(resolve_result.diagnostics)) {
    return out.reply();
  }

  // --- Typecheck ---
  // Continues past parse errors: the type checker skips error nodes
  // silently, producing partial type information.
  TypeContext types;
  auto check_result = typecheck(prog.program, resolve_result, types);
  collect_diagnostics(out.diagnostics, prog, check_result.diagnostics);
  if (has_error_severity(check_result.diagnostics)) {
    return out.reply();
  }

  // Partial ASTs with error nodes are fine for resolve/typecheck/tooling
  // but not for lowering.
  if (has_parse_errors) {
    return out.reply();
  }

  // --- HIR ---
  HirContext hir_ctx;
  auto hir_result = build_hir(prog.program, resolve_result, check_result, hir_ctx);
  collect_diagnostics(out.diagnostics, prog, hir_result.diagnostics);
  // A builder may hand back a module alongside error diagnostics; that
  // module is not lowered further.
  if (hir_result.module == nullptr || has_error_severity(hir_result.diagnostics)) {
    if (!has_error_severity(hir_result.diagnostics)) {
      out.diagnostics.push_back(
          make_unlocated_diagnostic("HIR lowering failed without a diagnostic"));
    }
    return out.reply();
  }
  out.hir = printed([&](std::ostream& os) {
    print_hir(os,
              include_prelude ? *hir_result.module : user_declarations(*hir_result.module, prog));
  });

  // --- MIR ---
  MirContext mir_ctx;
  auto mir_result = build_mir(*hir_result.module, mir_ctx, types);
  collect_diagnostics(out.diagnostics, prog, mir_result.diagnostics);
  if (mir_result.module == nullptr || has_error_severity(mir_result.diagnostics)) {
    if (!has_error_severity(mir_result.diagnostics)) {
      out.diagnostics.push_back(
          make_unlocated_diagnostic("MIR lowering failed without a diagnostic"));
    }
    return out.reply();
  }

  auto mono_result = monomorphize(*mir_result.module, mir_ctx, types, mir_result.generic_templates);
  collect_diagnostics(out.diagnostics, prog, mono_result.diagnostics);

  auto user_mir = user_functions(*mir_result.module, prog);
  out.mir = printed(
      [&](std::ostream& os) { print_mir(os, include_prelude ? *mir_result.module : user_mir); });

  // Stop before LLVM lowering on any monomorphization error, including
  // prelude-origin ones: a MIR concreteness violation is an internal
  // error that would otherwise surface as an LLVM DataLayout assertion.
  // The MIR dump above is still available for inspection.
  if (has_error_severity(mono_result.diagnostics)) {
    return out.reply();
  }

  // --- LLVM IR ---
  llvm::LLVMContext llvm_ctx;
  LlvmBackend llvm_backend(llvm_ctx);
  auto llvm_result = llvm_backend.lower(*mir_result.module, &prog.program.source_map);
  collect_diagnostics(
      out.diagnostics, prog, without_prelude_warnings(llvm_result.diagnostics, prog));

  if (llvm_result.module != nullptr && !has_error_severity(llvm_result.diagnostics)) {
    out.llvm_ir = printed([&](std::ostream& os) {
      if (include_prelude) {
        LlvmBackend::print_ir(os, *llvm_result.module);
      } else {
        LlvmBackend::print_ir(os, *llvm_result.module, user_function_filter(user_mir));
      }
    });
  }
  return out.reply();
}

} // namespace dao::playground
