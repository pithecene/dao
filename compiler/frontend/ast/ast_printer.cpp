#include "frontend/ast/ast_printer.h"

#include "support/op_str.h"
#include "support/variant.h"

#include <string>

namespace dao {

namespace {

// NOLINTBEGIN(readability-identifier-length)

class AstPrinter {
public:
  explicit AstPrinter(std::ostream& out) : out_(out) {
  }

  void print(const FileNode& file) {
    out_ << "File\n";
    if (file.module_decl != nullptr) {
      print_module_decl(*file.module_decl);
    }
    for (const auto* imp : file.imports) {
      print_import(*imp);
    }
    for (const auto* decl : file.declarations) {
      print_decl(*decl);
    }
  }

private:
  std::ostream& out_;
  int depth_ = 1;

  void indent() {
    for (int i = 0; i < depth_; ++i) {
      out_ << "  ";
    }
  }

  struct Scope {
    int& depth;
    explicit Scope(int& d) : depth(d) {
      ++depth;
    }
    ~Scope() {
      --depth;
    }
    Scope(const Scope&) = delete;
    auto operator=(const Scope&) -> Scope& = delete;
    Scope(Scope&&) = delete;
    auto operator=(Scope&&) -> Scope& = delete;
  };

  // -----------------------------------------------------------------------
  // Module declaration
  // -----------------------------------------------------------------------

  void print_module_decl(const ModuleNode& node) {
    indent();
    out_ << "Module ";
    print_qualified_path(node.path);
    out_ << "\n";
  }

  // -----------------------------------------------------------------------
  // Import
  // -----------------------------------------------------------------------

  void print_import(const ImportNode& node) {
    indent();
    out_ << "Import ";
    print_qualified_path(node.path);
    out_ << "\n";
  }

  // -----------------------------------------------------------------------
  // Declarations
  // -----------------------------------------------------------------------

  void print_decl(const Decl& decl) {
    std::visit(overloaded{
                   [&](const FunctionDecl& fn) { print_function_decl(fn); },
                   [&](const ClassDecl& cls) { print_class_decl(cls); },
                   [&](const EnumDeclNode& en) { print_enum_decl(en); },
                   [&](const AliasDecl& alias) { print_alias_decl(alias); },
                   [&](const ConceptDecl& concept_) { print_concept_decl(concept_); },
                   [&](const ExtendDecl& ext) { print_extend_decl(ext); },
                   [](const ErrorDeclNode&) {},
               },
               decl.payload);
  }

  void print_type_params(const std::vector<GenericParam>& type_params) {
    if (type_params.empty()) {
      return;
    }
    out_ << "<";
    for (size_t i = 0; i < type_params.size(); ++i) {
      if (i > 0) {
        out_ << ", ";
      }
      out_ << type_params[i].name;
      if (!type_params[i].constraints.empty()) {
        out_ << ": ";
        for (size_t j = 0; j < type_params[i].constraints.size(); ++j) {
          if (j > 0) {
            out_ << " + ";
          }
          print_type_inline(*type_params[i].constraints[j]);
        }
      }
    }
    out_ << ">";
  }

  void print_function_decl(const FunctionDecl& fn) {
    indent();
    if (fn.is_extern) {
      out_ << "ExternFunctionDecl " << fn.name;
    } else {
      out_ << "FunctionDecl " << fn.name;
    }
    print_type_params(fn.type_params);
    out_ << "\n";
    Scope scope(depth_);

    for (const auto& param : fn.params) {
      indent();
      if (param.type != nullptr) {
        out_ << "Param " << param.name << ": ";
        print_type_inline(*param.type);
      } else {
        out_ << "Param " << param.name;
      }
      out_ << "\n";
    }

    if (fn.return_type != nullptr) {
      indent();
      out_ << "ReturnType: ";
      print_type_inline(*fn.return_type);
      out_ << "\n";
    }

    if (fn.is_expr_bodied()) {
      indent();
      out_ << "ExprBody\n";
      {
        Scope body_scope(depth_);
        print_expr(*fn.expr_body);
      }
    } else {
      for (const auto* stmt : fn.body) {
        print_stmt(*stmt);
      }
    }
  }

