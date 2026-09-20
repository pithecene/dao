#include "frontend/ast/ast.h"

namespace dao {

// ---------------------------------------------------------------------------
// kind() — derived from active variant alternative.
// ---------------------------------------------------------------------------

auto Decl::kind() const -> NodeKind {
  return std::visit(overloaded{
                        [](const FunctionDecl&) { return NodeKind::FunctionDecl; },
                        [](const ClassDecl&) { return NodeKind::ClassDecl; },
                        [](const EnumDeclNode&) { return NodeKind::EnumDecl; },
                        [](const AliasDecl&) { return NodeKind::AliasDecl; },
                        [](const ConceptDecl&) { return NodeKind::ConceptDecl; },
                        [](const ExtendDecl&) { return NodeKind::ExtendDecl; },
                        [](const ErrorDeclNode&) { return NodeKind::ErrorDecl; },
                    },
                    payload);
}

auto Stmt::kind() const -> NodeKind {
  return std::visit(overloaded{
                        [](const LetStatement&) { return NodeKind::LetStatement; },
                        [](const Assignment&) { return NodeKind::Assignment; },
                        [](const IfStatement&) { return NodeKind::IfStatement; },
                        [](const WhileStatement&) { return NodeKind::WhileStatement; },
                        [](const ForStatement&) { return NodeKind::ForStatement; },
                        [](const YieldStatement&) { return NodeKind::YieldStatement; },
                        [](const BreakStmtNode&) { return NodeKind::BreakStatement; },
                        [](const MatchStmt&) { return NodeKind::MatchStatement; },
                        [](const ModeBlock&) { return NodeKind::ModeBlock; },
                        [](const ResourceBlock&) { return NodeKind::ResourceBlock; },
                        [](const ReturnStatement&) { return NodeKind::ReturnStatement; },
                        [](const ExpressionStatement&) { return NodeKind::ExpressionStatement; },
                        [](const ErrorStmtNode&) { return NodeKind::ErrorStmt; },
                    },
                    payload);
}

auto Expr::kind() const -> NodeKind {
  return std::visit(overloaded{
                        [](const BinaryExpr&) { return NodeKind::BinaryExpr; },
                        [](const UnaryExpr&) { return NodeKind::UnaryExpr; },
                        [](const CallExpr&) { return NodeKind::CallExpr; },
                        [](const IndexExpr&) { return NodeKind::IndexExpr; },
                        [](const FieldExpr&) { return NodeKind::FieldExpr; },
                        [](const PipeExpr&) { return NodeKind::PipeExpr; },
                        [](const TryExpr&) { return NodeKind::TryExpr; },
                        [](const LambdaExpr&) { return NodeKind::Lambda; },
                        [](const IntLiteral&) { return NodeKind::IntLiteral; },
                        [](const FloatLiteral&) { return NodeKind::FloatLiteral; },
                        [](const StringLiteral&) { return NodeKind::StringLiteral; },
                        [](const BoolLiteral&) { return NodeKind::BoolLiteral; },
                        [](const ListLiteral&) { return NodeKind::ListLiteral; },
                        [](const IdentifierExpr&) { return NodeKind::Identifier; },
                        [](const QualifiedName&) { return NodeKind::QualifiedName; },
                        [](const ErrorExprNode&) { return NodeKind::ErrorExpr; },
                    },
                    payload);
}

auto TypeNode::kind() const -> NodeKind {
  return std::visit(overloaded{
                        [](const NamedType&) { return NodeKind::NamedType; },
                        [](const FunctionTypeNode&) { return NodeKind::FunctionType; },
                    },
                    payload);
}

// ---------------------------------------------------------------------------
// node_kind_name
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto node_kind_name(NodeKind kind) -> const char* {
  switch (kind) {
  case NodeKind::File:
    return "File";
  case NodeKind::ModuleDecl:
    return "ModuleDecl";
  case NodeKind::Import:
    return "Import";
  case NodeKind::FunctionDecl:
    return "FunctionDecl";
  case NodeKind::ClassDecl:
    return "ClassDecl";
  case NodeKind::EnumDecl:
    return "EnumDecl";
  case NodeKind::AliasDecl:
    return "AliasDecl";
  case NodeKind::ConceptDecl:
    return "ConceptDecl";
  case NodeKind::ExtendDecl:
    return "ExtendDecl";
  case NodeKind::FieldSpec:
    return "FieldSpec";
  case NodeKind::LetStatement:
    return "LetStatement";
  case NodeKind::Assignment:
    return "Assignment";
  case NodeKind::IfStatement:
    return "IfStatement";
  case NodeKind::WhileStatement:
    return "WhileStatement";
  case NodeKind::ForStatement:
    return "ForStatement";
  case NodeKind::YieldStatement:
    return "YieldStatement";
  case NodeKind::BreakStatement:
    return "BreakStatement";
  case NodeKind::MatchStatement:
    return "MatchStatement";
  case NodeKind::ModeBlock:
    return "ModeBlock";
  case NodeKind::ResourceBlock:
    return "ResourceBlock";
  case NodeKind::ReturnStatement:
    return "ReturnStatement";
  case NodeKind::ExpressionStatement:
    return "ExpressionStatement";
  case NodeKind::BinaryExpr:
    return "BinaryExpr";
  case NodeKind::UnaryExpr:
    return "UnaryExpr";
  case NodeKind::CallExpr:
    return "CallExpr";
  case NodeKind::IndexExpr:
    return "IndexExpr";
  case NodeKind::FieldExpr:
    return "FieldExpr";
  case NodeKind::PipeExpr:
    return "PipeExpr";
  case NodeKind::TryExpr:
    return "TryExpr";
  case NodeKind::Lambda:
    return "Lambda";
  case NodeKind::IntLiteral:
    return "IntLiteral";
  case NodeKind::FloatLiteral:
    return "FloatLiteral";
  case NodeKind::StringLiteral:
    return "StringLiteral";
  case NodeKind::BoolLiteral:
    return "BoolLiteral";
  case NodeKind::ListLiteral:
    return "ListLiteral";
  case NodeKind::Identifier:
    return "Identifier";
  case NodeKind::QualifiedName:
    return "QualifiedName";
  case NodeKind::NamedType:
    return "NamedType";
  case NodeKind::FunctionType:
    return "FunctionType";
  case NodeKind::ErrorExpr:
    return "ErrorExpr";
  case NodeKind::ErrorStmt:
    return "ErrorStmt";
  case NodeKind::ErrorDecl:
    return "ErrorDecl";
  }
  return "Unknown";
}

} // namespace dao
