#include "frontend/diagnostics/source.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"
#include "ir/hir/hir.h"
#include "ir/hir/hir_builder.h"
#include "ir/hir/hir_context.h"
#include "ir/hir/hir_printer.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>
#include <sstream>
#include <string>

using namespace boost::ut;
using namespace dao;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

/// Owns all pipeline state so HIR nodes remain valid.
struct HirTestPipeline {
  SourceBuffer source;
  LexResult lex_result;
  ParseResult parse_result;
  ResolveResult resolve_result;
  TypeContext types;
  TypeCheckResult check_result;
  HirContext hir_ctx;
  HirBuildResult hir_result;

  explicit HirTestPipeline(const std::string& src)
      : source("test.dao", wrap_with_test_module(src)),
        lex_result(lex(source)),
        parse_result(parse(lex_result.tokens)) {
    if (parse_result.file != nullptr) {
      resolve_result = resolve(*parse_result.file);
      check_result =
          typecheck(*parse_result.file, resolve_result, types);
      hir_result =
          build_hir(*parse_result.file, resolve_result, check_result, hir_ctx);
    }
  }

  [[nodiscard]] auto module() const -> HirModule* {
    return hir_result.program == nullptr ? nullptr : hir_result.program->modules.front();
  }

  [[nodiscard]] auto dump() const -> std::string {
    std::ostringstream out;
    if (hir_result.program != nullptr) {
      print_hir(out, *hir_result.program);
    }
    return out.str();
  }
};

