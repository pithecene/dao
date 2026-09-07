#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "support/test_utils.h"

#define BOOST_UT_DISABLE_MODULE
#include <boost/ut.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace {

using namespace boost::ut;
using namespace dao;

struct ParseOutput {
  std::unique_ptr<SourceBuffer> source;
  LexResult lex_result;
  ParseResult parse_result;
};

// Test helper: CONTRACT_SYNTAX_SURFACE.md requires every source file to
// begin with a `module` declaration. Fixtures in this test file focus
// on parser behavior below the module layer, so we prepend a canonical
// `module test` line to the supplied source before lexing. This keeps
// fixtures focused on what each test is actually exercising while
// still honoring the parser's leading-declaration requirement.
//
// The helper is idempotent: fixtures that already begin with a
// `module` declaration (e.g. the module_tests suite) are passed through
// unchanged so their own module header remains authoritative.
auto parse_string(std::string_view src) -> ParseOutput {
  std::string wrapped;
  if (dao::starts_with_module(src)) {
    wrapped.assign(src);
  } else {
    wrapped = "module test\n";
    wrapped.append(src);
  }
  auto source = std::make_unique<SourceBuffer>("<test>", std::move(wrapped));
  auto lex_result = lex(*source);
  auto parse_result = parse(lex_result.tokens);
  return {.source = std::move(source),
          .lex_result = std::move(lex_result),
          .parse_result = std::move(parse_result)};
}

auto parse_file(const std::filesystem::path& path) -> ParseOutput {
  auto contents = read_file(path);
  auto source = std::make_unique<SourceBuffer>(path.filename().string(), contents);
  auto lex_result = lex(*source);
  auto parse_result = parse(lex_result.tokens);
  return {.source = std::move(source),
          .lex_result = std::move(lex_result),
          .parse_result = std::move(parse_result)};
}

// NOLINTBEGIN(readability-function-cognitive-complexity,readability-magic-numbers,modernize-use-trailing-return-type)

