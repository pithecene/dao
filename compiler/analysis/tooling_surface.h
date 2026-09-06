#ifndef DAO_ANALYSIS_TOOLING_SURFACE_H
#define DAO_ANALYSIS_TOOLING_SURFACE_H

#include <array>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Analysis surface — the compiler-owned part of what tooling sees:
// semantic token kinds and their visual groups, lexical categories,
// diagnostic severities, and the payload shapes of the analysis results
// (CONTRACT_COMPILER_PHASES.md "Analysis Responsibilities",
// CONTRACT_LANGUAGE_TOOLING.md "Shared Analysis Ownership").
//
// Transport is not here.  Routes, request envelopes, and the playground's
// own response shapes live with the playground
// (tools/playground/compiler_service/service_surface.h), which renders
// this surface into the frontend's generated TypeScript together with
// its own.  Tests assert the token kinds equal the taxonomy listed in
// CONTRACT_LANGUAGE_TOOLING.md and that every kind the emitter produces
// over the corpus is listed here.
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
    std::string_view{"keyword"},
    std::string_view{"mode"},
    std::string_view{"resource"},
    std::string_view{"type"},
    std::string_view{"decl"},
    std::string_view{"variable"},
    std::string_view{"field"},
    std::string_view{"module"},
    std::string_view{"lambda-param"},
    std::string_view{"literal-number"},
    std::string_view{"literal-string"},
    std::string_view{"operator"},
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
    std::string_view{"keyword"},
    std::string_view{"identifier"},
    std::string_view{"literal.number"},
    std::string_view{"literal.string"},
    std::string_view{"literal.bool"},
    std::string_view{"operator"},
    std::string_view{"punctuation"},
    std::string_view{"synthetic"},
    std::string_view{"error"},
    std::string_view{"unknown"},
};

inline constexpr std::array kDiagnosticSeverities{
    std::string_view{"error"},
    std::string_view{"warning"},
};

// ---------------------------------------------------------------------------
// Analysis payload shapes.
//
// A field type is a TypeScript type expression drawn from: `string`,
// `number`, `boolean`, one of the enumerations above (`TokenKind`,
// `LexicalCategory`, `DiagnosticSeverity`), a shape name, or a shape
// name followed by `[]`.  Optional fields may be absent from a request.
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
    FieldSpec{"kind", "string"},
    FieldSpec{"category", "LexicalCategory"},
    FieldSpec{"offset", "number"},
    FieldSpec{"length", "number"},
    FieldSpec{"line", "number"},
    FieldSpec{"col", "number"},
    FieldSpec{"text", "string"},
};

inline constexpr std::array kSemanticTokenFields{
    FieldSpec{"kind", "TokenKind"},
    FieldSpec{"offset", "number"},
    FieldSpec{"length", "number"},
    FieldSpec{"line", "number"},
    FieldSpec{"col", "number"},
};

inline constexpr std::array kDiagnosticFields{
    FieldSpec{"severity", "DiagnosticSeverity"},
    FieldSpec{"offset", "number"},
    FieldSpec{"length", "number"},
    FieldSpec{"line", "number"},
    FieldSpec{"col", "number"},
    FieldSpec{"message", "string"},
};

inline constexpr std::array kHoverFields{
    FieldSpec{"name", "string"},
    FieldSpec{"kind", "string"},
    FieldSpec{"type", "string"},
};

inline constexpr std::array kDefinitionFields{
    FieldSpec{"offset", "number"},
    FieldSpec{"length", "number"},
    FieldSpec{"line", "number"},
    FieldSpec{"col", "number"},
};

inline constexpr std::array kDocumentSymbolFields{
    FieldSpec{"name", "string"},
    FieldSpec{"kind", "string"},
    FieldSpec{"offset", "number"},
    FieldSpec{"length", "number"},
    FieldSpec{"children", "DocumentSymbol[]"},
};

inline constexpr std::array kReferenceFields{
    FieldSpec{"offset", "number"},
    FieldSpec{"length", "number"},
    FieldSpec{"isDefinition", "boolean"},
};

inline constexpr std::array kCompletionFields{
    FieldSpec{"label", "string"},
    FieldSpec{"kind", "string"},
    FieldSpec{"type", "string"},
};

inline constexpr std::array kShapes{
    ShapeSpec{"LexToken", kLexTokenFields},
    ShapeSpec{"SemanticToken", kSemanticTokenFields},
    ShapeSpec{"Diagnostic", kDiagnosticFields},
    ShapeSpec{"Hover", kHoverFields},
    ShapeSpec{"Definition", kDefinitionFields},
    ShapeSpec{"DocumentSymbol", kDocumentSymbolFields},
    ShapeSpec{"Reference", kReferenceFields},
    ShapeSpec{"Completion", kCompletionFields},
};

auto find_shape(std::string_view name) -> const ShapeSpec*;

/// The analysis part of the frontend's generated TypeScript: token
/// kinds, groups, categories, severities, and the shapes above.  A
/// transport (the playground service) renders its envelopes after it.
auto render_analysis_typescript() -> std::string;

/// One shape as a TypeScript interface.
void render_shape_typescript(std::ostream& out, const ShapeSpec& shape);

} // namespace dao::tooling

#endif // DAO_ANALYSIS_TOOLING_SURFACE_H
