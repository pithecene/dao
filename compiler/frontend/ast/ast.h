#ifndef DAO_FRONTEND_AST_AST_H
#define DAO_FRONTEND_AST_AST_H

#include "frontend/diagnostics/source.h"
#include "support/arena.h"
#include "support/variant.h"

#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// AstContext — owns all AST nodes for a compilation unit.
// ---------------------------------------------------------------------------

class AstContext {
public:
  AstContext() = default;
  ~AstContext() = default;

  AstContext(const AstContext&) = delete;
  auto operator=(const AstContext&) -> AstContext& = delete;
  AstContext(AstContext&&) noexcept = default;
  auto operator=(AstContext&&) noexcept -> AstContext& = default;

  template <typename T, typename... Args> auto alloc(Args&&... args) -> T* {
    return arena_.alloc<T>(std::forward<Args>(args)...);
  }

private:
  Arena arena_;
};

// ---------------------------------------------------------------------------
// Node kind tags
// ---------------------------------------------------------------------------

enum class NodeKind : std::uint8_t {
  // File
  File,
  ModuleDecl,
  Import,

  // Declarations
  FunctionDecl,
  ClassDecl,
  EnumDecl,
  AliasDecl,
  ConceptDecl,
  ExtendDecl,

  // Class members
  FieldSpec,

  // Statements
  LetStatement,
  Assignment,
  IfStatement,
  WhileStatement,
  ForStatement,
  YieldStatement,
  BreakStatement,
  MatchStatement,
  ModeBlock,
  ResourceBlock,
  ReturnStatement,
  ExpressionStatement,

  // Expressions
  BinaryExpr,
  UnaryExpr,
  CallExpr,
  IndexExpr,
  FieldExpr,
  PipeExpr,
  TryExpr,
  Lambda,
  IntLiteral,
  FloatLiteral,
  StringLiteral,
  BoolLiteral,
  ListLiteral,
  Identifier,
  QualifiedName,

  // Types
  NamedType,
  PointerType,
  FunctionType,

  // Error recovery placeholders
  ErrorExpr,
  ErrorStmt,
  ErrorDecl,
};

// ---------------------------------------------------------------------------
// Forward declarations for recursive references.
// ---------------------------------------------------------------------------

struct Decl;
struct Stmt;
struct Expr;
struct TypeNode;

// ---------------------------------------------------------------------------
// Utility types
// ---------------------------------------------------------------------------

struct QualifiedPath {
  std::vector<std::string_view> segments;
  Span span;
  // One span per segment, as the tokens sat in the source: `lib :: x`
  // is legal, so a consumer cannot place a segment from the text alone.
  std::vector<Span> segment_spans;
};

struct Param {
  std::string_view name;
  Span name_span;
  TypeNode* type;
};

struct GenericParam {
  std::string_view name;
  Span name_span;
  std::vector<TypeNode*> constraints; // concept bounds from `: Concept + Concept`
};

// ---------------------------------------------------------------------------
// Binary / Unary operator tags
// ---------------------------------------------------------------------------

enum class BinaryOp : std::uint8_t {
  Add,    // +
  Sub,    // -
  Mul,    // *
  Div,    // /
  Mod,    // %
  EqEq,   // ==
  BangEq, // !=
  Lt,     // <
  LtEq,   // <=
  Gt,     // >
  GtEq,   // >=
  And,    // and
  Or,     // or
};

enum class UnaryOp : std::uint8_t {
  Negate, // -
  Not,    // !
  Deref,  // *
  AddrOf, // &
};

// ---------------------------------------------------------------------------
// File-level nodes — standalone, not part of any variant.
// ---------------------------------------------------------------------------

struct ModuleNode {
  Span span;
  QualifiedPath path;
};

struct ImportNode {
  Span span;
  QualifiedPath path;
};

struct FileNode {
  Span span;
  // Mandatory leading module declaration. Per
  // CONTRACT_SYNTAX_SURFACE.md, every source file must begin with
  // exactly one `module` declaration. This pointer may be null only
  // when the parser encountered a missing-module error and emitted
  // a diagnostic; downstream passes may treat it as an error file.
  ModuleNode* module_decl = nullptr;
  std::vector<ImportNode*> imports;
  std::vector<Decl*> declarations;
};

// ---------------------------------------------------------------------------
// Class field specifier — standalone, not part of any variant.
// ---------------------------------------------------------------------------

struct FieldSpec {
  Span span;
  std::string_view name;
  Span name_span;
  TypeNode* type;
};

// ---------------------------------------------------------------------------
// Conformance and denial specifiers — used inside class bodies.
// ---------------------------------------------------------------------------

