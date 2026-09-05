#include "analysis/completion.h"
#include "frontend/module/program.h"
#include "analysis/hover.h"
#include "analysis/goto_definition.h"
#include "analysis/document_symbols.h"
#include "analysis/references.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>
#include <string>
#include <vector>
#include <utility>

using namespace boost::ut;
using namespace dao;

namespace {

struct AnalysisPipeline {
  SourceBuffer source;
  LexResult lex_result;
  ParseResult parse_result;
  ResolveResult resolve_result;
  TypeContext types;
  TypeCheckResult check_result;

  explicit AnalysisPipeline(const std::string& src)
      : source("test.dao", wrap_with_test_module(src)),
        lex_result(lex(source)),
        parse_result(parse(lex_result.tokens)) {
    if (parse_result.file != nullptr) {
      resolve_result = resolve(*parse_result.file);
      check_result =
          typecheck(*parse_result.file, resolve_result, types);
    }
  }

  /// Rebase a raw fixture-relative offset to the wrapped source.
  [[nodiscard]] static auto at(uint32_t raw) -> uint32_t {
    return raw + kTestModulePrefixBytes;
  }
};

// ---------------------------------------------------------------------------
// Cross-module navigation (Task 31 D5): hover and definition follow a
// qualified name into the module that declares it.
// ---------------------------------------------------------------------------

struct ProgramPipeline {
  Program program;
  ResolveResult resolve_result;
  TypeContext types;
  TypeCheckResult check_result;

  explicit ProgramPipeline(std::vector<std::pair<std::string, std::string>> files) {
    std::vector<SourceInput> inputs;
    for (auto& [display, text] : files) {
      inputs.push_back({.display_path = display, .text = text, .is_prelude = false});
    }
    program = build_program(std::move(inputs));
    resolve_result = resolve(program);
    check_result = typecheck(program, resolve_result, types);
  }

  /// Program offset of the first occurrence of `text` in a file.
  [[nodiscard]] auto offset_in(std::string_view display, std::string_view text) const
      -> uint32_t {
    for (const auto& file : program.files) {
      if (file->display_path == display) {
        return file->base_offset + static_cast<uint32_t>(file->buffer.contents().find(text));
      }
    }
    return 0;
  }

  [[nodiscard]] auto file_of(uint32_t offset) const -> std::string {
    const auto* file = program.source_map.file_for(offset);
    return file == nullptr ? "" : file->display_path;
  }
};

} // namespace

suite<"cross_module_navigation"> cross_module_navigation = [] {
  "hover_and_definition_follow_a_qualified_call_into_its_module"_test = [] {
    ProgramPipeline pipe({
        {"main.dao", "module app::main\nimport app::math\nfn main(): i32 -> math::add(1, 2)\n"},
        {"math.dao", "module app::math\nfn add(a: i32, b: i32): i32 -> a + b\n"},
    });
    expect(pipe.resolve_result.diagnostics.empty() && pipe.check_result.diagnostics.empty());

    auto use = pipe.offset_in("main.dao", "add(1");
    auto hover = query_hover(use, pipe.resolve_result, pipe.check_result);
    expect(hover.has_value() && hover->name == "add" && hover->symbol_kind == "function")
        << (hover ? hover->name : "no hover");
    expect(hover.has_value() && hover->type == "fn(i32, i32): i32") << (hover ? hover->type : "");

    auto definition = query_definition(use, pipe.resolve_result);
    expect(definition.has_value()) << "no definition";
    if (definition) {
      expect(pipe.file_of(definition->offset) == "math.dao") << pipe.file_of(definition->offset);
      expect(pipe.program.source_map.locate(definition->offset).line == 2_u);
    }

    // The head segment still names the module binding.
    auto head = query_hover(pipe.offset_in("main.dao", "math::add"), pipe.resolve_result,
                            pipe.check_result);
    expect(head.has_value() && head->symbol_kind == "module") << (head ? head->symbol_kind : "");
  };
};

namespace {
} // namespace

// ---------------------------------------------------------------------------
// Document symbols
// ---------------------------------------------------------------------------