  void print_class_decl(const ClassDecl& node) {
    indent();
    out_ << "ClassDecl " << node.name;
    print_type_params(node.type_params);
    out_ << "\n";
    Scope scope(depth_);
    for (const auto* field : node.fields) {
      indent();
      out_ << "Field " << field->name << ": ";
      print_type_inline(*field->type);
      out_ << "\n";
    }
    for (const auto* method : node.methods) {
      print_decl(*method);
    }
    for (const auto& conf : node.conformances) {
      indent();
      out_ << "Conformance " << conf.target.concept_name << "\n";
      Scope conf_scope(depth_);
      for (const auto* method : conf.methods) {
        print_decl(*method);
      }
    }
    for (const auto& deny : node.denials) {
      indent();
      out_ << "Deny " << deny.target.concept_name << "\n";
    }
  }

  void print_enum_decl(const EnumDeclNode& node) {
    indent();
    if (node.is_enum_class) {
      out_ << "EnumClassDecl " << node.name;
    } else {
      out_ << "EnumDecl " << node.name;
    }
    print_type_params(node.type_params);
    out_ << "\n";
    Scope scope(depth_);
    for (const auto& variant : node.variants) {
      indent();
      out_ << "Variant " << variant.name << "\n";
    }
  }

  void print_concept_decl(const ConceptDecl& node) {
    indent();
    if (node.is_derived) {
      out_ << "DerivedConceptDecl " << node.name;
    } else {
      out_ << "ConceptDecl " << node.name;
    }
    print_type_params(node.type_params);
    out_ << "\n";
    Scope scope(depth_);
    for (const auto* method : node.methods) {
      print_decl(*method);
    }
  }

  void print_extend_decl(const ExtendDecl& node) {
    indent();
    out_ << "ExtendDecl ";
    print_type_inline(*node.target_type);
    out_ << " as " << node.target.concept_name << "\n";
    Scope scope(depth_);
    for (const auto* method : node.methods) {
      print_decl(*method);
    }
  }

  void print_alias_decl(const AliasDecl& node) {
    indent();
    out_ << "AliasDecl " << node.name << " = ";
    print_type_inline(*node.type);
    out_ << "\n";
  }