// Conformance block inside a class: `as ConceptName:`
struct ConformanceBlock {
  std::string_view concept_name;
  Span concept_span;
  std::vector<Decl*> methods; // FunctionDecl nodes
};

// Deny statement inside a class: `deny ConceptName`
struct DenySpec {
  std::string_view concept_name;
  Span concept_span;
};

// ---------------------------------------------------------------------------
// Declaration payloads
// ---------------------------------------------------------------------------

struct FunctionDecl {
  std::string_view name;
  Span name_span;
  std::vector<GenericParam> type_params;
  std::vector<Param> params;
  TypeNode* return_type;   // nullable
  std::vector<Stmt*> body; // empty if expression-bodied or extern
  Expr* expr_body;         // nullptr if block-bodied or extern
  bool is_extern = false;

  [[nodiscard]] auto is_expr_bodied() const -> bool {
    return expr_body != nullptr;
  }
};

struct ClassDecl {
  std::string_view name;
  Span name_span;
  std::vector<GenericParam> type_params;
  std::vector<FieldSpec*> fields;
  std::vector<Decl*> methods; // Direct method declarations (FunctionDecl)
  std::vector<ConformanceBlock> conformances;
  std::vector<DenySpec> denials;
};

struct EnumVariantSpec {
  std::string_view name;
  Span name_span;
  std::vector<TypeNode*> payload_types;      // empty for payload-free variants
  std::vector<std::string_view> field_names; // parallel to payload_types (enum class only)
  std::vector<Span> field_name_spans;        // for diagnostics
};

struct EnumDeclNode {
  std::string_view name;
  Span name_span;
  std::vector<GenericParam> type_params;
  std::vector<EnumVariantSpec> variants;
  bool is_enum_class = false;
};

struct AliasDecl {
  std::string_view name;
  Span name_span;
  TypeNode* type;
};

struct ConceptDecl {
  std::string_view name;
  Span name_span;
  std::vector<GenericParam> type_params;
  std::vector<Decl*> methods; // FunctionDecl nodes (bare sigs or defaults)
  bool is_derived = false;
};

struct ExtendDecl {
  TypeNode* target_type;
  std::string_view concept_name;
  Span concept_span;
  std::vector<Decl*> methods; // FunctionDecl nodes
};

// Error recovery placeholders — carry no data; diagnostics are reported
// separately. These allow partial ASTs to flow through downstream passes.
struct ErrorDeclNode {};
struct ErrorStmtNode {};
struct ErrorExprNode {};

using DeclPayload = std::variant<FunctionDecl,
                                 ClassDecl,
                                 EnumDeclNode,
                                 AliasDecl,
                                 ConceptDecl,
                                 ExtendDecl,
                                 ErrorDeclNode>;

// ---------------------------------------------------------------------------
// Statement payloads
// ---------------------------------------------------------------------------

struct LetStatement {
  std::string_view name;
  Span name_span;
  TypeNode* type;    // nullable
  Expr* initializer; // nullable
};

struct Assignment {
  Expr* target;
  Expr* value;
};

struct IfStatement {
  Expr* condition;
  std::vector<Stmt*> then_body;
  std::vector<Stmt*> else_body;

  [[nodiscard]] auto has_else() const -> bool {
    return !else_body.empty();
  }
};

struct WhileStatement {
  Expr* condition;
  std::vector<Stmt*> body;
};

struct ForStatement {
  std::string_view var;
  Span var_span;
  Expr* iterable;
  std::vector<Stmt*> body;
};

struct ModeBlock {
  std::string_view mode_name;
  Span name_span;
  std::vector<Stmt*> body;
};

struct ResourceBlock {
  std::string_view resource_kind;
  Span kind_span;
  std::string_view resource_name;
  Span name_span;
  std::vector<Stmt*> body;
};

struct YieldStatement {
  Expr* value;
};

struct BreakStmtNode {};

struct MatchArm {
  Expr* pattern;                          // constant or qualified enum variant
  std::vector<std::string_view> bindings; // destructuring bindings (empty for payload-free)
  std::vector<Span> binding_spans;        // spans for diagnostics / semantic tokens
  bool has_rest = false;                  // true when `..` appears at end of bindings
  std::string_view as_binding;            // non-empty when `pattern as name:` form is used
  Span as_binding_span;
  std::vector<Stmt*> body;
};

struct MatchStmt {
  Expr* scrutinee;
  std::vector<MatchArm> arms;
};

struct ReturnStatement {
  Expr* value; // nullable for bare return
};

struct ExpressionStatement {
  Expr* expr;
};