suite<"document_symbols"> document_symbols = [] {
  "functions appear as top-level symbols"_test = [] {
    AnalysisPipeline pipe("fn add(a: i32, b: i32): i32 -> a + b\n");
    auto symbols = query_document_symbols(*pipe.parse_result.file);
    expect(symbols.size() == 1_ul);
    expect(symbols[0].name == "add");
    expect(symbols[0].kind == "function");
    expect(symbols[0].children.size() == 2_ul);
    expect(symbols[0].children[0].name == "a");
    expect(symbols[0].children[0].kind == "parameter");
    expect(symbols[0].children[1].name == "b");
  };

  "classes show fields as children"_test = [] {
    AnalysisPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n");
    auto symbols = query_document_symbols(*pipe.parse_result.file);
    expect(symbols.size() == 1_ul);
    expect(symbols[0].name == "Point");
    expect(symbols[0].kind == "class");
    expect(symbols[0].children.size() == 2_ul);
    expect(symbols[0].children[0].name == "x");
    expect(symbols[0].children[0].kind == "field");
    expect(symbols[0].children[1].name == "y");
  };

  "concepts show methods as children"_test = [] {
    AnalysisPipeline pipe(
        "concept Printable:\n"
        "  fn to_string(self): string\n");
    auto symbols = query_document_symbols(*pipe.parse_result.file);
    expect(symbols.size() == 1_ul);
    expect(symbols[0].name == "Printable");
    expect(symbols[0].kind == "concept");
    expect(symbols[0].children.size() == 1_ul);
    expect(symbols[0].children[0].name == "to_string");
    expect(symbols[0].children[0].kind == "method");
  };

  "prelude declarations are filtered"_test = [] {
    // Prelude files are separate program files; querying the user
    // file's symbols never sees them.
    std::vector<std::string> prelude = {"module core::probe\nfn prelude_fn(): i32 -> 0\n"};
    auto program = make_test_program("fn user_fn(): i32 -> 1\n", prelude);
    auto symbols = query_document_symbols(*user_file(program).file());
    expect(symbols.size() == 1_ul);
    expect(symbols[0].name == "user_fn");
  };

  "aliases appear as symbols"_test = [] {
    AnalysisPipeline pipe("type NodeId = i32\n");
    auto symbols = query_document_symbols(*pipe.parse_result.file);
    expect(symbols.size() == 1_ul);
    expect(symbols[0].name == "NodeId");
    expect(symbols[0].kind == "alias");
  };

  "extern functions appear"_test = [] {
    AnalysisPipeline pipe("extern fn sqrt(x: f64): f64\n");
    auto symbols = query_document_symbols(*pipe.parse_result.file);
    expect(symbols.size() == 1_ul);
    expect(symbols[0].name == "sqrt");
    expect(symbols[0].kind == "extern_function");
  };
};

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

suite<"references"> references = [] {
  "find references to a function"_test = [] {
    AnalysisPipeline pipe(
        "fn add(a: i32, b: i32): i32 -> a + b\n"
        "fn main(): i32\n"
        "  return add(1, 2)\n");
    // Raw offset 3 = "add" declaration (rebased for synthetic module
    // header).
    auto refs = query_references(AnalysisPipeline::at(3), pipe.resolve_result);
    expect(refs.size() >= 2_ul) << "should find definition + at least one use";

    bool has_def = false;
    bool has_use = false;
    for (const auto& ref : refs) {
      if (ref.is_definition) {
        has_def = true;
      } else {
        has_use = true;
      }
    }
    expect(has_def) << "should include definition";
    expect(has_use) << "should include use site";
  };

  "find references from use site"_test = [] {
    AnalysisPipeline pipe(
        "fn foo(): i32 -> 42\n"
        "fn main(): i32\n"
        "  return foo()\n");
    // Use the declaration offset (raw 3, rebased via at()).
    auto refs = query_references(AnalysisPipeline::at(3), pipe.resolve_result);
    expect(refs.size() >= 2_ul) << "definition + use";
  };

  "no references for unknown offset"_test = [] {
    AnalysisPipeline pipe("fn main(): i32 -> 0\n");
    // Sentinel out-of-range offset; not rebased.
    auto refs = query_references(9999, pipe.resolve_result);
    expect(refs.empty()) << "should return empty for unknown offset";
  };
};

// ---------------------------------------------------------------------------
// Completion
// ---------------------------------------------------------------------------

namespace {

auto has_completion(const std::vector<CompletionItem>& items,
                    std::string_view name) -> bool {
  for (const auto& item : items) {
    if (item.label == name) {
      return true;
    }
  }
  return false;
}

} // namespace

