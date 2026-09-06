#include "analysis/tooling_surface.h"

#include <algorithm>
#include <format>
#include <ostream>
#include <sstream>

namespace dao::tooling {

auto token_group(std::string_view kind) -> std::optional<std::string_view> {
  auto it = std::ranges::find(kTokenKinds, kind, &TokenKindSpec::kind);
  if (it == kTokenKinds.end()) {
    return std::nullopt;
  }
  return it->group;
}

auto find_shape(std::string_view name) -> const ShapeSpec* {
  auto it = std::ranges::find(kShapes, name, &ShapeSpec::name);
  return it == kShapes.end() ? nullptr : &*it;
}

namespace {

/// `"a" | "b" | "c"` for a list of string literals.
auto string_union(std::span<const std::string_view> values) -> std::string {
  std::string out;
  for (auto value : values) {
    if (!out.empty()) {
      out += " | ";
    }
    out += std::format("\"{}\"", value);
  }
  return out;
}

} // namespace

void render_shape_typescript(std::ostream& out, const ShapeSpec& shape) {
  out << std::format("export interface {} {{\n", shape.name);
  for (const auto& field : shape.fields) {
    out << std::format("  {}{}: {};\n", field.name, field.optional ? "?" : "", field.type);
  }
  out << "}\n\n";
}

auto render_analysis_typescript() -> std::string {
  std::ostringstream out;
  out << "// ---- Analysis surface (compiler/analysis/tooling_surface.h) ----\n\n";

  out << "export const TOKEN_KINDS = [\n";
  for (const auto& spec : kTokenKinds) {
    out << std::format("  \"{}\",\n", spec.kind);
  }
  out << "] as const;\n\n"
         "export type TokenKind = (typeof TOKEN_KINDS)[number];\n\n";

  out << std::format("export type TokenGroup = {};\n\n", string_union(kTokenGroups));

  out << "/** Visual group of every token kind; the CSS class is `dao-<group>`. */\n"
         "export const TOKEN_GROUP: Record<TokenKind, TokenGroup> = {\n";
  for (const auto& spec : kTokenKinds) {
    out << std::format("  \"{}\": \"{}\",\n", spec.kind, spec.group);
  }
  out << "};\n\n";

  out << std::format("export type LexicalCategory = {};\n\n", string_union(kLexicalCategories));
  out << std::format("export type DiagnosticSeverity = {};\n\n",
                     string_union(kDiagnosticSeverities));

  for (const auto& shape : kShapes) {
    render_shape_typescript(out, shape);
  }
  return out.str();
}

} // namespace dao::tooling