suite<"function_tests"> function_tests = [] {
  "expression-bodied function"_test = [] {
    auto output = parse_string("fn add(a: i32, b: i32): i32 -> a + b\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    auto* file = output.parse_result.file;
    expect(file != nullptr);
    expect(file->declarations.size() == 1_u);

    auto* decl0 = file->declarations[0];
    const auto& fn = decl0->as<FunctionDecl>();
    expect(decl0->kind() == NodeKind::FunctionDecl);
    expect(fn.name == "add");
    expect(fn.params.size() == 2_u);
    expect(fn.params[0].name == "a");
    expect(fn.params[1].name == "b");
    expect(fn.return_type != nullptr);
    expect(fn.is_expr_bodied());
    expect(fn.expr_body != nullptr);
    expect(fn.expr_body->kind() == NodeKind::BinaryExpr);
  };

  "block-bodied function"_test = [] {
    auto output = parse_string("fn main(): i32\n    0\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    auto* file = output.parse_result.file;
    expect(file->declarations.size() == 1_u);

    auto* decl0 = file->declarations[0];
    const auto& fn = decl0->as<FunctionDecl>();
    expect(!fn.is_expr_bodied());
    expect(fn.body.size() == 1_u);
    expect(fn.body[0]->kind() == NodeKind::ExpressionStatement);
  };

  "no-param function"_test = [] {
    auto output = parse_string("fn greet(): i32 -> 0\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    expect(fn.params.empty());
  };
};

suite<"let_tests"> let_tests = [] {
  "let with type and initializer"_test = [] {
    auto output = parse_string("fn f(): i32\n    let x: i32 = 42\n    x\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    expect(fn.body.size() == 2_u);

    const auto& let_stmt = fn.body[0]->as<LetStatement>();
    expect(fn.body[0]->kind() == NodeKind::LetStatement);
    expect(let_stmt.name == "x");
    expect(let_stmt.type != nullptr);
    expect(let_stmt.initializer != nullptr);
  };

  "let with type only"_test = [] {
    auto output = parse_string("fn f(): i32\n    let x: i32\n    x\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& let_stmt = fn.body[0]->as<LetStatement>();
    expect(let_stmt.type != nullptr);
    expect(let_stmt.initializer == nullptr);
  };

  "let with initializer only"_test = [] {
    auto output = parse_string("fn f(): i32\n    let x = 42\n    x\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& let_stmt = fn.body[0]->as<LetStatement>();
    expect(let_stmt.type == nullptr);
    expect(let_stmt.initializer != nullptr);
  };
};

suite<"control_flow_tests"> control_flow_tests = [] {
  "if statement"_test = [] {
    auto output = parse_string("fn f(): i32\n    if true:\n        0\n    1\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& if_stmt = fn.body[0]->as<IfStatement>();
    expect(fn.body[0]->kind() == NodeKind::IfStatement);
    expect(if_stmt.condition->kind() == NodeKind::BoolLiteral);
    expect(if_stmt.then_body.size() == 1_u);
    expect(!if_stmt.has_else());
  };

  "if-else statement"_test = [] {
    auto output = parse_string("fn f(): i32\n    if true:\n        0\n    else:\n        1\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& if_stmt = fn.body[0]->as<IfStatement>();
    expect(if_stmt.has_else());
    expect(if_stmt.else_body.size() == 1_u);
  };

  "while statement"_test = [] {
    auto output = parse_string("fn f(): i32\n    while true:\n        0\n    1\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& while_stmt = fn.body[0]->as<WhileStatement>();
    expect(fn.body[0]->kind() == NodeKind::WhileStatement);
    expect(while_stmt.body.size() == 1_u);
  };

  "for statement"_test = [] {
    auto output = parse_string("fn f(): i32\n    for i in items:\n        i\n    0\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& for_stmt = fn.body[0]->as<ForStatement>();
    expect(fn.body[0]->kind() == NodeKind::ForStatement);
    expect(for_stmt.var == "i");
  };
};

suite<"expression_tests"> expression_tests = [] {
  "binary operators"_test = [] {
    auto output = parse_string("fn f(): i32 -> 1 + 2 * 3\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    // Should be BinaryExpr(+, 1, BinaryExpr(*, 2, 3)) — * binds tighter than +
    const auto& add = fn.expr_body->as<BinaryExpr>();
    expect(fn.expr_body->kind() == NodeKind::BinaryExpr);
    expect(add.op == BinaryOp::Add);
    expect(add.left->kind() == NodeKind::IntLiteral);
    expect(add.right->kind() == NodeKind::BinaryExpr);
    const auto& mul = add.right->as<BinaryExpr>();
    expect(mul.op == BinaryOp::Mul);
  };

  "unary operators"_test = [] {
    auto output = parse_string("fn f(p: *i32): i32 -> *p\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& deref = fn.expr_body->as<UnaryExpr>();
    expect(fn.expr_body->kind() == NodeKind::UnaryExpr);
    expect(deref.op == UnaryOp::Deref);
  };

  "function call"_test = [] {
    auto output = parse_string("fn f(): i32\n    foo(1, 2)\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& expr_stmt = fn.body[0]->as<ExpressionStatement>();
    const auto& call = expr_stmt.expr->as<CallExpr>();
    expect(expr_stmt.expr->kind() == NodeKind::CallExpr);
    expect(call.args.size() == 2_u);
  };

  "field access"_test = [] {
    auto output = parse_string("fn f(): i32\n    x.y.z\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& expr_stmt = fn.body[0]->as<ExpressionStatement>();
    const auto& outer = expr_stmt.expr->as<FieldExpr>();
    expect(expr_stmt.expr->kind() == NodeKind::FieldExpr);
    expect(outer.field == "z");
    const auto& inner = outer.object->as<FieldExpr>();
    expect(inner.field == "y");
  };

  "index expression"_test = [] {
    auto output = parse_string("fn f(): i32\n    a[0]\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& expr_stmt = fn.body[0]->as<ExpressionStatement>();
    const auto& idx = expr_stmt.expr->as<IndexExpr>();
    expect(expr_stmt.expr->kind() == NodeKind::IndexExpr);
    expect(idx.indices.size() == 1_u);
  };

  "multi-arg bracket"_test = [] {
    auto output = parse_string("fn f(): i32\n    Map[i32, string]()\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& expr_stmt = fn.body[0]->as<ExpressionStatement>();
    const auto& call = expr_stmt.expr->as<CallExpr>();
    expect(expr_stmt.expr->kind() == NodeKind::CallExpr);
    const auto& idx = call.callee->as<IndexExpr>();
    expect(idx.indices.size() == 2_u);
  };

  "comparison operators"_test = [] {
    auto output = parse_string("fn f(): i32 -> 1 == 2\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& eq = fn.expr_body->as<BinaryExpr>();
    expect(eq.op == BinaryOp::EqEq);
  };

  "logical operators"_test = [] {
    auto output = parse_string("fn f(): i32 -> true and false or true\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    // or binds less tightly than and: or(and(true, false), true)
    const auto& or_expr = fn.expr_body->as<BinaryExpr>();
    expect(or_expr.op == BinaryOp::Or);
    const auto& and_expr = or_expr.left->as<BinaryExpr>();
    expect(and_expr.op == BinaryOp::And);
  };
};

suite<"lambda_tests"> lambda_tests = [] {
  "simple lambda"_test = [] {
    auto output = parse_string("fn f(): i32 -> |x| -> x + 1\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
  };

  "lambda in pipe"_test = [] {
    auto output = parse_string("fn f(): i32\n    xs |> map |x| -> x * x\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& expr_stmt = fn.body[0]->as<ExpressionStatement>();
    expect(expr_stmt.expr->kind() == NodeKind::PipeExpr);
  };
};

suite<"pipe_tests"> pipe_tests = [] {
  "simple pipe"_test = [] {
    auto output = parse_string("fn f(): i32\n    x |> foo |> bar\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& expr_stmt = fn.body[0]->as<ExpressionStatement>();
    const auto& outer = expr_stmt.expr->as<PipeExpr>();
    expect(expr_stmt.expr->kind() == NodeKind::PipeExpr);
    expect(outer.left->kind() == NodeKind::PipeExpr);
  };

  "multi-line pipe in suite"_test = [] {
    auto output = parse_string("fn f(): i32\n    xs\n        |> map |x| -> x\n    0\n");
    expect(output.parse_result.diagnostics.empty()) << "multi-line pipe in suite";
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    expect(fn.body.size() == 2_u) << "pipe expr + trailing 0";
    const auto& expr_stmt = fn.body[0]->as<ExpressionStatement>();
    expect(expr_stmt.expr->kind() == NodeKind::PipeExpr);
  };
};

suite<"mode_resource_tests"> mode_resource_tests = [] {
  "mode block"_test = [] {
    auto output = parse_string("fn f(): i32\n    mode unsafe =>\n        0\n    1\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& mode = fn.body[0]->as<ModeBlock>();
    expect(fn.body[0]->kind() == NodeKind::ModeBlock);
    expect(mode.mode_name == "unsafe");
    expect(mode.body.size() == 1_u);
  };

  "resource block"_test = [] {
    auto output = parse_string("fn f(): i32\n    resource memory Search =>\n        0\n    1\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& res = fn.body[0]->as<ResourceBlock>();
    expect(fn.body[0]->kind() == NodeKind::ResourceBlock);
    expect(res.resource_kind == "memory");
    expect(res.resource_name == "Search");
    expect(res.body.size() == 1_u);
  };
};

suite<"type_tests"> type_tests = [] {
  "pointer type"_test = [] {
    auto output = parse_string("fn f(p: *i32): i32 -> 0\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    auto* param_type = fn.params[0].type;
    expect(param_type->kind() == NodeKind::PointerType);
  };

  "parameterized type"_test = [] {
    auto output = parse_string("fn f(): List<i32> -> []\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& ret = fn.return_type->as<NamedType>();
    expect(fn.return_type->kind() == NodeKind::NamedType);
    expect(ret.name.segments.size() == 1_u);
    expect(ret.name.segments[0] == "List");
    expect(ret.type_args.size() == 1_u);
  };

  "multi-param type"_test = [] {
    auto output = parse_string("fn f(): Map<i32, string> -> []\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& ret = fn.return_type->as<NamedType>();
    expect(ret.type_args.size() == 2_u);
  };
};

suite<"generic_tests"> generic_tests = [] {
  "generic function with one type param"_test = [] {
    auto output = parse_string("fn identity<T>(x: T): T -> x\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    expect(fn.name == "identity");
    expect(fn.type_params.size() == 1_u);
    expect(fn.type_params[0].name == "T");
    expect(fn.type_params[0].constraints.empty());
  };

  "generic function with constrained type param"_test = [] {
    auto output = parse_string("fn print<T: Printable>(value: T): void -> value\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    expect(fn.type_params.size() == 1_u);
    expect(fn.type_params[0].name == "T");
    expect(fn.type_params[0].constraints.size() == 1_u);
  };

  "generic function with multiple constraints"_test = [] {
    auto output = parse_string("fn sort<T: Comparable + Printable>(x: T): T -> x\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    expect(fn.type_params.size() == 1_u);
    expect(fn.type_params[0].constraints.size() == 2_u);
  };

  "generic function with multiple type params"_test = [] {
    auto output = parse_string("fn pair<A, B>(a: A, b: B): i32 -> 0\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    expect(fn.type_params.size() == 2_u);
    expect(fn.type_params[0].name == "A");
    expect(fn.type_params[1].name == "B");
  };

  "generic class"_test = [] {
    auto output = parse_string("class Box<T>:\n    value: T\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& cls = output.parse_result.file->declarations[0]->as<ClassDecl>();
    expect(cls.name == "Box");
    expect(cls.type_params.size() == 1_u);
    expect(cls.type_params[0].name == "T");
    expect(cls.fields.size() == 1_u);
  };

  "non-generic function has empty type params"_test = [] {
    auto output = parse_string("fn add(a: i32, b: i32): i32 -> a + b\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    expect(fn.type_params.empty());
  };
};

suite<"import_tests"> import_tests = [] {
  "simple import"_test = [] {
    auto output = parse_string("import foo\nfn f(): i32 -> 0\n");
    expect(output.parse_result.diagnostics.empty());
    expect(output.parse_result.file->imports.size() == 1_u);
    auto* imp = output.parse_result.file->imports[0];
    expect(imp->path.segments.size() == 1_u);
    expect(imp->path.segments[0] == "foo");
  };

  "qualified import"_test = [] {
    auto output = parse_string("import net::http\nfn f(): i32 -> 0\n");
    expect(output.parse_result.diagnostics.empty());
    auto* imp = output.parse_result.file->imports[0];
    expect(imp->path.segments.size() == 2_u);
    expect(imp->path.segments[0] == "net");
    expect(imp->path.segments[1] == "http");
  };
};

suite<"module_tests"> module_tests = [] {
  "simple module declaration"_test = [] {
    auto output = parse_string("module app\nfn f(): i32 -> 0\n");
    expect(output.parse_result.diagnostics.empty());
    expect(output.parse_result.file->module_decl != nullptr);
    auto* mod = output.parse_result.file->module_decl;
    expect(mod->path.segments.size() == 1_u);
    expect(mod->path.segments[0] == "app");
  };

  "qualified module declaration"_test = [] {
    auto output = parse_string("module app::math\nfn f(): i32 -> 0\n");
    expect(output.parse_result.diagnostics.empty());
    auto* mod = output.parse_result.file->module_decl;
    expect(mod->path.segments.size() == 2_u);
    expect(mod->path.segments[0] == "app");
    expect(mod->path.segments[1] == "math");
  };

  "module with imports and declarations"_test = [] {
    auto output = parse_string(
        "module app::main\nimport core::fmt\nimport app::math\nfn f(): i32 -> 0\n");
    expect(output.parse_result.diagnostics.empty());
    expect(output.parse_result.file->module_decl != nullptr);
    expect(output.parse_result.file->imports.size() == 2_u);
    expect(output.parse_result.file->declarations.size() == 1_u);
  };

  "missing module declaration is reported"_test = [] {
    // Bypass the parse_string wrapper's auto-prepending of
    // `module test` so we can exercise the parser's enforcement of
    // the "every file begins with a module declaration" rule
    // frozen in CONTRACT_SYNTAX_SURFACE.md.
    auto source = std::make_unique<SourceBuffer>("<test>", std::string("fn f(): i32 -> 0\n"));
    auto lex_result = lex(*source);
    auto parse_result = parse(lex_result.tokens);

    bool has_missing_module = false;
    for (const auto& diag : parse_result.diagnostics) {
      if (diag.message.find("module") != std::string::npos) {
        has_missing_module = true;
        break;
      }
    }
    expect(has_missing_module) << "expected a diagnostic about the missing module declaration";
    expect(parse_result.file != nullptr);
    expect(parse_result.file->module_decl == nullptr);
  };
};

suite<"assignment_tests"> assignment_tests = [] {
  "simple assignment"_test = [] {
    auto output = parse_string("fn f(): i32\n    x = 42\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& assign = fn.body[0]->as<Assignment>();
    expect(fn.body[0]->kind() == NodeKind::Assignment);
    expect(assign.target->kind() == NodeKind::Identifier);
    expect(assign.value->kind() == NodeKind::IntLiteral);
  };

  "field assignment"_test = [] {
    auto output = parse_string("fn f(): i32\n    x.y = 42\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& assign = fn.body[0]->as<Assignment>();
    expect(assign.target->kind() == NodeKind::FieldExpr);
  };

  "index assignment"_test = [] {
    auto output = parse_string("fn f(): i32\n    a[0] = 42\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& assign = fn.body[0]->as<Assignment>();
    expect(assign.target->kind() == NodeKind::IndexExpr);
  };

  "invalid assignment target"_test = [] {
    auto output = parse_string("fn f(): i32\n    1 = 2\n");
    expect(!output.parse_result.diagnostics.empty()) << "should reject invalid LHS";
  };
};

suite<"return_tests"> return_tests = [] {
  "return statement"_test = [] {
    auto output = parse_string("fn f(): i32\n    return 42\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& ret = fn.body[0]->as<ReturnStatement>();
    expect(fn.body[0]->kind() == NodeKind::ReturnStatement);
    expect(ret.value->kind() == NodeKind::IntLiteral);
  };
};

suite<"class_tests"> class_tests = [] {
  "class with fields"_test = [] {
    auto output = parse_string("class Point:\n    x: i32\n    y: i32\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    auto* file = output.parse_result.file;
    expect(file->declarations.size() == 1_u);

    auto* decl0 = file->declarations[0];
    const auto& cls = decl0->as<ClassDecl>();
    expect(decl0->kind() == NodeKind::ClassDecl);
    expect(cls.name == "Point");
    expect(cls.fields.size() == 2_u);
    expect(cls.fields[0]->name == "x");
    expect(cls.fields[1]->name == "y");
    expect(cls.fields[0]->type != nullptr);
    expect(cls.fields[1]->type != nullptr);
  };

  "class with single field"_test = [] {
    auto output = parse_string("class Wrapper:\n    value: string\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& cls = output.parse_result.file->declarations[0]->as<ClassDecl>();
    expect(cls.fields.size() == 1_u);
    expect(cls.fields[0]->name == "value");
  };

  "class followed by function"_test = [] {
    auto output = parse_string("class Point:\n    x: i32\n    y: i32\nfn f(): i32 -> 0\n");
    expect(output.parse_result.diagnostics.empty());
    expect(output.parse_result.file->declarations.size() == 2_u);
    expect(output.parse_result.file->declarations[0]->kind() == NodeKind::ClassDecl);
    expect(output.parse_result.file->declarations[1]->kind() == NodeKind::FunctionDecl);
  };

  "let inside class body is rejected"_test = [] {
    auto output = parse_string("class Point:\n    let x: i32\n");
    expect(!output.parse_result.diagnostics.empty()) << "let in class body should fail";
  };

  "struct keyword produces migration diagnostic"_test = [] {
    auto output = parse_string("struct Point:\n    x: i32\n");
    expect(!output.parse_result.diagnostics.empty());
    bool found = false;
    for (const auto& diag : output.parse_result.diagnostics) {
      if (diag.message.find("renamed") != std::string::npos) {
        found = true;
      }
    }
    expect(found) << "should mention 'renamed'";
  };
};

suite<"file_tests"> file_tests = [] {
  "examples parse without error"_test = [] {
    std::filesystem::path root(DAO_SOURCE_DIR);
    for (const auto& entry : std::filesystem::directory_iterator(root / "examples")) {
      if (entry.path().extension() == ".dao") {
        auto output = parse_file(entry.path());
        expect(output.parse_result.diagnostics.empty())
            << "parse errors in " << entry.path().filename().string();
        expect(output.parse_result.file != nullptr)
            << "null file for " << entry.path().filename().string();
      }
    }
  };

  "syntax probes parse without error"_test = [] {
    std::filesystem::path root(DAO_SOURCE_DIR);
    for (const auto& entry : std::filesystem::directory_iterator(root / "spec" / "syntax_probes")) {
      if (entry.path().extension() == ".dao") {
        auto output = parse_file(entry.path());
        expect(output.parse_result.diagnostics.empty())
            << "parse errors in " << entry.path().filename().string();
        expect(output.parse_result.file != nullptr)
            << "null file for " << entry.path().filename().string();
      }
    }
  };
};

suite<"qualified_name_tests"> qualified_name_tests = [] {
  "qualified name in expression"_test = [] {
    auto output = parse_string("fn f(): i32\n    Foo::bar()\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& expr_stmt = fn.body[0]->as<ExpressionStatement>();
    const auto& call = expr_stmt.expr->as<CallExpr>();
    expect(call.callee->kind() == NodeKind::QualifiedName);
    const auto& qn = call.callee->as<QualifiedName>();
    expect(qn.segments.size() == 2_u);
    expect(qn.segments[0] == "Foo");
    expect(qn.segments[1] == "bar");
  };
};

// ---------------------------------------------------------------------------
// Concept declarations
// ---------------------------------------------------------------------------

suite<"concept_tests"> concept_tests = [] {
  "simple concept with bare method"_test = [] {
    auto output = parse_string("concept Printable:\n"
                               "    fn to_string(self): string\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    auto* file = output.parse_result.file;
    expect(file->declarations.size() == 1_u);

    auto* decl = file->declarations[0];
    expect(decl->kind() == NodeKind::ConceptDecl);
    const auto& concept_ = decl->as<ConceptDecl>();
    expect(concept_.name == "Printable");
    expect(!concept_.is_derived);
    expect(concept_.methods.size() == 1_u);

    const auto& method = concept_.methods[0]->as<FunctionDecl>();
    expect(method.name == "to_string");
    expect(method.params.size() == 1_u);
    expect(method.params[0].name == "self");
    expect(method.params[0].type == nullptr) << "bare self has no explicit type";
    expect(!method.is_expr_bodied());
    expect(method.body.empty()) << "bare signature has no body";
  };

  "concept with default method"_test = [] {
    auto output = parse_string("concept Equatable:\n"
                               "    fn eq(self, other: Equatable): bool\n"
                               "    fn ne(self, other: Equatable): bool -> !self.eq(other)\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& concept_ = output.parse_result.file->declarations[0]->as<ConceptDecl>();
    expect(concept_.name == "Equatable");
    expect(concept_.methods.size() == 2_u);

    const auto& eq_method = concept_.methods[0]->as<FunctionDecl>();
    expect(eq_method.name == "eq");
    expect(!eq_method.is_expr_bodied());

    const auto& ne_method = concept_.methods[1]->as<FunctionDecl>();
    expect(ne_method.name == "ne");
    expect(ne_method.is_expr_bodied());
  };

  "derived concept"_test = [] {
    auto output = parse_string("derived concept Copyable:\n"
                               "    fn copy(self): Copyable\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& concept_ = output.parse_result.file->declarations[0]->as<ConceptDecl>();
    expect(concept_.name == "Copyable");
    expect(concept_.is_derived);
  };

  "generic concept"_test = [] {
    auto output = parse_string("concept Iterator<T>:\n"
                               "    fn has_next(self): bool\n"
                               "    fn next(self): T\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& concept_ = output.parse_result.file->declarations[0]->as<ConceptDecl>();
    expect(concept_.name == "Iterator");
    expect(concept_.type_params.size() == 1_u);
    expect(concept_.type_params[0].name == "T");
    expect(concept_.methods.size() == 2_u);
  };
};

// ---------------------------------------------------------------------------
// Conformance blocks
// ---------------------------------------------------------------------------

suite<"conformance_tests"> conformance_tests = [] {
  "class with inline conformance"_test = [] {
    auto output = parse_string("class Point:\n"
                               "    x: f64\n"
                               "    y: f64\n"
                               "    as Printable:\n"
                               "        fn to_string(self): string -> \"point\"\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& cls = output.parse_result.file->declarations[0]->as<ClassDecl>();
    expect(cls.name == "Point");
    expect(cls.fields.size() == 2_u);
    expect(cls.conformances.size() == 1_u);
    expect(cls.conformances[0].concept_name == "Printable");
    expect(cls.conformances[0].methods.size() == 1_u);
  };

  "class with deny"_test = [] {
    auto output = parse_string("class SecretKey:\n"
                               "    data: i32\n"
                               "    deny Printable\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& cls = output.parse_result.file->declarations[0]->as<ClassDecl>();
    expect(cls.denials.size() == 1_u);
    expect(cls.denials[0].concept_name == "Printable");
  };

  "class with multiple conformances"_test = [] {
    auto output = parse_string("class Point:\n"
                               "    x: f64\n"
                               "    as Printable:\n"
                               "        fn to_string(self): string -> \"p\"\n"
                               "    as Equatable:\n"
                               "        fn eq(self, other: Point): bool -> true\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    const auto& cls = output.parse_result.file->declarations[0]->as<ClassDecl>();
    expect(cls.conformances.size() == 2_u);
    expect(cls.conformances[0].concept_name == "Printable");
    expect(cls.conformances[1].concept_name == "Equatable");
  };
};

// ---------------------------------------------------------------------------
// Extend declarations
// ---------------------------------------------------------------------------

suite<"extend_tests"> extend_tests = [] {
  "extend builtin with concept"_test = [] {
    auto output = parse_string("extend i32 as Printable:\n"
                               "    fn to_string(self): string -> \"num\"\n");
    expect(output.parse_result.diagnostics.empty()) << "no parse errors";
    auto* file = output.parse_result.file;
    expect(file->declarations.size() == 1_u);

    auto* decl = file->declarations[0];
    expect(decl->kind() == NodeKind::ExtendDecl);
    const auto& ext = decl->as<ExtendDecl>();
    expect(ext.concept_name == "Printable");
    expect(ext.module_binding.empty()) << "an unqualified conformance names no module";
    expect(ext.target_type != nullptr);
    expect(ext.methods.size() == 1_u);
  };
};

// ---------------------------------------------------------------------------
// self is contextual — valid as identifier outside method context
// ---------------------------------------------------------------------------

suite<"self_keyword_tests"> self_keyword_tests = [] {
  "self is reserved in let binding"_test = [] {
    auto output = parse_string("fn foo(): i32\n"
                               "    let self: i32 = 1\n"
                               "    return 0\n");
    expect(!output.parse_result.diagnostics.empty())
        << "self should be rejected as a let binding name";
  };

  "self is reserved as class field name"_test = [] {
    auto output = parse_string("class Wrapper:\n"
                               "    self: i32\n");
    expect(!output.parse_result.diagnostics.empty()) << "self should be rejected as a field name";
  };

  "self is valid as parameter name"_test = [] {
    auto output = parse_string("concept Show:\n"
                               "    fn show(self): string\n");
    expect(output.parse_result.diagnostics.empty())
        << "self should be accepted as a parameter name";
  };
};

// ---------------------------------------------------------------------------
// Error recovery
// ---------------------------------------------------------------------------

suite<"error_recovery"> error_recovery = [] {
  "broken declaration does not prevent parsing subsequent ones"_test = [] {
    SourceBuffer src("test.dao",
                     "fn good1(): i32 -> 1\n"
                     "\n"
                     "this is garbage\n"
                     "\n"
                     "fn good2(): i32 -> 2\n");
    auto lex_result = lex(src);
    auto result = parse(lex_result.tokens);
    expect(result.file != nullptr) << "file should be produced";
    // good1, ErrorDecl, and good2 should all be present.
    expect(result.file->declarations.size() == 3_ul) << "should have good1 + error + good2";
    if (result.file->declarations.size() == 3) {
      expect(result.file->declarations[0]->kind() == NodeKind::FunctionDecl);
      expect(result.file->declarations[1]->kind() == NodeKind::ErrorDecl)
          << "error placeholder should be inserted for garbage";
      expect(result.file->declarations[2]->kind() == NodeKind::FunctionDecl);
    }
  };

  "broken statement does not prevent parsing subsequent ones"_test = [] {
    SourceBuffer src("test.dao",
                     "fn main(): i32\n"
                     "  let x: i32 = 1\n"
                     "  @@@ broken\n"
                     "  return x\n");
    auto lex_result = lex(src);
    auto result = parse(lex_result.tokens);
    expect(result.file != nullptr) << "file should be produced";
    expect(!result.file->declarations.empty()) << "function should be parsed";
    if (!result.file->declarations.empty()) {
      const auto& fn = result.file->declarations[0]->as<FunctionDecl>();
      // Should have let, ErrorStmt, and return.
      expect(fn.body.size() >= 3_ul) << "should have let + error + return";
      if (fn.body.size() >= 3) {
        expect(fn.body[0]->kind() == NodeKind::LetStatement);
        expect(fn.body[1]->kind() == NodeKind::ErrorStmt)
            << "error placeholder should be inserted for broken statement";
        expect(fn.body[2]->kind() == NodeKind::ReturnStatement);
      }
    }
  };

  "incomplete source still produces partial AST"_test = [] {
    SourceBuffer src("test.dao",
                     "fn add(a: i32, b: i32): i32 -> a + b\n"
                     "\n"
                     "fn incomplete(\n");
    auto lex_result = lex(src);
    auto result = parse(lex_result.tokens);
    expect(result.file != nullptr) << "file should be produced";
    // At least the first valid function should be present.
    expect(!result.file->declarations.empty()) << "valid declaration should survive incomplete one";
  };
};

// ---------------------------------------------------------------------------
// Call-site explicit type arguments
// ---------------------------------------------------------------------------

suite<"call_type_arg_tests"> call_type_arg_tests = [] {
  "explicit type arg parsed"_test = [] {
    auto output = parse_string("fn f(): i32\n    size_of<i32>()\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& call = fn.body[0]->as<ExpressionStatement>().expr->as<CallExpr>();
    expect(call.type_args.size() == 1_u);
    expect(call.args.empty());
  };

  "explicit type arg does not break comparison"_test = [] {
    auto output =
        parse_string("fn f(x: i32): i32\n    if x < 3:\n        return 1\n    return 0\n");
    expect(output.parse_result.diagnostics.empty());
  };

  "multiple type args parsed"_test = [] {
    auto output = parse_string("fn f(): i32\n    g<i32, f64>(1)\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& call = fn.body[0]->as<ExpressionStatement>().expr->as<CallExpr>();
    expect(call.type_args.size() == 2_u);
  };
};

// ---------------------------------------------------------------------------
// Class body methods
// ---------------------------------------------------------------------------

suite<"class_method_tests"> class_method_tests = [] {
  "class with method parses"_test = [] {
    auto output = parse_string("class Foo:\n"
                               "    x: i32\n"
                               "\n"
                               "    fn get(self): i32 -> self.x\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& cls = output.parse_result.file->declarations[0]->as<ClassDecl>();
    expect(cls.fields.size() == 1_u);
    expect(cls.methods.size() == 1_u);
    expect(cls.methods[0]->as<FunctionDecl>().name == "get");
  };

  "class with static method parses"_test = [] {
    auto output = parse_string("class Foo:\n"
                               "    x: i32\n"
                               "\n"
                               "    fn zero(): Foo\n"
                               "        let z: i32 = 0\n"
                               "        return Foo(z)\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& cls = output.parse_result.file->declarations[0]->as<ClassDecl>();
    expect(cls.methods.size() == 1_u);
    expect(cls.methods[0]->as<FunctionDecl>().name == "zero");
    expect(cls.methods[0]->as<FunctionDecl>().params.empty());
  };

  "store through pointer parses"_test = [] {
    auto output = parse_string("fn f(p: *i32): void\n"
                               "    mode unsafe =>\n"
                               "        *p = 42\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& mode = fn.body[0]->as<ModeBlock>();
    const auto& assign = mode.body[0]->as<Assignment>();
    expect(assign.target->kind() == NodeKind::UnaryExpr);
    expect(assign.target->as<UnaryExpr>().op == UnaryOp::Deref);
  };
};

// ---------------------------------------------------------------------------
// Static method call syntax
// ---------------------------------------------------------------------------

suite<"static_method_tests"> static_method_tests = [] {
  "nongeneric static call parses as mangled identifier"_test = [] {
    auto output = parse_string("fn f(): i32\n    Foo::bar()\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& call = fn.body[0]->as<ExpressionStatement>().expr->as<CallExpr>();
    // When followed by (, 2-segment QualifiedName stays as QualifiedName
    // but the resolver mangles it. Parser preserves the QualifiedName.
    expect(call.callee->kind() == NodeKind::QualifiedName);
  };

  "generic static call parses with type args"_test = [] {
    auto output = parse_string("fn f(): i32\n    Foo<i32>::bar()\n");
    expect(output.parse_result.diagnostics.empty());
    const auto& fn = output.parse_result.file->declarations[0]->as<FunctionDecl>();
    const auto& call = fn.body[0]->as<ExpressionStatement>().expr->as<CallExpr>();
    expect(call.type_args.size() == 1_u);
    expect(call.callee->kind() == NodeKind::Identifier);
    expect(call.callee->as<IdentifierExpr>().name == "Foo.bar");
  };
};

suite<"diagnostic_wording_tests"> diagnostic_wording_tests = [] {
  "an expected-token error names the token it found"_test = [] {
    const auto& diags =
        parse_string("fn f(): i32\n  let x: i32 = )\n  return x\n").parse_result.diagnostics;
    expect(!diags.empty() && diags.front().message == "expected expression, got ')'")
        << (diags.empty() ? "no diagnostic" : diags.front().message);
  };

  "layout tokens are named in words"_test = [] {
    const auto& diags = parse_string("fn f(): i32 ->\n").parse_result.diagnostics;
    expect(!diags.empty() && diags.front().message == "expected expression, got end of line")
        << (diags.empty() ? "no diagnostic" : diags.front().message);
  };
};

// NOLINTEND(readability-function-cognitive-complexity,readability-magic-numbers,modernize-use-trailing-return-type)

} // namespace

// ---------------------------------------------------------------------------
// Conformance positions may name the concept through an import binding
// (CONTRACT_MODULE_SYSTEM.md §6): `as b::C:`, `deny b::C`, `extend T as b::C:`.
// ---------------------------------------------------------------------------

suite<"qualified_conformance"> qualified_conformance = [] {
  "as names a concept through a binding"_test = [] {
    auto output =
        parse_string("class P:\n  x: i32\n  as fmt::Printable:\n    fn show(self): i32 -> 1\n");
    expect(output.parse_result.diagnostics.empty()) << "parses";
    const auto& cls = output.parse_result.file->declarations[0]->as<ClassDecl>();
    expect(cls.conformances[0].module_binding == "fmt");
    expect(cls.conformances[0].concept_name == "Printable");
  };

  "deny names a concept through a binding"_test = [] {
    auto output = parse_string("class P:\n  x: i32\n  deny fmt::Printable\n");
    expect(output.parse_result.diagnostics.empty()) << "parses";
    const auto& cls = output.parse_result.file->declarations[0]->as<ClassDecl>();
    expect(cls.denials[0].module_binding == "fmt");
    expect(cls.denials[0].concept_name == "Printable");
  };

  "extend names a concept through a binding"_test = [] {
    auto output = parse_string("extend i32 as fmt::Printable:\n  fn show(self): i32 -> 1\n");
    expect(output.parse_result.diagnostics.empty()) << "parses";
    const auto& ext = output.parse_result.file->declarations[0]->as<ExtendDecl>();
    expect(ext.module_binding == "fmt");
    expect(ext.concept_name == "Printable");
  };
};

auto main() -> int {
} // NOLINT(readability-named-parameter)