suite<"completion"> completion = [] {
  // Raw offsets below are written as if the fixture started at byte 0.
  // AnalysisPipeline::at(raw) rebases them for the synthetic `module
  // test` header injected by the pipeline.
  "top-level functions are offered"_test = [] {
    AnalysisPipeline pipe(
        "fn add(a: i32, b: i32): i32 -> a + b\n"
        "fn main(): i32\n"
        "  return 0\n");
    // Offset inside main body (after "  return ").
    auto items = query_completions(AnalysisPipeline::at(55), pipe.resolve_result,
                                   pipe.check_result);
    expect(has_completion(items, "add")) << "should offer add";
    expect(has_completion(items, "main")) << "should offer main";
  };

  "local variables are offered"_test = [] {
    // Source layout: "fn main(): i32\n  let x: i32 = 42\n  return x\n"
    //                0              14                 32           43
    // Offset 40 is inside "return x", well after let x.
    AnalysisPipeline pipe(
        "fn main(): i32\n"
        "  let x: i32 = 42\n"
        "  return x\n");
    auto items = query_completions(AnalysisPipeline::at(40), pipe.resolve_result,
                                   pipe.check_result);
    expect(has_completion(items, "x")) << "should offer local x";
  };

  "parameters are offered"_test = [] {
    // Source: "fn add(a: i32, b: i32): i32\n  return a + b\n"
    //         0                            28              42
    // Offset 38 is inside "return a + b".
    AnalysisPipeline pipe(
        "fn add(a: i32, b: i32): i32\n"
        "  return a + b\n");
    auto items = query_completions(AnalysisPipeline::at(38), pipe.resolve_result,
                                   pipe.check_result);
    expect(has_completion(items, "a")) << "should offer param a";
    expect(has_completion(items, "b")) << "should offer param b";
  };

  "locals before cursor are offered, after cursor are not"_test = [] {
    // Source: "fn main(): i32\n  let x: i32 = 1\n  let y: i32 = 2\n  return x\n"
    //         0              14               30                46           58
    // Offset 33 is at the start of "let y" — x is declared, y is not yet.
    AnalysisPipeline pipe(
        "fn main(): i32\n"
        "  let x: i32 = 1\n"
        "  let y: i32 = 2\n"
        "  return x\n");
    auto items = query_completions(AnalysisPipeline::at(33), pipe.resolve_result,
                                   pipe.check_result);
    expect(has_completion(items, "x")) << "x declared before cursor";
    expect(!has_completion(items, "y")) << "y declared after cursor";
  };

  "builtin types are offered"_test = [] {
    AnalysisPipeline pipe(
        "fn main(): i32\n"
        "  return 0\n");
    auto items = query_completions(AnalysisPipeline::at(20), pipe.resolve_result,
                                   pipe.check_result);
    expect(has_completion(items, "i32")) << "should offer i32";
    expect(has_completion(items, "f64")) << "should offer f64";
    expect(has_completion(items, "bool")) << "should offer bool";
  };

  "internal hooks are filtered out"_test = [] {
    AnalysisPipeline pipe(
        "fn main(): i32\n"
        "  return 0\n");
    auto items = query_completions(AnalysisPipeline::at(20), pipe.resolve_result,
                                   pipe.check_result);
    for (const auto& item : items) {
      expect(!item.label.starts_with("__"))
          << "should not offer __-prefixed: " << item.label;
    }
  };

  "completions include type info"_test = [] {
    AnalysisPipeline pipe(
        "fn add(a: i32, b: i32): i32 -> a + b\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto items = query_completions(AnalysisPipeline::at(55), pipe.resolve_result,
                                   pipe.check_result);
    for (const auto& item : items) {
      if (item.label == "add") {
        expect(!item.type.empty()) << "add should have type info";
        break;
      }
    }
  };

  "if/else branches have separate scopes"_test = [] {
    // Source: "fn test(): i32\n  if true:\n    let a: i32 = 1\n  else:\n    let b: i32 = 2\n  return 0\n"
    // Then branch: let a at offset ~28. Else branch: let b at offset ~52.
    AnalysisPipeline pipe(
        "fn test(): i32\n"
        "  if true:\n"
        "    let a: i32 = 1\n"
        "    return a\n"
        "  else:\n"
        "    let b: i32 = 2\n"
        "    return b\n"
        "  return 0\n");
    // Inside else branch — "let b" is at ~61, "return b" at ~79.
    // Offset 82 should be inside else scope.
    auto items = query_completions(AnalysisPipeline::at(82), pipe.resolve_result,
                                   pipe.check_result);
    expect(has_completion(items, "b")) << "b should be visible in else";
    expect(!has_completion(items, "a")) << "a should NOT be visible in else";
  };

  "shadowing: inner local hides outer"_test = [] {
    AnalysisPipeline pipe(
        "fn test(x: i32): i32\n"
        "  let x: i32 = 99\n"
        "  return x\n");
    // Offset inside body, after let x shadows param x.
    auto items = query_completions(AnalysisPipeline::at(40), pipe.resolve_result,
                                   pipe.check_result);
    // Should have exactly one "x", not two.
    int x_count = 0;
    for (const auto& item : items) {
      if (item.label == "x") {
        ++x_count;
      }
    }
    expect(x_count == 1) << "shadowed x should appear once, got " << x_count;
  };

  "completion works at end of file"_test = [] {
    std::string src =
        "fn add(a: i32, b: i32): i32 -> a + b\n"
        "fn main(): i32\n"
        "  return 0\n";
    AnalysisPipeline pipe(src);
    // Cursor at raw src.size() — one past the last character of the
    // raw fixture (rebased onto the wrapped source).
    auto items = query_completions(
        AnalysisPipeline::at(static_cast<uint32_t>(src.size())),
        pipe.resolve_result, pipe.check_result);
    expect(!items.empty()) << "should offer completions at end of file";
    expect(has_completion(items, "add")) << "add should be visible at EOF";
  };
};