  // -----------------------------------------------------------------------
  // Statements
  // -----------------------------------------------------------------------

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void print_stmt(const Stmt& stmt) {
    std::visit(overloaded{
                   [&](const LetStatement& node) {
                     indent();
                     out_ << "LetStatement " << node.name;
                     if (node.type != nullptr) {
                       out_ << ": ";
                       print_type_inline(*node.type);
                     }
                     out_ << "\n";
                     if (node.initializer != nullptr) {
                       Scope scope(depth_);
                       print_expr(*node.initializer);
                     }
                   },
                   [&](const Assignment& node) {
                     indent();
                     out_ << "Assignment\n";
                     Scope scope(depth_);
                     indent();
                     out_ << "Target\n";
                     {
                       Scope target_scope(depth_);
                       print_expr(*node.target);
                     }
                     indent();
                     out_ << "Value\n";
                     {
                       Scope value_scope(depth_);
                       print_expr(*node.value);
                     }
                   },
                   [&](const IfStatement& node) {
                     indent();
                     out_ << "IfStatement\n";
                     Scope scope(depth_);
                     indent();
                     out_ << "Condition\n";
                     {
                       Scope cond_scope(depth_);
                       print_expr(*node.condition);
                     }
                     indent();
                     out_ << "Then\n";
                     {
                       Scope then_scope(depth_);
                       for (const auto* s : node.then_body) {
                         print_stmt(*s);
                       }
                     }
                     if (node.has_else()) {
                       indent();
                       out_ << "Else\n";
                       Scope else_scope(depth_);
                       for (const auto* s : node.else_body) {
                         print_stmt(*s);
                       }
                     }
                   },
                   [&](const WhileStatement& node) {
                     indent();
                     out_ << "WhileStatement\n";
                     Scope scope(depth_);
                     indent();
                     out_ << "Condition\n";
                     {
                       Scope cond_scope(depth_);
                       print_expr(*node.condition);
                     }
                     for (const auto* s : node.body) {
                       print_stmt(*s);
                     }
                   },
                   [&](const ForStatement& node) {
                     indent();
                     out_ << "ForStatement " << node.var << "\n";
                     Scope scope(depth_);
                     indent();
                     out_ << "Iterable\n";
                     {
                       Scope iter_scope(depth_);
                       print_expr(*node.iterable);
                     }
                     for (const auto* s : node.body) {
                       print_stmt(*s);
                     }
                   },
                   [&](const ModeBlock& node) {
                     indent();
                     out_ << "ModeBlock " << node.mode_name << "\n";
                     Scope scope(depth_);
                     for (const auto* s : node.body) {
                       print_stmt(*s);
                     }
                   },
                   [&](const ResourceBlock& node) {
                     indent();
                     out_ << "ResourceBlock " << node.resource_kind << " " << node.resource_name
                          << "\n";
                     Scope scope(depth_);
                     for (const auto* s : node.body) {
                       print_stmt(*s);
                     }
                   },
                   [&](const YieldStatement& node) {
                     indent();
                     out_ << "YieldStatement\n";
                     Scope scope(depth_);
                     print_expr(*node.value);
                   },
                   [&](const BreakStmtNode&) {
                     indent();
                     out_ << "BreakStatement\n";
                   },
                   [&](const MatchStmt& node) {
                     indent();
                     out_ << "MatchStatement\n";
                     {
                       Scope scope(depth_);
                       indent();
                       out_ << "Scrutinee\n";
                       {
                         Scope inner(depth_);
                         print_expr(*node.scrutinee);
                       }
                       for (const auto& arm : node.arms) {
                         indent();
                         out_ << "Arm\n";
                         {
                           Scope arm_scope(depth_);
                           indent();
                           out_ << "Pattern\n";
                           {
                             Scope pat(depth_);
                             print_expr(*arm.pattern);
                           }
                           for (const auto* s : arm.body) {
                             print_stmt(*s);
                           }
                         }
                       }
                     }
                   },
                   [&](const ReturnStatement& node) {
                     indent();
                     out_ << "ReturnStatement\n";
                     if (node.value != nullptr) {
                       Scope scope(depth_);
                       print_expr(*node.value);
                     }
                   },
                   [&](const ExpressionStatement& node) {
                     indent();
                     out_ << "ExpressionStatement\n";
                     Scope scope(depth_);
                     print_expr(*node.expr);
                   },
                   [](const ErrorStmtNode&) {},
               },
               stmt.payload);
  }

  // -----------------------------------------------------------------------
  // Expressions
  // -----------------------------------------------------------------------

