#ifndef DAO_PLAYGROUND_SERVICE_SURFACE_H
#define DAO_PLAYGROUND_SERVICE_SURFACE_H

#include "analysis/tooling_surface.h"

#include <array>
#include <span>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Service surface — the playground transport, owned by the playground.
//
// The compiler owns semantic truth (`compiler/analysis/tooling_surface.h`:
// token kinds, lexical categories, severities, and the payload shapes of
// the analysis results).  This file owns how the playground carries it:
// the request envelopes, the service's own response shapes, the routes
// with their methods and paths, and the capability matrix that records
// which surface serves what (CONTRACT_COMPILER_PHASES.md "Analysis
// Responsibilities"; CONTRACT_LANGUAGE_TOOLING.md "Shared Analysis
// Ownership": tooling adapts transport, never semantic truth).
//
// `service_surface_dump` renders the TypeScript the frontend compiles
// against and the Markdown capability matrix; `service_test` asserts
// both checked-in files equal the render, that every route is bound,
// and that every reply matches its declared shape.
// ---------------------------------------------------------------------------

namespace dao::playground {

using tooling::FieldSpec;
using tooling::ShapeSpec;

// --- Request envelopes ------------------------------------------------------
//
// Requests are program-shaped: the files of the program and which of
// them is the document the request is about.  A single-document UI
// sends one file and names it; a workspace sends them all.  `offset`
// is local to the document.  Replies name the file of every position
// they carry, so a definition in another file of the program is
// reported, not hidden.

inline constexpr std::array kSourceInputFields{
    FieldSpec{"path", "string"},
    FieldSpec{"source", "string"},
};

inline constexpr std::array kProgramRequestFields{
    FieldSpec{"files", "SourceInput[]"},
    FieldSpec{"document", "string"},
};

inline constexpr std::array kAnalyzeRequestFields{
    FieldSpec{"files", "SourceInput[]"},
    FieldSpec{"document", "string"},
    FieldSpec{"includePrelude", "boolean", /*optional=*/true},
};

inline constexpr std::array kPositionRequestFields{
    FieldSpec{"files", "SourceInput[]"},
    FieldSpec{"document", "string"},
    FieldSpec{"offset", "number"},
};

inline constexpr std::array kExampleNameFields{
    FieldSpec{"name", "string"},
};

// --- Service responses (the analysis payloads they carry are the compiler's) ---
//
// `file` is the document's path as the request named it; `module` is
// the module the document declares (or the synthetic one it was given).

inline constexpr std::array kAnalyzeResponseFields{
    FieldSpec{"file", "string"},
    FieldSpec{"module", "string"},
    FieldSpec{"tokens", "LexToken[]"},
    FieldSpec{"semanticTokens", "SemanticToken[]"},
    FieldSpec{"ast", "string"},
    FieldSpec{"hir", "string"},
    FieldSpec{"mir", "string"},
    FieldSpec{"llvm_ir", "string"},
    FieldSpec{"diagnostics", "Diagnostic[]"},
};

inline constexpr std::array kRunResponseFields{
    FieldSpec{"file", "string"},
    FieldSpec{"stdout", "string"},
    FieldSpec{"stderr", "string"},
    FieldSpec{"exit_code", "number"},
    FieldSpec{"diagnostics", "Diagnostic[]"},
};

inline constexpr std::array kExampleEntryFields{
    FieldSpec{"name", "string"},
};

inline constexpr std::array kExamplesListFields{
    FieldSpec{"examples", "ExampleEntry[]"},
};

inline constexpr std::array kExampleSourceFields{
    FieldSpec{"name", "string"},
    FieldSpec{"source", "string"},
};

inline constexpr std::array kErrorReplyFields{
    FieldSpec{"error", "string"},
};

inline constexpr std::array kServiceShapes{
    ShapeSpec{"SourceInput", kSourceInputFields},
    ShapeSpec{"ProgramRequest", kProgramRequestFields},
    ShapeSpec{"AnalyzeRequest", kAnalyzeRequestFields},
    ShapeSpec{"PositionRequest", kPositionRequestFields},
    ShapeSpec{"ExampleName", kExampleNameFields},
    ShapeSpec{"AnalyzeResponse", kAnalyzeResponseFields},
    ShapeSpec{"RunResponse", kRunResponseFields},
    ShapeSpec{"ExampleEntry", kExampleEntryFields},
    ShapeSpec{"ExamplesList", kExamplesListFields},
    ShapeSpec{"ExampleSource", kExampleSourceFields},
    ShapeSpec{"ErrorReply", kErrorReplyFields},
};

/// A service shape, else an analysis shape, else null.
auto find_service_shape(std::string_view name) -> const ShapeSpec*;

// --- Routes -----------------------------------------------------------------

/// `request` names the shape a POST body must satisfy (`void` for a GET
/// without a body; `ExampleName` for the path parameter of
/// `/api/examples/:name`).  `response` is a TypeScript type expression
/// over the shapes: a shape, `Shape[]`, or `Shape | null`.
struct RouteSpec {
  std::string_view name;
  std::string_view method;
  std::string_view path;
  std::string_view request;
  std::string_view response;
};

inline constexpr std::array kRoutes{
    RouteSpec{"analyze", "POST", "/api/analyze", "AnalyzeRequest", "AnalyzeResponse"},
    RouteSpec{"run", "POST", "/api/run", "ProgramRequest", "RunResponse"},
    RouteSpec{"hover", "POST", "/api/hover", "PositionRequest", "Hover | null"},
    RouteSpec{"gotoDef", "POST", "/api/goto-def", "PositionRequest", "Definition | null"},
    RouteSpec{
        "documentSymbols", "POST", "/api/document-symbols", "ProgramRequest", "DocumentSymbol[]"},
    RouteSpec{"references", "POST", "/api/references", "PositionRequest", "Reference[]"},
    RouteSpec{"completions", "POST", "/api/completions", "PositionRequest", "Completion[]"},
    RouteSpec{"examples", "GET", "/api/examples", "void", "ExamplesList"},
    RouteSpec{"example", "GET", "/api/examples/:name", "ExampleName", "ExampleSource"},
};

auto find_route(std::string_view name) -> const RouteSpec*;

// --- Capabilities -----------------------------------------------------------
//
// The status of record for what tooling can do: one row per capability
// with the compiler entry points behind it and the playground route,
// `daoc` command, and LSP method that expose it (empty when none does).
// Rendered as docs/tooling_capabilities.md.  `service_test` checks that
// every entry point is declared in the header it names, every route is
// in `kRoutes`, every command is in the driver, and LSP methods are
// well-formed and unique.

/// A compiler function and the header that declares it.
struct EntryPoint {
  std::string_view symbol;
  std::string_view header; // path under compiler/
};

struct CapabilitySpec {
  std::string_view name;
  std::span<const EntryPoint> analysis;
  std::string_view playground; // a kRoutes name, or ""
  std::string_view cli;        // a `daoc` subcommand, or ""
  std::string_view lsp;        // the LSP method it maps to, or ""
};

// Diagnostics come from every phase that can reject a program: each
// header under compiler/ that declares a `diagnostics` vector must be
// here (service_test walks them), so an omitted producer fails the test.
inline constexpr std::array kDiagnosticEntryPoints{
    EntryPoint{"lex", "frontend/lexer/lexer.h"},
    EntryPoint{"parse", "frontend/parser/parser.h"},
    EntryPoint{"build_program", "frontend/module/program.h"},
    EntryPoint{"resolve", "frontend/resolve/resolve.h"},
    EntryPoint{"typecheck", "frontend/typecheck/type_checker.h"},
    EntryPoint{"build_hir", "ir/hir/hir_builder.h"},
    EntryPoint{"build_mir", "ir/mir/mir_builder.h"},
    EntryPoint{"monomorphize", "ir/mir/mir_monomorphize.h"},
    EntryPoint{"LlvmBackend::lower", "backend/llvm/llvm_backend.h"},
};
inline constexpr std::array kSemanticTokenEntryPoints{
    EntryPoint{"classify_tokens", "analysis/semantic_tokens.h"},
};
inline constexpr std::array kHoverEntryPoints{
    EntryPoint{"query_hover", "analysis/hover.h"},
};
inline constexpr std::array kDefinitionEntryPoints{
    EntryPoint{"query_definition", "analysis/goto_definition.h"},
};
inline constexpr std::array kReferencesEntryPoints{
    EntryPoint{"query_references", "analysis/references.h"},
};
inline constexpr std::array kDocumentSymbolEntryPoints{
    EntryPoint{"query_document_symbols", "analysis/document_symbols.h"},
};
inline constexpr std::array kCompletionEntryPoints{
    EntryPoint{"query_completions", "analysis/completion.h"},
};
inline constexpr std::array kAstDumpEntryPoints{
    EntryPoint{"print_ast", "frontend/ast/ast_printer.h"},
};
inline constexpr std::array kHirDumpEntryPoints{
    EntryPoint{"print_hir", "ir/hir/hir_printer.h"},
};
inline constexpr std::array kMirDumpEntryPoints{
    EntryPoint{"print_mir", "ir/mir/mir_printer.h"},
};
inline constexpr std::array kLlvmDumpEntryPoints{
    EntryPoint{"LlvmBackend::print_ir", "backend/llvm/llvm_backend.h"},
};
inline constexpr std::array kNativeBuildEntryPoints{
    EntryPoint{"LlvmBackend::emit_object", "backend/llvm/llvm_backend.h"},
};

inline constexpr std::array kCapabilities{
    CapabilitySpec{"Diagnostics",
                   kDiagnosticEntryPoints,
                   "analyze",
                   "check",
                   "textDocument/publishDiagnostics"},
    CapabilitySpec{"Semantic tokens",
                   kSemanticTokenEntryPoints,
                   "analyze",
                   "tokens",
                   "textDocument/semanticTokens/full"},
    CapabilitySpec{"Hover", kHoverEntryPoints, "hover", "", "textDocument/hover"},
    CapabilitySpec{
        "Go to definition", kDefinitionEntryPoints, "gotoDef", "", "textDocument/definition"},
    CapabilitySpec{
        "References", kReferencesEntryPoints, "references", "", "textDocument/references"},
    CapabilitySpec{"Document symbols",
                   kDocumentSymbolEntryPoints,
                   "documentSymbols",
                   "",
                   "textDocument/documentSymbol"},
    CapabilitySpec{
        "Completion", kCompletionEntryPoints, "completions", "", "textDocument/completion"},
    CapabilitySpec{"AST dump", kAstDumpEntryPoints, "analyze", "ast", ""},
    CapabilitySpec{"HIR dump", kHirDumpEntryPoints, "analyze", "hir", ""},
    CapabilitySpec{"MIR dump", kMirDumpEntryPoints, "analyze", "mir", ""},
    CapabilitySpec{"LLVM IR dump", kLlvmDumpEntryPoints, "analyze", "llvm-ir", ""},
    CapabilitySpec{"Native build", kNativeBuildEntryPoints, "", "build", ""},
    // Linking against the runtime and executing is the service's own
    // work (run.cpp), not a compiler entry point.
    CapabilitySpec{"Build and run", kNativeBuildEntryPoints, "run", "", ""},
};

/// The TypeScript module the frontend compiles against: the compiler's
/// analysis surface followed by this service's envelopes and routes.
/// `service_surface_dump` writes it; `service_test` asserts the
/// checked-in file equals it.
auto render_service_typescript() -> std::string;

/// The capability matrix as Markdown (docs/tooling_capabilities.md);
/// same generate-and-verify rule as the TypeScript.
auto render_capability_matrix() -> std::string;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_SERVICE_SURFACE_H