using StmtPayload = std::variant<LetStatement,
                                 Assignment,
                                 IfStatement,
                                 WhileStatement,
                                 ForStatement,
                                 YieldStatement,
                                 BreakStmtNode,
                                 MatchStmt,
                                 ModeBlock,
                                 ResourceBlock,
                                 ReturnStatement,
                                 ExpressionStatement,
                                 ErrorStmtNode>;

// ---------------------------------------------------------------------------
// Expression payloads
// ---------------------------------------------------------------------------

struct BinaryExpr {
  BinaryOp op;
  Expr* left;
  Expr* right;
};

struct UnaryExpr {
  UnaryOp op;
  Expr* operand;
};

struct CallExpr {
  Expr* callee;
  std::vector<Expr*> args;
  std::vector<TypeNode*> type_args;          // Explicit type arguments: f<i32>(x)
  std::vector<std::string_view> arg_names;   // parallel to args; empty string = positional
  std::vector<Span> arg_name_spans;          // parallel to args; zero-length unless named
};

struct IndexExpr {
  Expr* object;
  std::vector<Expr*> indices;
};

struct FieldExpr {
  Expr* object;
  std::string_view field;
  Span field_span;
};

struct PipeExpr {
  Expr* left;
  Expr* right;
};

struct TryExpr {
  Expr* operand;
};

struct LambdaExpr {
  std::vector<std::pair<std::string_view, Span>> params;
  Expr* body;
};

struct IntLiteral {
  std::string_view text;
};
struct FloatLiteral {
  std::string_view text;
};
struct StringLiteral {
  std::string_view text;
};
struct BoolLiteral {
  bool value;
};

struct ListLiteral {
  std::vector<Expr*> elements;
};

struct IdentifierExpr {
  std::string_view name;
};

struct QualifiedName {
  std::vector<std::string_view> segments;
  std::vector<Span> segment_spans; // one per segment, see QualifiedPath
};

using ExprPayload = std::variant<BinaryExpr,
                                 UnaryExpr,
                                 CallExpr,
                                 IndexExpr,
                                 FieldExpr,
                                 PipeExpr,
                                 TryExpr,
                                 LambdaExpr,
                                 IntLiteral,
                                 FloatLiteral,
                                 StringLiteral,
                                 BoolLiteral,
                                 ListLiteral,
                                 IdentifierExpr,
                                 QualifiedName,
                                 ErrorExprNode>;

// ---------------------------------------------------------------------------
// Type node payloads
// ---------------------------------------------------------------------------

struct NamedType {
  QualifiedPath name;
  std::vector<TypeNode*> type_args;
};

struct PointerType {
  TypeNode* pointee;
};

struct FunctionTypeNode {
  std::vector<TypeNode*> param_types;
  TypeNode* return_type;
};

using TypeNodePayload = std::variant<NamedType, PointerType, FunctionTypeNode>;

// ---------------------------------------------------------------------------
// Container nodes — arena-allocated.
//
// Each container holds a span and a typed payload variant. The kind()
// method derives NodeKind from the active variant alternative.
// as<T>() / is<T>() provide typed access.
// ---------------------------------------------------------------------------

struct Decl {
  Span span;
  DeclPayload payload;

  [[nodiscard]] auto kind() const -> NodeKind;

  template <typename T> [[nodiscard]] auto as() const -> const T& {
    return std::get<T>(payload);
  }
  template <typename T> [[nodiscard]] auto is() const -> bool {
    return std::holds_alternative<T>(payload);
  }
};

struct Stmt {
  Span span;
  StmtPayload payload;

  [[nodiscard]] auto kind() const -> NodeKind;

  template <typename T> [[nodiscard]] auto as() const -> const T& {
    return std::get<T>(payload);
  }
  template <typename T> [[nodiscard]] auto is() const -> bool {
    return std::holds_alternative<T>(payload);
  }
};

struct Expr {
  Span span;
  ExprPayload payload;

  [[nodiscard]] auto kind() const -> NodeKind;

  template <typename T> [[nodiscard]] auto as() const -> const T& {
    return std::get<T>(payload);
  }
  template <typename T> [[nodiscard]] auto is() const -> bool {
    return std::holds_alternative<T>(payload);
  }
};

struct TypeNode {
  Span span;
  TypeNodePayload payload;

  [[nodiscard]] auto kind() const -> NodeKind;

  template <typename T> [[nodiscard]] auto as() const -> const T& {
    return std::get<T>(payload);
  }
  template <typename T> [[nodiscard]] auto is() const -> bool {
    return std::holds_alternative<T>(payload);
  }
};

// ---------------------------------------------------------------------------
// Convenience: node_kind_name
// ---------------------------------------------------------------------------

auto node_kind_name(NodeKind kind) -> const char*;

} // namespace dao

#endif // DAO_FRONTEND_AST_AST_H
