#include "analysis/semantic_tokens.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dao {

namespace {

// ---------------------------------------------------------------------------
// Builtin type names
// ---------------------------------------------------------------------------

auto is_builtin_type(std::string_view name) -> bool {
  // Builtin scalar types only — predeclared named types (string, void)
  // are classified as type.nominal, not type.builtin.
  // See CONTRACT_TYPE_SYSTEM_FOUNDATIONS.md §5.
  static constexpr std::string_view builtins[] = {
      "i8",
      "i16",
      "i32",
      "i64",
      "u8",
      "u16",
      "u32",
      "u64",
      "f32",
      "f64",
      "bool",
  };
  for (auto b : builtins) {
    if (name == b) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Lexical classification — TokenKind → taxonomy category
// ---------------------------------------------------------------------------

auto lexical_category(TokenKind kind) -> std::string_view {
  switch (kind) {
  // Keywords — control
  case TokenKind::KwModule:
    return "keyword.module";
  case TokenKind::KwImport:
    return "keyword.import";
  case TokenKind::KwExtern:
    return "keyword.extern";
  case TokenKind::KwFn:
    return "keyword.fn";
  case TokenKind::KwClass:
    return "keyword.type";
  case TokenKind::KwEnum:
    return "keyword.type";
  case TokenKind::KwType:
    return "keyword.type";
  case TokenKind::KwLet:
    return "keyword.let";
  case TokenKind::KwIf:
    return "keyword.if";
  case TokenKind::KwElse:
    return "keyword.else";
  case TokenKind::KwWhile:
    return "keyword.while";
  case TokenKind::KwFor:
    return "keyword.for";
  case TokenKind::KwIn:
    return "keyword.in";
  case TokenKind::KwReturn:
    return "keyword.return";
  case TokenKind::KwYield:
    return "keyword.yield";
  case TokenKind::KwBreak:
    return "keyword.break";
  case TokenKind::KwMatch:
    return "keyword.match";

  // Keywords — execution / resource constructs
  case TokenKind::KwMode:
    return "keyword.mode";
  case TokenKind::KwResource:
    return "keyword.resource";

  // Keyword literals and word-spelled logical operators.
  case TokenKind::KwTrue:
  case TokenKind::KwFalse:
    return "literal.bool";
  case TokenKind::KwAnd:
  case TokenKind::KwOr:
    return "operator.logical";

  // Keywords — concepts and conformance
  case TokenKind::KwConcept:
    return "keyword.concept";
  case TokenKind::KwDerived:
    return "keyword.derived";
  case TokenKind::KwAs:
    return "keyword.as";
  case TokenKind::KwExtend:
    return "keyword.extend";
  case TokenKind::KwDeny:
    return "keyword.deny";
  case TokenKind::KwSelf:
    return "keyword.self";
  case TokenKind::KwWhere:
    return "keyword.where";

  // Numeric literals
  case TokenKind::IntLiteral:
  case TokenKind::FloatLiteral:
    return "literal.number";

  // String literals
  case TokenKind::StringLiteral:
    return "literal.string";

  // Operators — specific taxonomy categories
  case TokenKind::PipeGt:
    return "operator.pipe";
  case TokenKind::Arrow:
  case TokenKind::FatArrow:
    return "operator.arrow";
  case TokenKind::Eq:
    return "operator.assignment";
  case TokenKind::ColonColon:
    return "operator.namespace";
  case TokenKind::Colon:
    return "operator.context";

  // Operators — expansion categories (CONTRACT_LANGUAGE_TOOLING.md,
  // "Expansions since the initial freeze").  `<` and `>` also delimit
  // generic argument lists; classify_tokens repaints those as
  // punctuation from context.
  case TokenKind::EqEq:
  case TokenKind::BangEq:
  case TokenKind::Lt:
  case TokenKind::LtEq:
  case TokenKind::Gt:
  case TokenKind::GtEq:
    return "operator.comparison";
  case TokenKind::Plus:
  case TokenKind::Minus:
  case TokenKind::Star:
  case TokenKind::Slash:
  case TokenKind::Percent:
    return "operator.arithmetic";
  case TokenKind::Bang:
    return "operator.logical";
  case TokenKind::Amp:
    return "operator.address";
  case TokenKind::Dot:
    return "operator.member";
  case TokenKind::DotDot:
    return "operator.range";
  case TokenKind::Question:
    return "operator.try";

  // Punctuation
  case TokenKind::Pipe: // lambda parameter delimiter
  case TokenKind::Comma:
  case TokenKind::LParen:
  case TokenKind::RParen:
  case TokenKind::LBracket:
  case TokenKind::RBracket:
    return "punctuation";

  // Identifiers need structural context.
  case TokenKind::Identifier:
    return "";

  // Synthetic and error tokens are not classified.
  case TokenKind::Newline:
  case TokenKind::Indent:
  case TokenKind::Dedent:
  case TokenKind::Eof:
  case TokenKind::Error:
    return "";
  }
  return "";
}

// ---------------------------------------------------------------------------
// AST walker — collects span → category mappings from structural context
// ---------------------------------------------------------------------------

class AstClassifier {
public:
  using SpanMap = std::unordered_map<uint32_t, std::string_view>;

  // A qualified-name expression, keyed by the offset of its first
  // segment: the resolver records a single use for the whole path at
  // that offset, so classify_tokens needs the other segments' spans to
  // paint `Type::method` and `Enum::Variant` per segment.
  using QualifiedSpans = std::unordered_map<uint32_t, std::vector<Span>>;

  // Field-access sites whose object is a bare identifier, keyed by the
  // field's offset and mapping to the object's offset.  When the
  // object resolves to a type, the field is an enum variant.
  using FieldObjects = std::unordered_map<uint32_t, uint32_t>;

  auto classifications() const -> const SpanMap& {
    return map_;
  }
  auto qualified_expressions() const -> const QualifiedSpans& {
    return qualified_;
  }

  /// A named-argument label and the call's callee.
  struct NamedLabel {
    Span label;
    const Expr* callee;
  };
  auto named_labels() const -> const std::vector<NamedLabel>& {
    return named_labels_;
  }
  auto field_objects() const -> const FieldObjects& {
    return field_objects_;
  }

  void visit_file(const FileNode& file) {
    if (file.module_decl != nullptr) {
      visit_module_decl(*file.module_decl);
    }
    for (const auto* imp : file.imports) {
      visit_import(*imp);
    }
    for (const auto* decl : file.declarations) {
      visit_decl(*decl);
    }
  }

private:
  SpanMap map_;
  QualifiedSpans qualified_;
  FieldObjects field_objects_;
  std::vector<NamedLabel> named_labels_;

  void classify(Span span, std::string_view kind) {
    map_[span.offset] = kind;
  }

  void classify_generic_params(const std::vector<GenericParam>& params) {
    for (const auto& param : params) {
      classify(param.name_span, "decl.type");
      for (const auto* constraint : param.constraints) {
        visit_type(*constraint);
      }
    }
  }

  void visit_methods(const std::vector<Decl*>& methods) {
    for (const auto* method : methods) {
      visit_decl(*method);
    }
  }

  // --- Qualified path helpers ---

  // Compute per-segment spans from a QualifiedPath. Segments are
  // separated by "::" (2 characters) in the source.
  static auto segment_spans(const QualifiedPath& path)
      -> std::vector<std::pair<std::string_view, Span>> {
    std::vector<std::pair<std::string_view, Span>> result;
    uint32_t offset = path.span.offset;
    for (const auto& seg : path.segments) {
      auto len = static_cast<uint32_t>(seg.size());
      result.emplace_back(seg, Span{.offset = offset, .length = len});
      offset += len + 2; // skip "::" separator
    }
    return result;
  }

  // Classify all segments in a qualified path: leading segments are
  // use.module, the trailing segment gets the given category.
  void classify_qualified(const QualifiedPath& path, std::string_view tail_category) {
    auto spans = segment_spans(path);
    for (size_t i = 0; i < spans.size(); ++i) {
      if (i + 1 < spans.size()) {
        classify(spans[i].second, "use.module");
      } else {
        classify(spans[i].second, tail_category);
      }
    }
  }

  // --- Module declaration ---

  void visit_module_decl(const ModuleNode& node) {
    // All segments of the declared module path are binding sites —
    // classify every segment as decl.module, mirroring the import
    // convention where the trailing segment is decl.module.
    auto spans = segment_spans(node.path);
    for (const auto& entry : spans) {
      classify(entry.second, "decl.module");
    }
  }

  // --- Imports ---

  void visit_import(const ImportNode& node) {
    // Leading segments of an import path are module references.
    // The last segment is the binding site — classified as decl.module.
    auto spans = segment_spans(node.path);
    for (size_t i = 0; i < spans.size(); ++i) {
      if (i + 1 < spans.size()) {
        classify(spans[i].second, "use.module");
      } else {
        classify(spans[i].second, "decl.module");
      }
    }
  }

  // --- Declarations ---

  void visit_decl(const Decl& decl) {
    switch (decl.kind()) {
    case NodeKind::FunctionDecl:
      visit_function(decl);
      break;
    case NodeKind::ClassDecl:
      visit_class(decl);
      break;
    case NodeKind::EnumDecl:
      visit_enum(decl);
      break;
    case NodeKind::AliasDecl:
      visit_alias(decl);
      break;
    case NodeKind::ConceptDecl:
      visit_concept(decl);
      break;
    case NodeKind::ExtendDecl:
      visit_extend(decl);
      break;
    default:
      break;
    }
  }

  void visit_function(const Decl& decl) {
    const auto& fn = decl.as<FunctionDecl>();
    classify(fn.name_span, "decl.function");
    classify_generic_params(fn.type_params);

    for (const auto& param : fn.params) {
      // The `self` receiver is a keyword, not a declared parameter name.
      if (param.name != "self") {
        classify(param.name_span, "decl.variable.param");
      }
      if (param.type != nullptr) {
        visit_type(*param.type);
      }
    }

    if (fn.return_type != nullptr) {
      visit_type(*fn.return_type);
    }

    for (const auto* stmt : fn.body) {
      visit_stmt(*stmt);
    }

    if (fn.expr_body != nullptr) {
      visit_expr(*fn.expr_body);
    }
  }

  void visit_class(const Decl& decl) {
    const auto& st = decl.as<ClassDecl>();
    classify(st.name_span, "decl.type");
    classify_generic_params(st.type_params);

    for (const auto* field : st.fields) {
      classify(field->name_span, "decl.field");
      if (field->type != nullptr) {
        visit_type(*field->type);
      }
    }
    visit_methods(st.methods);
    for (const auto& conformance : st.conformances) {
      classify(conformance.target.concept_span, "use.type");
      visit_methods(conformance.methods);
    }
    for (const auto& denial : st.denials) {
      classify(denial.target.concept_span, "use.type");
    }
  }

  void visit_enum(const Decl& decl) {
    const auto& en = decl.as<EnumDeclNode>();
    classify(en.name_span, "decl.type");
    classify_generic_params(en.type_params);
    for (const auto& variant : en.variants) {
      classify(variant.name_span, "decl.field");
      for (const auto* payload : variant.payload_types) {
        visit_type(*payload);
      }
      for (const auto& field_span : variant.field_name_spans) {
        classify(field_span, "decl.field");
      }
    }
  }

  void visit_alias(const Decl& decl) {
    const auto& alias = decl.as<AliasDecl>();
    classify(alias.name_span, "decl.type");

    if (alias.type != nullptr) {
      visit_type(*alias.type);
    }
  }

  void visit_concept(const Decl& decl) {
    const auto& concept_decl = decl.as<ConceptDecl>();
    classify(concept_decl.name_span, "decl.type");
    classify_generic_params(concept_decl.type_params);
    visit_methods(concept_decl.methods);
  }

  void visit_extend(const Decl& decl) {
    const auto& extend = decl.as<ExtendDecl>();
    if (extend.target_type != nullptr) {
      visit_type(*extend.target_type);
    }
    if (!extend.target.concept_name.empty()) {
      classify(extend.target.concept_span, "use.type");
    }
    visit_methods(extend.methods);
  }

  // --- Statements ---

  void visit_stmt(const Stmt& stmt) {
    switch (stmt.kind()) {
    case NodeKind::LetStatement: {
      const auto& let_stmt = stmt.as<LetStatement>();
      classify(let_stmt.name_span, "decl.variable.local");
      if (let_stmt.type != nullptr) {
        visit_type(*let_stmt.type);
      }
      if (let_stmt.initializer != nullptr) {
        visit_expr(*let_stmt.initializer);
      }
      break;
    }
    case NodeKind::Assignment: {
      const auto& assign = stmt.as<Assignment>();
      visit_expr(*assign.target);
      visit_expr(*assign.value);
      break;
    }
    case NodeKind::IfStatement: {
      const auto& if_stmt = stmt.as<IfStatement>();
      visit_expr(*if_stmt.condition);
      for (const auto* s : if_stmt.then_body) {
        visit_stmt(*s);
      }
      for (const auto* s : if_stmt.else_body) {
        visit_stmt(*s);
      }
      break;
    }
    case NodeKind::WhileStatement: {
      const auto& while_stmt = stmt.as<WhileStatement>();
      visit_expr(*while_stmt.condition);
      for (const auto* s : while_stmt.body) {
        visit_stmt(*s);
      }
      break;
    }
    case NodeKind::ForStatement: {
      const auto& for_stmt = stmt.as<ForStatement>();
      classify(for_stmt.var_span, "decl.variable.local");
      visit_expr(*for_stmt.iterable);
      for (const auto* s : for_stmt.body) {
        visit_stmt(*s);
      }
      break;
    }
    case NodeKind::YieldStatement: {
      const auto& yield_stmt = stmt.as<YieldStatement>();
      if (yield_stmt.value != nullptr) {
        visit_expr(*yield_stmt.value);
      }
      break;
    }
    case NodeKind::MatchStatement: {
      const auto& match_stmt = stmt.as<MatchStmt>();
      visit_expr(*match_stmt.scrutinee);
      for (const auto& arm : match_stmt.arms) {
        visit_pattern(*arm.pattern);
        for (const auto& binding_span : arm.binding_spans) {
          classify(binding_span, "decl.variable.local");
        }
        if (!arm.as_binding.empty()) {
          classify(arm.as_binding_span, "decl.variable.local");
        }
        for (const auto* s : arm.body) {
          visit_stmt(*s);
        }
      }
      break;
    }
    case NodeKind::ModeBlock: {
      const auto& mode = stmt.as<ModeBlock>();
      auto mode_name = mode.mode_name;
      if (mode_name == "unsafe") {
        classify(mode.name_span, "mode.unsafe");
      } else if (mode_name == "gpu") {
        classify(mode.name_span, "mode.gpu");
      } else if (mode_name == "parallel") {
        classify(mode.name_span, "mode.parallel");
      }
      for (const auto* s : mode.body) {
        visit_stmt(*s);
      }
      break;
    }
    case NodeKind::ResourceBlock: {
      const auto& res = stmt.as<ResourceBlock>();
      auto kind = res.resource_kind;
      if (kind == "memory") {
        classify(res.kind_span, "resource.kind.memory");
      }
      classify(res.name_span, "resource.binding");
      for (const auto* s : res.body) {
        visit_stmt(*s);
      }
      break;
    }
    case NodeKind::ReturnStatement: {
      const auto& ret = stmt.as<ReturnStatement>();
      if (ret.value != nullptr) {
        visit_expr(*ret.value);
      }
      break;
    }
    case NodeKind::ExpressionStatement: {
      const auto& expr_stmt = stmt.as<ExpressionStatement>();
      visit_expr(*expr_stmt.expr);
      break;
    }
    default:
      break;
    }
  }

  // --- Patterns ---

  // A match pattern is a constant, a bare variant name, `Enum.Variant`,
  // or `Enum::Variant`.  The variant name is a use.variant; the enum
  // head is left to the resolver (a type use).
  void visit_pattern(const Expr& pattern) {
    switch (pattern.kind()) {
    case NodeKind::FieldExpr: {
      const auto& field = pattern.as<FieldExpr>();
      visit_expr(*field.object);
      classify(field.field_span, "use.variant");
      break;
    }
    case NodeKind::QualifiedName: {
      auto spans = record_qualified(pattern);
      if (!spans.empty()) {
        classify(spans.back(), "use.variant");
      }
      break;
    }
    default:
      visit_expr(pattern);
      break;
    }
  }

  // Compute and remember the per-segment spans of a qualified-name
  // expression (segments are separated by `::`).
  auto record_qualified(const Expr& expr) -> std::vector<Span> {
    const auto& qn = expr.as<QualifiedName>();
    std::vector<Span> spans;
    uint32_t offset = expr.span.offset;
    for (const auto& seg : qn.segments) {
      auto len = static_cast<uint32_t>(seg.size());
      spans.push_back(Span{.offset = offset, .length = len});
      offset += len + 2; // skip "::"
    }
    if (!spans.empty()) {
      qualified_[spans.front().offset] = spans;
    }
    return spans;
  }

  // --- Expressions ---

  void visit_expr(const Expr& expr) {
    switch (expr.kind()) {
    case NodeKind::BinaryExpr: {
      const auto& bin = expr.as<BinaryExpr>();
      visit_expr(*bin.left);
      visit_expr(*bin.right);
      break;
    }
    case NodeKind::UnaryExpr: {
      const auto& unary = expr.as<UnaryExpr>();
      // The operator is the expression's first character; `*`/`&` are
      // address operators here, not arithmetic.
      Span op_span{.offset = expr.span.offset, .length = 1};
      switch (unary.op) {
      case UnaryOp::Deref:
      case UnaryOp::AddrOf:
        classify(op_span, "operator.address");
        break;
      case UnaryOp::Not:
        classify(op_span, "operator.logical");
        break;
      case UnaryOp::Negate:
        break;
      }
      visit_expr(*unary.operand);
      break;
    }
    case NodeKind::CallExpr: {
      const auto& call = expr.as<CallExpr>();
      visit_expr(*call.callee);
      for (const auto* type_arg : call.type_args) {
        visit_type(*type_arg);
      }
      for (const auto* arg : call.args) {
        visit_expr(*arg);
      }
      // Named-argument labels name fields when the callee is a type
      // (an enum-class variant or class constructor); that is decided
      // once the callee is resolved.
      for (const auto& label : call.arg_name_spans) {
        if (label.length > 0) {
          named_labels_.push_back({.label = label, .callee = call.callee});
        }
      }
      break;
    }
    case NodeKind::IndexExpr: {
      const auto& idx = expr.as<IndexExpr>();
      visit_expr(*idx.object);
      for (const auto* i : idx.indices) {
        visit_expr(*i);
      }
      break;
    }
    case NodeKind::FieldExpr: {
      const auto& field = expr.as<FieldExpr>();
      visit_expr(*field.object);
      classify(field.field_span, "use.field");
      if (field.object->kind() == NodeKind::Identifier) {
        field_objects_[field.field_span.offset] = field.object->span.offset;
      }
      break;
    }
    case NodeKind::PipeExpr: {
      const auto& pipe = expr.as<PipeExpr>();
      visit_expr(*pipe.left);
      visit_expr(*pipe.right);
      break;
    }
    case NodeKind::TryExpr: {
      const auto& try_expr = expr.as<TryExpr>();
      visit_expr(*try_expr.operand);
      break;
    }
    case NodeKind::Lambda: {
      const auto& lam = expr.as<LambdaExpr>();
      for (const auto& [name, span] : lam.params) {
        classify(span, "lambda.param");
      }
      visit_expr(*lam.body);
      break;
    }
    case NodeKind::ListLiteral: {
      const auto& list = expr.as<ListLiteral>();
      for (const auto* elem : list.elements) {
        visit_expr(*elem);
      }
      break;
    }
    case NodeKind::QualifiedName:
      // Segment classification is decided with the resolver's help in
      // classify_tokens (module path, Type::method, or Enum::Variant);
      // the walker only records the segment spans.
      record_qualified(expr);
      break;
    // Terminals — no structural classification needed.
    case NodeKind::Identifier:
    case NodeKind::IntLiteral:
    case NodeKind::FloatLiteral:
    case NodeKind::StringLiteral:
    case NodeKind::BoolLiteral:
      break;
    default:
      break;
    }
  }

  // --- Types ---

  void visit_type(const TypeNode& type) {
    switch (type.kind()) {
    case NodeKind::NamedType: {
      const auto& named = type.as<NamedType>();
      // Classify the type name: leading segments are use.module,
      // the final segment is type.builtin or type.nominal.
      if (!named.name.segments.empty()) {
        auto type_name = named.name.segments.back();
        auto category = is_builtin_type(type_name) ? "type.builtin" : "type.nominal";
        classify_qualified(named.name, category);
      }
      for (const auto* arg : named.type_args) {
        visit_type(*arg);
      }
      break;
    }
    case NodeKind::PointerType: {
      const auto& ptr = type.as<PointerType>();
      // The leading `*` of a pointer type is an address operator, not
      // multiplication.
      classify(Span{.offset = type.span.offset, .length = 1}, "operator.address");
      visit_type(*ptr.pointee);
      break;
    }
    case NodeKind::FunctionType: {
      const auto& ftn = type.as<FunctionTypeNode>();
      // Classify 'fn' keyword in the function type.
      classify(Span{.offset = type.span.offset, .length = 2}, "keyword.fn");
      for (const auto* param_type : ftn.param_types) {
        visit_type(*param_type);
      }
      visit_type(*ftn.return_type);
      break;
    }
    default:
      break;
    }
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Map a resolved symbol kind to a semantic token category for use sites.
auto resolve_use_category(SymbolKind kind) -> std::string_view {
  switch (kind) {
  case SymbolKind::Function:
    return "use.function";
  case SymbolKind::Param:
    return "use.variable.param";
  case SymbolKind::Local:
    return "use.variable.local";
  case SymbolKind::Module:
    return "use.module";
  case SymbolKind::LambdaParam:
    return "use.variable.param"; // reuse param category for lambda params
  case SymbolKind::Type:
    return "use.type"; // a type used as a value: constructor, static receiver, variant head
  case SymbolKind::GenericParam:
    return "use.type"; // generic type parameters classify as type uses
  case SymbolKind::Concept:
    return "use.type"; // concept references classify as type uses
  case SymbolKind::Builtin:
  case SymbolKind::Predeclared:
    return ""; // type-position symbols — classified by visit_type(), not here
  default:
    return "";
  }
}

namespace {

// Token kinds after which `<` opens a generic argument or parameter
// list rather than a comparison.
auto opens_generic_list(std::string_view previous_kind) -> bool {
  return previous_kind.starts_with("type.") || previous_kind == "use.type" ||
         previous_kind == "decl.type" || previous_kind == "use.function" ||
         previous_kind == "decl.function";
}

// Per-segment categories for a qualified-name expression, given how
// the resolver classified its head.
struct QualifiedPainting {
  std::string_view head;
  std::string_view tail;
};

auto paint_qualified(const Symbol& head_symbol) -> QualifiedPainting {
  switch (head_symbol.kind) {
  case SymbolKind::Function:
    // `Type::method` resolves to the mangled method symbol at the head.
    if (head_symbol.name.find('.') != std::string_view::npos) {
      return {.head = "use.type", .tail = "use.function"};
    }
    return {.head = "use.function", .tail = "use.function"};
  case SymbolKind::Type:
    return {.head = "use.type", .tail = "use.variant"};
  case SymbolKind::Module:
    return {.head = "use.module", .tail = ""};
  default:
    return {.head = "", .tail = ""};
  }
}

} // namespace

auto classify_tokens(const std::vector<Token>& tokens,
                     const FileNode* file,
                     const ResolveResult* resolve_result) -> std::vector<SemanticToken> {
  // Step 1: Collect structural classifications from AST.
  AstClassifier::SpanMap ast_map;
  AstClassifier::QualifiedSpans qualified;
  AstClassifier::FieldObjects field_objects;
  std::vector<AstClassifier::NamedLabel> named_labels;
  if (file != nullptr) {
    AstClassifier classifier;
    classifier.visit_file(*file);
    ast_map = classifier.classifications();
    qualified = classifier.qualified_expressions();
    field_objects = classifier.field_objects();
    named_labels = classifier.named_labels();
  }

  // The resolver's symbol for an identifier token, if any.
  auto resolved_symbol = [&](uint32_t offset) -> const Symbol* {
    if (resolve_result == nullptr) {
      return nullptr;
    }
    auto res_it = resolve_result->uses.find(offset);
    return res_it != resolve_result->uses.end() ? res_it->second : nullptr;
  };

  // Step 2: Walk tokens, preferring AST classification over lexical,
  // with resolve-driven classifications filling in identifier gaps.
  std::vector<SemanticToken> result;
  result.reserve(tokens.size());

  // Categories decided for later tokens by an earlier one: trailing
  // segments of a qualified name painted from its resolved head.
  std::unordered_map<uint32_t, std::string_view> pending;

  // A named-argument label is a field only when the callee is a type;
  // a label on an ordinary call names nothing the checker recognises
  // and is left unclassified rather than mislabelled.
  for (const auto& named : named_labels) {
    const auto* callee = resolved_symbol(named.callee->span.offset);
    if (callee != nullptr && callee->kind == SymbolKind::Type) {
      pending[named.label.offset] = "use.field";
    }
  }
  // Nesting depth of generic `<...>` lists, so their brackets are
  // punctuation rather than comparisons.
  uint32_t generic_depth = 0;
  std::string_view previous_kind;
  TokenKind previous_token = TokenKind::Eof;
  // `Type<Args>::method` is parsed into one mangled callee, so the
  // method token has no node of its own: remember the method name from
  // the resolved `Type.method` symbol and paint the next identifier
  // that follows `::` with that text.
  std::string_view pending_static_method;

  auto emit = [&](const Token& tok, std::string_view kind) {
    result.push_back({.span = tok.span, .kind = kind});
    previous_kind = kind;
  };

  for (const auto& tok : tokens) {
    // Skip synthetic tokens.
    if (tok.kind == TokenKind::Newline || tok.kind == TokenKind::Indent ||
        tok.kind == TokenKind::Dedent || tok.kind == TokenKind::Eof ||
        tok.kind == TokenKind::Error) {
      continue;
    }
    const auto preceding_token = previous_token;
    previous_token = tok.kind;

    if (auto pend = pending.find(tok.span.offset); pend != pending.end()) {
      emit(tok, pend->second);
      continue;
    }

    if (!pending_static_method.empty() && tok.kind == TokenKind::Identifier &&
        preceding_token == TokenKind::ColonColon && tok.text == pending_static_method) {
      pending_static_method = {};
      emit(tok, "use.function");
      continue;
    }

    // Generic argument/parameter brackets.
    if (tok.kind == TokenKind::Lt && opens_generic_list(previous_kind)) {
      ++generic_depth;
      emit(tok, "punctuation");
      continue;
    }
    if (tok.kind == TokenKind::Gt && generic_depth > 0) {
      --generic_depth;
      emit(tok, "punctuation");
      continue;
    }

    // Structural classification first: declaration sites, type
    // positions, fields, patterns.  A type name in a type position stays
    // `type.*` even though the resolver also records it as a use.
    if (auto it = ast_map.find(tok.span.offset); it != ast_map.end()) {
      auto kind = it->second;
      // `Enum.Variant`: a field access whose object is a type.
      if (kind == "use.field") {
        if (auto obj = field_objects.find(tok.span.offset); obj != field_objects.end()) {
          const auto* object_sym = resolved_symbol(obj->second);
          if (object_sym != nullptr && object_sym->kind == SymbolKind::Type) {
            kind = "use.variant";
          }
        }
      }
      emit(tok, kind);
      continue;
    }

    // Identifiers without structural classification are uses: the
    // resolver gives the authoritative category and, for a qualified
    // name, decides how its trailing segments are painted.
    if (tok.kind == TokenKind::Identifier) {
      if (const auto* sym = resolved_symbol(tok.span.offset); sym != nullptr) {
        if (auto qual = qualified.find(tok.span.offset); qual != qualified.end()) {
          auto painting = paint_qualified(*sym);
          const auto& spans = qual->second;
          for (size_t i = 1; i + 1 < spans.size(); ++i) {
            pending[spans[i].offset] = "use.module";
          }
          if (spans.size() > 1 && !painting.tail.empty()) {
            pending[spans.back().offset] = painting.tail;
          }
          if (!painting.head.empty()) {
            emit(tok, painting.head);
            continue;
          }
        }
        // A static call `Type<Args>::method(...)` resolves at the head
        // token to the mangled `Type.method` symbol.
        if (sym->kind == SymbolKind::Function) {
          auto dot = sym->name.find('.');
          if (dot != std::string_view::npos && sym->name.substr(0, dot) == tok.text) {
            pending_static_method = sym->name.substr(dot + 1);
            emit(tok, "use.type");
            continue;
          }
        }
        auto category = resolve_use_category(sym->kind);
        if (!category.empty()) {
          emit(tok, category);
          continue;
        }
      }
    }

    // Fall back to lexical classification.
    auto category = lexical_category(tok.kind);
    if (!category.empty()) {
      emit(tok, category);
    }
    // Identifiers with no classification are omitted.
  }

  return result;
}

} // namespace dao
