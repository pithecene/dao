#ifndef DAO_PLAYGROUND_SERVICE_SURFACE_H
#define DAO_PLAYGROUND_SERVICE_SURFACE_H

#include "analysis/tooling_surface.h"

#include <array>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Service surface — the playground transport, owned by the playground.
//
// The compiler owns semantic truth (`compiler/analysis/tooling_surface.h`:
// token kinds, lexical categories, severities, and the payload shapes of
// the analysis results).  This file owns how the playground carries it:
// the request envelopes, the service's own response shapes, and the
// routes with their methods and paths (CONTRACT_COMPILER_PHASES.md
// "Analysis Responsibilities"; CONTRACT_LANGUAGE_TOOLING.md "Shared
// Analysis Ownership": tooling adapts transport, never semantic truth).
//
// `service_surface_dump` renders both tables into the frontend's
// generated TypeScript; `service_test` asserts the checked-in file
// equals the render, that every route is bound, and that every reply
// matches its declared shape.
// ---------------------------------------------------------------------------

namespace dao::playground {

using tooling::FieldSpec;
using tooling::ShapeSpec;

// --- Request envelopes ------------------------------------------------------

inline constexpr std::array kSourceRequestFields{
    FieldSpec{"source", "string"},
};

inline constexpr std::array kAnalyzeRequestFields{
    FieldSpec{"source", "string"},
    FieldSpec{"includePrelude", "boolean", /*optional=*/true},
};

inline constexpr std::array kPositionRequestFields{
    FieldSpec{"source", "string"},
    FieldSpec{"offset", "number"},
};

inline constexpr std::array kExampleNameFields{
    FieldSpec{"name", "string"},
};

// --- Service responses (the analysis payloads they carry are the compiler's) ---

inline constexpr std::array kAnalyzeResponseFields{
    FieldSpec{"tokens", "LexToken[]"},
    FieldSpec{"semanticTokens", "SemanticToken[]"},
    FieldSpec{"ast", "string"},
    FieldSpec{"hir", "string"},
    FieldSpec{"mir", "string"},
    FieldSpec{"llvm_ir", "string"},
    FieldSpec{"diagnostics", "Diagnostic[]"},
};

inline constexpr std::array kRunResponseFields{
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
    ShapeSpec{"SourceRequest", kSourceRequestFields},
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
    RouteSpec{"run", "POST", "/api/run", "SourceRequest", "RunResponse"},
    RouteSpec{"hover", "POST", "/api/hover", "PositionRequest", "Hover | null"},
    RouteSpec{"gotoDef", "POST", "/api/goto-def", "PositionRequest", "Definition | null"},
    RouteSpec{
        "documentSymbols", "POST", "/api/document-symbols", "SourceRequest", "DocumentSymbol[]"},
    RouteSpec{"references", "POST", "/api/references", "PositionRequest", "Reference[]"},
    RouteSpec{"completions", "POST", "/api/completions", "PositionRequest", "Completion[]"},
    RouteSpec{"examples", "GET", "/api/examples", "void", "ExamplesList"},
    RouteSpec{"example", "GET", "/api/examples/:name", "ExampleName", "ExampleSource"},
};

auto find_route(std::string_view name) -> const RouteSpec*;

/// The TypeScript module the frontend compiles against: the compiler's
/// analysis surface followed by this service's envelopes and routes.
/// `service_surface_dump` writes it; `service_test` asserts the
/// checked-in file equals it.
auto render_service_typescript() -> std::string;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_SERVICE_SURFACE_H