/// Returns true if the string contains the given substring.
auto contains(const std::string& haystack, std::string_view needle) -> bool {
  return haystack.find(needle) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// Enum variant values: a tag for classification enums, a construction for
// payload-bearing ones (whose values are tagged structs)
// ---------------------------------------------------------------------------

suite<"hir_enum_variant_values"> hir_enum_variant_values = [] {
  "classification enum variant is its tag"_test = [] {
    HirTestPipeline p("enum Color:\n"
                      "  Red\n"
                      "  Green\n"
                      "fn pick(): Color\n"
                      "  return Color.Green\n");
    auto dump = p.dump();
    expect(contains(dump, "IntLiteral 1")) << dump;
    expect(!contains(dump, "EnumConstruct")) << dump;
  };

  "match arms on a payload-bearing enum compare tags, not constructions"_test = [] {
    HirTestPipeline p("enum class K:\n"
                      "  A(v: i64)\n"
                      "  B\n"
                      "fn pick(k: K): i64\n"
                      "  match k:\n"
                      "    K.A(v):\n"
                      "      return v\n"
                      "    K.B:\n"
                      "      return 0\n");
    auto dump = p.dump();
    expect(contains(dump, "EnumDiscriminant")) << dump;
    expect(contains(dump, "IntLiteral 1")) << dump;
    expect(!contains(dump, "EnumConstruct")) << dump;
  };

  "fieldless variant of a payload-bearing enum is constructed"_test = [] {
    HirTestPipeline p("enum class K:\n"
                      "  A(v: i64)\n"
                      "  B\n"
                      "fn pick(): K\n"
                      "  return K.B\n"
                      "fn pick_qualified(): K\n"
                      "  return K::B\n");
    auto dump = p.dump();
    expect(!contains(dump, "IntLiteral 1")) << dump;
    expect(contains(dump, "EnumConstruct")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: expression-bodied function normalized to HIR
// ---------------------------------------------------------------------------

suite<"hir_expr_bodied"> hir_expr_bodied = [] {
  "expression-bodied function normalized to return"_test = [] {
    HirTestPipeline p("fn double(x: i32): i32 -> x + x\n");
    auto dump = p.dump();
    expect(contains(dump, "Function double")) << dump;
    expect(contains(dump, "Param x : i32")) << dump;
    expect(contains(dump, "Return")) << dump;
    expect(contains(dump, "Binary +")) << dump;
    expect(contains(dump, "SymbolRef x")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: block-bodied function
// ---------------------------------------------------------------------------

suite<"hir_block_bodied"> hir_block_bodied = [] {
  "block-bodied function"_test = [] {
    HirTestPipeline p("fn add(a: i32, b: i32): i32\n    return a + b\n");
    auto dump = p.dump();
    expect(contains(dump, "Function add : i32")) << dump;
    expect(contains(dump, "Param a : i32")) << dump;
    expect(contains(dump, "Param b : i32")) << dump;
    expect(contains(dump, "Return")) << dump;
    expect(contains(dump, "Binary + : i32")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: let with inferred type
// ---------------------------------------------------------------------------

suite<"hir_let"> hir_let = [] {
  "let with initializer"_test = [] {
    HirTestPipeline p("fn main(): i32\n    let x: i32 = 42\n    return x\n");
    auto dump = p.dump();
    expect(contains(dump, "Let x : i32")) << dump;
    expect(contains(dump, "IntLiteral 42 : i32")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: assignment
// ---------------------------------------------------------------------------

suite<"hir_assignment"> hir_assignment = [] {
  "variable assignment"_test = [] {
    HirTestPipeline p(
        "fn main(): i32\n"
        "    let x: i32 = 0\n"
        "    x = 42\n"
        "    return x\n");
    auto dump = p.dump();
    expect(contains(dump, "Assign")) << dump;
    expect(contains(dump, "Target")) << dump;
    expect(contains(dump, "Value")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: if / while / for
// ---------------------------------------------------------------------------

suite<"hir_control_flow"> hir_control_flow = [] {
  "if statement"_test = [] {
    HirTestPipeline p(
        "fn test(x: i32): i32\n"
        "    if x > 0:\n"
        "        return x\n"
        "    return 0\n");
    auto dump = p.dump();
    expect(contains(dump, "If")) << dump;
    expect(contains(dump, "Condition")) << dump;
    expect(contains(dump, "Then")) << dump;
    expect(contains(dump, "Binary >")) << dump;
  };

  "while statement"_test = [] {
    HirTestPipeline p(
        "fn test(x: i32): i32\n"
        "    let i: i32 = 0\n"
        "    while i < x:\n"
        "        i = i + 1\n"
        "    return i\n");
    auto dump = p.dump();
    expect(contains(dump, "While")) << dump;
    expect(contains(dump, "Condition")) << dump;
    expect(contains(dump, "Binary <")) << dump;
  };

  "for statement"_test = [] {
    HirTestPipeline p(
        "fn gen(n: i32): Generator<i32>\n"
        "    yield n\n"
        "fn test(): i32\n"
        "    for item in gen(10):\n"
        "        let y: i32 = item\n"
        "    return 0\n");
    auto dump = p.dump();
    expect(contains(dump, "For item")) << dump;
    expect(contains(dump, "Iterable")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: return
// ---------------------------------------------------------------------------

suite<"hir_return"> hir_return = [] {
  "return with value"_test = [] {
    HirTestPipeline p("fn main(): i32\n    return 42\n");
    auto dump = p.dump();
    expect(contains(dump, "Return")) << dump;
    expect(contains(dump, "IntLiteral 42 : i32")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: pipe expression
// ---------------------------------------------------------------------------

suite<"hir_pipe"> hir_pipe = [] {
  "pipe preserved as first-class node"_test = [] {
    HirTestPipeline p(
        "fn double(x: i32): i32 -> x + x\n"
        "fn main(): i32\n"
        "    return 5 |> double\n");
    auto dump = p.dump();
    expect(contains(dump, "Pipe")) << dump;
    expect(contains(dump, "IntLiteral 5")) << dump;
    expect(contains(dump, "SymbolRef double")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: lambda expression
// ---------------------------------------------------------------------------

suite<"hir_lambda"> hir_lambda = [] {
  "lambda structural lowering"_test = [] {
    HirTestPipeline p(
        "fn main(): i32\n"
        "    return 5 |> |x| -> x + 1\n");
    auto dump = p.dump();
    expect(contains(dump, "Lambda")) << dump;
    expect(contains(dump, "Body")) << dump;
    expect(contains(dump, "Param x")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: mode block
// ---------------------------------------------------------------------------

suite<"hir_mode"> hir_mode = [] {
  "mode block preserved"_test = [] {
    HirTestPipeline p(
        "fn main(): i32\n"
        "    mode unsafe =>\n"
        "        let x: i32 = 42\n"
        "    return 0\n");
    auto dump = p.dump();
    expect(contains(dump, "Mode unsafe")) << dump;
    expect(contains(dump, "Let x : i32")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: resource block
// ---------------------------------------------------------------------------

suite<"hir_resource"> hir_resource = [] {
  "resource block preserved"_test = [] {
    HirTestPipeline p(
        "fn main(): i32\n"
        "    resource gpu compute =>\n"
        "        let x: i32 = 1\n"
        "    return 0\n");
    auto dump = p.dump();
    expect(contains(dump, "Resource gpu compute")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: function call
// ---------------------------------------------------------------------------

suite<"hir_call"> hir_call = [] {
  "function call"_test = [] {
    HirTestPipeline p(
        "fn add(a: i32, b: i32): i32 -> a + b\n"
        "fn main(): i32\n"
        "    return add(1, 2)\n");
    auto dump = p.dump();
    expect(contains(dump, "Call : i32")) << dump;
    expect(contains(dump, "Callee")) << dump;
    expect(contains(dump, "SymbolRef add")) << dump;
    expect(contains(dump, "Args")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Positive: literals
// ---------------------------------------------------------------------------

suite<"hir_literals"> hir_literals = [] {
  "integer literal"_test = [] {
    HirTestPipeline p("fn main(): i32\n    return 42\n");
    expect(contains(p.dump(), "IntLiteral 42 : i32"));
  };

  "float literal"_test = [] {
    HirTestPipeline p("fn main(): f64\n    return 3.14\n");
    expect(contains(p.dump(), "FloatLiteral 3.14 : f64"));
  };

  "string literal"_test = [] {
    HirTestPipeline p("fn main(): string\n    return \"hello\"\n");
    auto dump = p.dump();
    expect(contains(dump, "StringLiteral")) << dump;
    expect(contains(dump, ": string")) << dump;
  };

  "bool literal"_test = [] {
    HirTestPipeline p("fn main(): bool\n    return true\n");
    expect(contains(p.dump(), "BoolLiteral true : bool"));
  };
};

// ---------------------------------------------------------------------------
// Semantic preservation: typed expressions
// ---------------------------------------------------------------------------

suite<"hir_semantic"> hir_semantic = [] {
  "expressions carry semantic types"_test = [] {
    HirTestPipeline p("fn main(): i32\n    return 1 + 2\n");
    expect(p.module() != nullptr);
    expect(p.hir_result.diagnostics.empty()) << "no HIR diagnostics";

    const auto& decls = p.module()->declarations;
    expect(decls.size() == 1_ul);
    const auto& fn = decls[0]->as<HirFunction>();
    expect(fn.body.size() == 1_ul);
    const auto& ret = fn.body[0]->as<HirReturn>();
    expect(ret.value != nullptr);
    expect(ret.value->type != nullptr) << "return expr has type";
  };

  "symbol references resolved"_test = [] {
    HirTestPipeline p("fn id(x: i32): i32\n    return x\n");
    const auto& decls = p.module()->declarations;
    const auto& fn = decls[0]->as<HirFunction>();
    const auto& ret = fn.body[0]->as<HirReturn>();
    expect(ret.value->kind() == HirKind::SymbolRef);
    const auto& ref = ret.value->as<HirSymbolRef>();
    expect(ref.symbol != nullptr) << "symbol identity resolved";
    expect(ref.symbol->name == "x") << "correct symbol name";
  };
};

// ---------------------------------------------------------------------------
// Struct declaration
// ---------------------------------------------------------------------------

suite<"hir_class"> hir_class = [] {
  "class declaration lowered"_test = [] {
    HirTestPipeline p(
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n");
    expect(contains(p.dump(), "ClassDecl Point"));
  };
};

// ---------------------------------------------------------------------------
// HIR builder diagnostics (no crashes on empty)
// ---------------------------------------------------------------------------

suite<"hir_edge"> hir_edge = [] {
  "empty module"_test = [] {
    HirTestPipeline p("");
    expect(p.module() != nullptr);
    expect(p.module()->declarations.empty());
  };

  "void function"_test = [] {
    HirTestPipeline p("fn noop(): void\n    let x: i32 = 1\n");
    expect(contains(p.dump(), "Function noop"));
  };
};

// ---------------------------------------------------------------------------
// Generator / yield
// ---------------------------------------------------------------------------

suite<"hir_generator"> hir_generator = [] {
  "yield lowers to HirYield"_test = [] {
    HirTestPipeline p(
        "fn range(n: i32): Generator<i32>\n"
        "    let i = 0\n"
        "    while i < n:\n"
        "        yield i\n"
        "        i = i + 1\n");
    auto dump = p.dump();
    expect(contains(dump, "Function range : Generator<i32>")) << dump;
    expect(contains(dump, "Yield")) << dump;
    expect(contains(dump, "SymbolRef i : i32")) << dump;
  };

  "for-in over generator lowers to HirFor"_test = [] {
    HirTestPipeline p(
        "fn range(n: i32): Generator<i32>\n"
        "    yield n\n"
        "fn main(): i32\n"
        "    let total = 0\n"
        "    for x in range(10):\n"
        "        total = total + x\n"
        "    return total\n");
    auto dump = p.dump();
    expect(contains(dump, "For x")) << dump;
    expect(contains(dump, "Iterable")) << dump;
    expect(contains(dump, "Call : Generator<i32>")) << dump;
  };
};

// NOLINTEND(readability-magic-numbers)

auto main() -> int {} // NOLINT(readability-named-parameter)