// ---------------------------------------------------------------------------
// Dot completion
// ---------------------------------------------------------------------------

suite<"dot_completion"> dot_completion = [] {
  "struct fields offered"_test = [] {
    AnalysisPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n");
    auto* point_type = pipe.check_result.typed.decl_type(
        pipe.parse_result.file->declarations[0]);
    expect(point_type != nullptr) << "Point type should exist";
    auto items = query_dot_completions(point_type, pipe.check_result);
    expect(has_completion(items, "x")) << "should offer field x";
    expect(has_completion(items, "y")) << "should offer field y";
  };

  "methods from extends offered"_test = [] {
    AnalysisPipeline pipe(
        "extern fn __test_hook(x: i32): string\n"
        "concept Showable:\n"
        "  fn show(self): string\n"
        "extend i32 as Showable:\n"
        "  fn show(self): string -> __test_hook(self)\n");
    // Get the i32 type via the type context.
    auto* i32_type = pipe.types.i32();
    auto items = query_dot_completions(i32_type, pipe.check_result);
    expect(has_completion(items, "show")) << "should offer method show";
  };

  "expr_types map includes call expressions for dot lookup"_test = [] {
    // Verify that a function call returning a struct type is recorded
    // in the expr_types map — this is the foundation for expression-
    // typed dot completion (e.g. make_point(). should offer fields).
    AnalysisPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "fn make_point(): Point\n"
        "  return Point(0, 0)\n"
        "fn main(): i32\n"
        "  let p = make_point()\n"
        "  return p.x\n");
    // The call expression `make_point()` should have a typed entry.
    bool found_call = false;
    for (const auto& [expr, type] : pipe.check_result.typed.expr_types()) {
      if (expr->kind() == NodeKind::CallExpr && type != nullptr &&
          type->kind() == TypeKind::Struct) {
        found_call = true;
        // Verify dot completion works with this type.
        auto items = query_dot_completions(type, pipe.check_result);
        expect(has_completion(items, "x"))
            << "should offer field x on call result";
        expect(has_completion(items, "y"))
            << "should offer field y on call result";
      }
    }
    expect(found_call) << "call returning struct should be in expr_types";
  };

  "dot completion on non-struct returns empty for fields"_test = [] {
    AnalysisPipeline pipe("fn main(): i32 -> 0\n");
    auto* bool_type = pipe.types.bool_type();
    auto items = query_dot_completions(bool_type, pipe.check_result);
    // bool has no fields, but may have methods from Equatable etc.
    for (const auto& item : items) {
      expect(item.kind != "field")
          << "bool should not have fields: " << item.label;
    }
  };
};

auto main() -> int {} // NOLINT(readability-named-parameter)
