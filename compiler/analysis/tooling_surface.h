#ifndef DAO_ANALYSIS_TOOLING_SURFACE_H
#define DAO_ANALYSIS_TOOLING_SURFACE_H

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Tooling surface — the single source of truth for what the compiler
// exposes to editors: semantic token kinds, lexical categories,
// diagnostic severities, JSON payload shapes, and service routes.
//
// Everything downstream derives from these tables instead of restating
// them:
//   - the playground frontend's TypeScript types are generated from
//     them (`tooling_surface_dump`, checked in under
//     tools/playground/frontend/src/generated/)
//   - the compiler service registers routes from them and validates
//     request bodies against the request shapes
//   - tests assert the token kinds equal the taxonomy listed in
//     CONTRACT_LANGUAGE_TOOLING.md, that every kind the emitter produces
//     over the corpus is listed here, that every route dispatches, and
//     that every response matches its declared shape
//
// Adding a kind, field, or route means adding it here; the tests fail
// until the contract and the generated TypeScript agree.
// ---------------------------------------------------------------------------

namespace dao::tooling {

// ---------------------------------------------------------------------------
// Semantic token kinds and their visual groups.
//
// The group names a colour role; the frontend maps a group to the CSS
// class `dao-<group>` verbatim, so a new group needs a stylesheet rule.
// ---------------------------------------------------------------------------

struct TokenKindSpec {
  std::string_view kind;
  std::string_view group;
};

inline constexpr std::array kTokenGroups{
    std::string_view{"keyword"},        std::string_view{"mode"},
    std::string_view{"resource"},       std::string_view{"type"},
    std::string_view{"decl"},           std::string_view{"variable"},
    std::string_view{"field"},          std::string_view{"module"},
    std::string_view{"lambda-param"},   std::string_view{"literal-number"},
    std::string_view{"literal-string"}, std::string_view{"operator"},
    std::string_view{"punctuation"},
};

// Order follows CONTRACT_LANGUAGE_TOOLING.md: the initial freeze, then
// the expansions since.
inline constexpr std::array kTokenKinds{
    // Keywords and control
    TokenKindSpec{"keyword.module", "keyword"},
    TokenKindSpec{"keyword.import", "keyword"},
    TokenKindSpec{"keyword.type", "keyword"},
    TokenKindSpec{"keyword.extern", "keyword"},
    TokenKindSpec{"keyword.fn", "keyword"},
    TokenKindSpec{"keyword.let", "keyword"},
    TokenKindSpec{"keyword.return", "keyword"},
    TokenKindSpec{"keyword.if", "keyword"},
    TokenKindSpec{"keyword.else", "keyword"},
    TokenKindSpec{"keyword.while", "keyword"},
    TokenKindSpec{"keyword.for", "keyword"},
    TokenKindSpec{"keyword.in", "keyword"},
    // Execution / resource constructs
    TokenKindSpec{"keyword.mode", "keyword"},
    TokenKindSpec{"keyword.resource", "keyword"},
    TokenKindSpec{"mode.unsafe", "mode"},
    TokenKindSpec{"mode.gpu", "mode"},
    TokenKindSpec{"mode.parallel", "mode"},
    TokenKindSpec{"resource.kind.memory", "resource"},
    TokenKindSpec{"resource.binding", "resource"},
    // Types and declarations
    TokenKindSpec{"type.builtin", "type"},
    TokenKindSpec{"type.nominal", "type"},
    TokenKindSpec{"decl.function", "decl"},
    TokenKindSpec{"decl.type", "decl"},
    TokenKindSpec{"decl.field", "field"},
    TokenKindSpec{"decl.module", "module"},
    // Symbol uses
    TokenKindSpec{"use.function", "decl"},
    TokenKindSpec{"use.variable.local", "variable"},
    TokenKindSpec{"use.variable.param", "variable"},
    TokenKindSpec{"use.field", "field"},
    TokenKindSpec{"use.module", "module"},
    // Literals and operators
    TokenKindSpec{"literal.string", "literal-string"},
    TokenKindSpec{"literal.number", "literal-number"},
    TokenKindSpec{"operator.pipe", "operator"},
    TokenKindSpec{"operator.arrow", "operator"},
    TokenKindSpec{"operator.context", "operator"},
    TokenKindSpec{"operator.assignment", "operator"},
    TokenKindSpec{"operator.namespace", "operator"},
    TokenKindSpec{"punctuation", "punctuation"},
    // Lambda / pipeline support
    TokenKindSpec{"lambda.param", "lambda-param"},
    // Expansions since the initial freeze
    TokenKindSpec{"decl.variable.local", "variable"},
    TokenKindSpec{"decl.variable.param", "variable"},
    TokenKindSpec{"keyword.concept", "keyword"},
    TokenKindSpec{"keyword.derived", "keyword"},
    TokenKindSpec{"keyword.extend", "keyword"},
    TokenKindSpec{"keyword.deny", "keyword"},
    TokenKindSpec{"keyword.as", "keyword"},
    TokenKindSpec{"keyword.self", "keyword"},
    TokenKindSpec{"keyword.where", "keyword"},
    TokenKindSpec{"keyword.match", "keyword"},
    TokenKindSpec{"keyword.break", "keyword"},
    TokenKindSpec{"keyword.yield", "keyword"},
    TokenKindSpec{"operator.arithmetic", "operator"},
    TokenKindSpec{"operator.comparison", "operator"},
    TokenKindSpec{"operator.logical", "operator"},
    TokenKindSpec{"operator.member", "operator"},
    TokenKindSpec{"operator.range", "operator"},
    TokenKindSpec{"operator.address", "operator"},
    TokenKindSpec{"operator.try", "operator"},
    TokenKindSpec{"literal.bool", "literal-number"},
    TokenKindSpec{"use.type", "type"},
    TokenKindSpec{"use.variant", "field"},
};

/// Visual group of a semantic token kind, or nullopt for a kind the
/// surface does not list.  Linear over the ~60 kinds above; callers
/// classify per token kind, not per token.
auto token_group(std::string_view kind) -> std::optional<std::string_view>;

// ---------------------------------------------------------------------------
// Lexical categories (coarse classes of raw lexer tokens) and diagnostic
// severities as they appear in JSON.
// ---------------------------------------------------------------------------

inline constexpr std::array kLexicalCategories{
    std::string_view{"keyword"},        std::string_view{"identifier"},
    std::string_view{"literal.number"}, std::string_view{"literal.string"},
    std::string_view{"literal.bool"},   std::string_view{"operator"},
    std::string_view{"punctuation"},    std::string_view{"synthetic"},
    std::string_view{"error"},          std::string_view{"unknown"},
};

inline constexpr std::array kDiagnosticSeverities{
    std::string_view{"error"},
    std::string_view{"warning"},
};

// ---------------------------------------------------------------------------
// JSON payload shapes.
//
// A field type is a TypeScript type expression drawn from: `string`,
// `number`, `boolean`, one of the enumerations above (`TokenKind`,
// `LexicalCategory`, `DiagnosticSeverity`), a shape name, or a shape
// name followed by `[]`.  Optional fields may be absent from a request.
//
// Positions carry file identity: `file` is the display path of the
// file a span lies in (`""` for a message without a location), and
// `offset`/`line`/`col` are local to that file.  Replies about the
// document being edited report its path in a top-level `file` field so
// a consumer can tell which positions are in the buffer it shows.
// ---------------------------------------------------------------------------

struct FieldSpec {
  std::string_view name;
  std::string_view type;
  bool optional = false;
};

struct ShapeSpec {
  std::string_view name;
  std::span<const FieldSpec> fields;
};

inline constexpr std::array kLexTokenFields{
    FieldSpec{"kind", "string"},   FieldSpec{"category", "LexicalCategory"},
    FieldSpec{"offset", "number"}, FieldSpec{"length", "number"},
    FieldSpec{"line", "number"},   FieldSpec{"col", "number"},
    FieldSpec{"text", "string"},
};

inline constexpr std::array kSemanticTokenFields{
    FieldSpec{"kind", "TokenKind"}, FieldSpec{"offset", "number"},
    FieldSpec{"length", "number"},  FieldSpec{"line", "number"},
    FieldSpec{"col", "number"},
};

inline constexpr std::array kDiagnosticFields{
    FieldSpec{"severity", "DiagnosticSeverity"}, FieldSpec{"file", "string"},
    FieldSpec{"offset", "number"},               FieldSpec{"length", "number"},
    FieldSpec{"line", "number"},                 FieldSpec{"col", "number"},
    FieldSpec{"message", "string"},
};

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

inline constexpr std::array kHoverFields{
    FieldSpec{"name", "string"},
    FieldSpec{"kind", "string"},
    FieldSpec{"type", "string"},
};

inline constexpr std::array kDefinitionFields{
    FieldSpec{"file", "string"},   FieldSpec{"offset", "number"},
    FieldSpec{"length", "number"}, FieldSpec{"line", "number"},
    FieldSpec{"col", "number"},
};

inline constexpr std::array kDocumentSymbolFields{
    FieldSpec{"name", "string"},   FieldSpec{"kind", "string"},
    FieldSpec{"offset", "number"}, FieldSpec{"length", "number"},
    FieldSpec{"children", "DocumentSymbol[]"},
};

inline constexpr std::array kReferenceFields{
    FieldSpec{"file", "string"},
    FieldSpec{"offset", "number"},
    FieldSpec{"length", "number"},
    FieldSpec{"line", "number"},
    FieldSpec{"col", "number"},
    FieldSpec{"isDefinition", "boolean"},
};

inline constexpr std::array kCompletionFields{
    FieldSpec{"label", "string"},
    FieldSpec{"kind", "string"},
    FieldSpec{"type", "string"},
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

inline constexpr std::array kShapes{
    ShapeSpec{"LexToken", kLexTokenFields},
    ShapeSpec{"SemanticToken", kSemanticTokenFields},
    ShapeSpec{"Diagnostic", kDiagnosticFields},
    ShapeSpec{"SourceRequest", kSourceRequestFields},
    ShapeSpec{"AnalyzeRequest", kAnalyzeRequestFields},
    ShapeSpec{"PositionRequest", kPositionRequestFields},
    ShapeSpec{"ExampleName", kExampleEntryFields},
    ShapeSpec{"AnalyzeResponse", kAnalyzeResponseFields},
    ShapeSpec{"RunResponse", kRunResponseFields},
    ShapeSpec{"Hover", kHoverFields},
    ShapeSpec{"Definition", kDefinitionFields},
    ShapeSpec{"DocumentSymbol", kDocumentSymbolFields},
    ShapeSpec{"Reference", kReferenceFields},
    ShapeSpec{"Completion", kCompletionFields},
    ShapeSpec{"ExampleEntry", kExampleEntryFields},
    ShapeSpec{"ExamplesList", kExamplesListFields},
    ShapeSpec{"ExampleSource", kExampleSourceFields},
    ShapeSpec{"ErrorReply", kErrorReplyFields},
};

auto find_shape(std::string_view name) -> const ShapeSpec*;

// ---------------------------------------------------------------------------
// Service routes.
//
// `request` names the shape a POST body must satisfy (`void` for a
// GET without a body; `ExampleName` for the path parameter of
// `/api/examples/:name`).  `response` is a TypeScript type expression
// over the shapes: a shape, `Shape[]`, or `Shape | null`.
// ---------------------------------------------------------------------------

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
    RouteSpec{"documentSymbols", "POST", "/api/document-symbols", "SourceRequest",
              "DocumentSymbol[]"},
    RouteSpec{"references", "POST", "/api/references", "PositionRequest", "Reference[]"},
    RouteSpec{"completions", "POST", "/api/completions", "PositionRequest", "Completion[]"},
    RouteSpec{"examples", "GET", "/api/examples", "void", "ExamplesList"},
    RouteSpec{"example", "GET", "/api/examples/:name", "ExampleName", "ExampleSource"},
};

auto find_route(std::string_view name) -> const RouteSpec*;

// ---------------------------------------------------------------------------
// Capabilities and the surfaces that serve them.
//
// The status of record for what tooling can do: one row per capability
// with the compiler entry point behind it and the playground route,
// `daoc` command, and LSP method that expose it (empty when none does).
// Rendered as docs/tooling_capabilities.md; tests check the route and
// command names against the route table and the driver.
// ---------------------------------------------------------------------------

struct CapabilitySpec {
  std::string_view name;
  std::string_view analysis;   // compiler entry point
  std::string_view playground; // a kRoutes name, or ""
  std::string_view cli;        // a `daoc` subcommand, or ""
  std::string_view lsp;        // the LSP method it maps to, or ""
};

inline constexpr std::array kCapabilities{
    CapabilitySpec{"Diagnostics", "Diagnostic", "analyze", "check",
                   "textDocument/publishDiagnostics"},
    CapabilitySpec{"Semantic tokens", "classify_tokens", "analyze", "tokens",
                   "textDocument/semanticTokens/full"},
    CapabilitySpec{"Hover", "query_hover", "hover", "", "textDocument/hover"},
    CapabilitySpec{"Go to definition", "query_definition", "gotoDef", "",
                   "textDocument/definition"},
    CapabilitySpec{"References", "query_references", "references", "",
                   "textDocument/references"},
    CapabilitySpec{"Document symbols", "query_document_symbols", "documentSymbols", "",
                   "textDocument/documentSymbol"},
    CapabilitySpec{"Completion", "query_completions", "completions", "",
                   "textDocument/completion"},
    CapabilitySpec{"AST dump", "print_ast", "analyze", "ast", ""},
    CapabilitySpec{"HIR dump", "print_hir", "analyze", "hir", ""},
    CapabilitySpec{"MIR dump", "print_mir", "analyze", "mir", ""},
    CapabilitySpec{"LLVM IR dump", "LlvmBackend::print_ir", "analyze", "llvm-ir", ""},
    CapabilitySpec{"Native build and run", "LlvmBackend::emit_object", "run", "build", ""},
};

/// The TypeScript module the playground frontend compiles against.
/// `tooling_surface_dump` writes it; a test asserts the checked-in
/// file equals it.
auto render_typescript() -> std::string;

/// The capability matrix as Markdown (docs/tooling_capabilities.md);
/// same generate-and-verify rule as the TypeScript.
auto render_capability_matrix() -> std::string;

} // namespace dao::tooling

#endif // DAO_ANALYSIS_TOOLING_SURFACE_H
