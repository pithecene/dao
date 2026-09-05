#include "analysis/document_symbols.h"

namespace dao {

namespace {

auto symbol_for_function(const FunctionDecl& fn, Span decl_span)
    -> DocumentSymbol {
  DocumentSymbol sym;
  sym.name = std::string(fn.name);
  sym.kind = fn.is_extern ? "extern_function" : "function";
  sym.span = decl_span;
  sym.name_span = fn.name_span;

  for (const auto& param : fn.params) {
    sym.children.push_back({
        .name = std::string(param.name),
        .kind = "parameter",
        .span = param.name_span,
        .name_span = param.name_span,
        .children = {},
    });
  }
  return sym;
}

auto symbol_for_class(const ClassDecl& cls, Span decl_span)
    -> DocumentSymbol {
  DocumentSymbol sym;
  sym.name = std::string(cls.name);
  sym.kind = "class";
  sym.span = decl_span;
  sym.name_span = cls.name_span;

  for (const auto* field : cls.fields) {
    sym.children.push_back({
        .name = std::string(field->name),
        .kind = "field",
        .span = field->span,
        .name_span = field->name_span,
        .children = {},
    });
  }
  return sym;
}

auto symbol_for_concept(const ConceptDecl& decl, Span decl_span)
    -> DocumentSymbol {
  DocumentSymbol sym;
  sym.name = std::string(decl.name);
  sym.kind = decl.is_derived ? "derived_concept" : "concept";
  sym.span = decl_span;
  sym.name_span = decl.name_span;

  for (const auto* method : decl.methods) {
    const auto& fn = method->as<FunctionDecl>();
    sym.children.push_back({
        .name = std::string(fn.name),
        .kind = "method",
        .span = method->span,
        .name_span = fn.name_span,
        .children = {},
    });
  }
  return sym;
}

auto symbol_for_alias(const AliasDecl& alias, Span decl_span)
    -> DocumentSymbol {
  return {
      .name = std::string(alias.name),
      .kind = "alias",
      .span = decl_span,
      .name_span = alias.name_span,
      .children = {},
  };
}

} // namespace

auto query_document_symbols(const FileNode& file) -> std::vector<DocumentSymbol> {
  std::vector<DocumentSymbol> symbols;

  for (const auto* decl : file.declarations) {
    switch (decl->kind()) {
    case NodeKind::FunctionDecl:
      symbols.push_back(
          symbol_for_function(decl->as<FunctionDecl>(), decl->span));
      break;
    case NodeKind::ClassDecl:
      symbols.push_back(
          symbol_for_class(decl->as<ClassDecl>(), decl->span));
      break;
    case NodeKind::ConceptDecl:
      symbols.push_back(
          symbol_for_concept(decl->as<ConceptDecl>(), decl->span));
      break;
    case NodeKind::AliasDecl:
      symbols.push_back(
          symbol_for_alias(decl->as<AliasDecl>(), decl->span));
      break;
    default:
      // ExtendDecl — skip for document symbols (it extends, doesn't
      // declare a new named symbol).
      break;
    }
  }

  return symbols;
}

} // namespace dao
