#include "analysis/tooling_surface.h"

#include <algorithm>
#include <format>
#include <ranges>
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

auto find_route(std::string_view name) -> const RouteSpec* {
  auto it = std::ranges::find(kRoutes, name, &RouteSpec::name);
  return it == kRoutes.end() ? nullptr : &*it;
}

// ---------------------------------------------------------------------------
// TypeScript rendering
// ---------------------------------------------------------------------------

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

void render_shape(std::ostream& out, const ShapeSpec& shape) {
  out << std::format("export interface {} {{\n", shape.name);
  for (const auto& field : shape.fields) {
    out << std::format("  {}{}: {};\n", field.name, field.optional ? "?" : "", field.type);
  }
  out << "}\n\n";
}

} // namespace

auto render_typescript() -> std::string {
  std::ostringstream out;
  out << "// Generated from compiler/analysis/tooling_surface.h by tooling_surface_dump.\n"
         "// Do not edit by hand: change the table and run `task gen-tooling-surface`.\n"
         "// tooling_surface_test fails while this file and the table disagree.\n\n";

  // Token kinds and groups.
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

  // Shapes.
  for (const auto& shape : kShapes) {
    render_shape(out, shape);
  }

  // Routes.
  out << "export const ROUTES = {\n";
  for (const auto& route : kRoutes) {
    out << std::format("  {}: {{ method: \"{}\", path: \"{}\" }},\n", route.name, route.method,
                       route.path);
  }
  out << "} as const;\n\n"
         "export type RouteName = keyof typeof ROUTES;\n\n";

  out << "/** Request body and response type of every route. */\n"
         "export interface RouteTypes {\n";
  for (const auto& route : kRoutes) {
    out << std::format("  {}: {{ request: {}; response: {} }};\n", route.name, route.request,
                       route.response);
  }
  out << "}\n";

  return out.str();
}

auto render_capability_matrix() -> std::string {
  std::ostringstream out;
  out << "# Tooling Capabilities — Dao\n\n"
         "Generated from `compiler/analysis/tooling_surface.h` by `tooling_surface_dump`;\n"
         "do not edit by hand (`task gen-tooling-surface`).  `tooling_surface_test` fails\n"
         "while this file and the table disagree.\n\n"
         "One row per capability the compiler exposes to tooling and the surfaces that\n"
         "serve it.  \"—\" means no surface of that kind serves it.  The LSP column names\n"
         "the method each capability maps to; `tools/lsp` is not implemented, so none is\n"
         "served over LSP yet.\n\n"
         "| Capability | Compiler entry point | Playground route | `daoc` command | LSP method |\n"
         "|---|---|---|---|---|\n";
  auto cell = [](std::string_view value, bool code) -> std::string {
    if (value.empty()) {
      return "—";
    }
    return code ? std::format("`{}`", value) : std::string(value);
  };
  for (const auto& cap : kCapabilities) {
    out << std::format("| {} | {} | {} | {} | {} |\n", cap.name, cell(cap.analysis, true),
                       cell(cap.playground, true), cell(cap.cli, true), cell(cap.lsp, true));
  }
  return out.str();
}

} // namespace dao::tooling