  // binary_op_str / unary_op_str: see support/op_str.h

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void print_expr(const Expr& expr) {
    std::visit(overloaded{
                   [&](const IntLiteral& node) {
                     indent();
                     out_ << "IntLiteral " << node.text << "\n";
                   },
                   [&](const FloatLiteral& node) {
                     indent();
                     out_ << "FloatLiteral " << node.text << "\n";
                   },
                   [&](const StringLiteral& node) {
                     indent();
                     out_ << "StringLiteral " << node.text << "\n";
                   },
                   [&](const BoolLiteral& node) {
                     indent();
                     out_ << "BoolLiteral " << (node.value ? "true" : "false") << "\n";
                   },
                   [&](const IdentifierExpr& node) {
                     indent();
                     out_ << "Identifier " << node.name << "\n";
                   },
                   [&](const QualifiedName& node) {
                     indent();
                     out_ << "QualifiedName ";
                     for (size_t i = 0; i < node.segments.size(); ++i) {
                       if (i > 0) {
                         out_ << "::";
                       }
                       out_ << node.segments[i];
                     }
                     out_ << "\n";
                   },
                   [&](const BinaryExpr& node) {
                     indent();
                     out_ << "BinaryExpr " << binary_op_str(node.op) << "\n";
                     Scope scope(depth_);
                     print_expr(*node.left);
                     print_expr(*node.right);
                   },
                   [&](const UnaryExpr& node) {
                     indent();
                     out_ << "UnaryExpr " << unary_op_str(node.op) << "\n";
                     Scope scope(depth_);
                     print_expr(*node.operand);
                   },
                   [&](const CallExpr& node) {
                     indent();
                     out_ << "CallExpr\n";
                     Scope scope(depth_);
                     indent();
                     out_ << "Callee\n";
                     {
                       Scope callee_scope(depth_);
                       print_expr(*node.callee);
                     }
                     if (!node.type_args.empty()) {
                       indent();
                       out_ << "TypeArgs\n";
                       Scope ta_scope(depth_);
                       for (const auto* ta : node.type_args) {
                         indent();
                         print_type_inline(*ta);
                         out_ << "\n";
                       }
                     }
                     if (!node.args.empty()) {
                       indent();
                       out_ << "Args\n";
                       Scope args_scope(depth_);
                       for (const auto* arg : node.args) {
                         print_expr(*arg);
                       }
                     }
                   },
                   [&](const IndexExpr& node) {
                     indent();
                     out_ << "IndexExpr\n";
                     Scope scope(depth_);
                     print_expr(*node.object);
                     indent();
                     out_ << "Indices\n";
                     {
                       Scope indices_scope(depth_);
                       for (const auto* idx : node.indices) {
                         print_expr(*idx);
                       }
                     }
                   },
                   [&](const FieldExpr& node) {
                     indent();
                     out_ << "FieldExpr ." << node.field << "\n";
                     Scope scope(depth_);
                     print_expr(*node.object);
                   },
                   [&](const PipeExpr& node) {
                     indent();
                     out_ << "PipeExpr\n";
                     Scope scope(depth_);
                     print_expr(*node.left);
                     print_expr(*node.right);
                   },
                   [&](const TryExpr& node) {
                     indent();
                     out_ << "TryExpr\n";
                     Scope scope(depth_);
                     print_expr(*node.operand);
                   },
                   [&](const LambdaExpr& node) {
                     indent();
                     out_ << "Lambda";
                     for (const auto& [name, span] : node.params) {
                       out_ << " " << name;
                     }
                     out_ << "\n";
                     Scope scope(depth_);
                     print_expr(*node.body);
                   },
                   [&](const ListLiteral& node) {
                     indent();
                     out_ << "ListLiteral\n";
                     if (!node.elements.empty()) {
                       Scope scope(depth_);
                       for (const auto* elem : node.elements) {
                         print_expr(*elem);
                       }
                     }
                   },
                   [](const ErrorExprNode&) {},
               },
               expr.payload);
  }

  // -----------------------------------------------------------------------
  // Types (inline rendering)
  // -----------------------------------------------------------------------

  void print_type_inline(const TypeNode& type) {
    std::visit(overloaded{
                   [&](const NamedType& named) {
                     print_qualified_path(named.name);
                     if (!named.type_args.empty()) {
                       out_ << "<";
                       for (size_t i = 0; i < named.type_args.size(); ++i) {
                         if (i > 0) {
                           out_ << ", ";
                         }
                         print_type_inline(*named.type_args[i]);
                       }
                       out_ << ">";
                     }
                   },
                   [&](const PointerType& ptr) {
                     out_ << "*";
                     print_type_inline(*ptr.pointee);
                   },
                   [&](const FunctionTypeNode& ftn) {
                     out_ << "fn(";
                     for (size_t i = 0; i < ftn.param_types.size(); ++i) {
                       if (i > 0) {
                         out_ << ", ";
                       }
                       print_type_inline(*ftn.param_types[i]);
                     }
                     out_ << "): ";
                     print_type_inline(*ftn.return_type);
                   },
               },
               type.payload);
  }

  void print_qualified_path(const QualifiedPath& path) {
    for (size_t i = 0; i < path.segments.size(); ++i) {
      if (i > 0) {
        out_ << "::";
      }
      out_ << path.segments[i];
    }
  }
};

// NOLINTEND(readability-identifier-length)

} // namespace

void print_ast(std::ostream& out, const FileNode& file) {
  AstPrinter printer(out);
  printer.print(file);
}

} // namespace dao
