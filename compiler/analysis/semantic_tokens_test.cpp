#include "analysis/semantic_tokens.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frontend/resolve/resolve.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace boost::ut;
using namespace dao;

namespace {

struct ClassifiedSource {
  SourceBuffer source;
  LexResult lex_result;
  ParseResult parse_result;
  ResolveResult resolve_result;
  std::vector<SemanticToken> tokens;
};

// Classify without resolver (structural + lexical only).
auto classify_source(const std::string& name, std::string contents) -> ClassifiedSource {
  SourceBuffer source(name, wrap_with_test_module(std::move(contents)));
  auto lex_result = lex(source);
  ParseResult parse_result;
  if (lex_result.diagnostics.empty()) {
    parse_result = parse(lex_result.tokens);
  }
  auto sem_tokens = classify_tokens(lex_result.tokens, parse_result.file);
  return {std::move(source), std::move(lex_result), std::move(parse_result), {}, std::move(sem_tokens)};
}

// Classify with resolver (structural + resolve-driven + lexical).
auto classify_source_resolved(const std::string& name, std::string contents) -> ClassifiedSource {
  SourceBuffer source(name, wrap_with_test_module(std::move(contents)));
  auto lex_result = lex(source);
  ParseResult parse_result;
  ResolveResult resolve_result;
  if (lex_result.diagnostics.empty()) {
    parse_result = parse(lex_result.tokens);
    if (parse_result.file != nullptr) {
      resolve_result = resolve(*parse_result.file);
    }
  }
  // Fixtures must be valid Dao: the parser recovers from errors, so an
  // invalid fixture would still classify and the test would prove nothing.
  expect(lex_result.diagnostics.empty() && parse_result.diagnostics.empty())
      << "fixture does not parse: "
      << (!lex_result.diagnostics.empty() ? lex_result.diagnostics[0].message
          : !parse_result.diagnostics.empty() ? parse_result.diagnostics[0].message : "");
  auto sem_tokens = classify_tokens(lex_result.tokens, parse_result.file, &resolve_result);
  return {std::move(source), std::move(lex_result), std::move(parse_result),
          std::move(resolve_result), std::move(sem_tokens)};
}

auto find_token(const std::vector<SemanticToken>& tokens, std::string_view kind)
    -> const SemanticToken* {
  for (const auto& tok : tokens) {
    if (tok.kind == kind) {
      return &tok;
    }
  }
  return nullptr;
}

auto find_token_at(const ClassifiedSource& result, std::string_view kind, std::string_view text)
    -> const SemanticToken* {
  for (const auto& tok : result.tokens) {
    if (tok.kind == kind && result.source.text(tok.span) == text) {
      return &tok;
    }
  }
  return nullptr;
}

auto count_tokens(const std::vector<SemanticToken>& tokens, std::string_view kind) -> size_t {
  size_t count = 0;
  for (const auto& tok : tokens) {
    if (tok.kind == kind) {
      ++count;
    }
  }
  return count;
}

} // namespace

// NOLINTBEGIN(readability-magic-numbers)

suite<"keyword_classification"> keyword_classification = [] {
  "a conformance target's binding is use.module even unresolved"_test = [] {
    // `as typo::Reveal` names a module binding by position.  Whether or
    // not `typo` resolves, it must not vanish from the token stream --
    // an editor paints the unresolved segment too.
    auto result = classify_source_resolved("main.dao",
                                           "class Point:\n    x: i32\n    as typo::Reveal:\n"
                                           "        fn reveal(self): i32 -> self.x\n");
    expect(find_token_at(result, "use.module", "typo") != nullptr)
        << "the unresolved binding segment was left unclassified";
    expect(find_token_at(result, "use.type", "Reveal") != nullptr);
  };

  "keyword.fn is classified"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    0\n");
    expect(find_token(result.tokens, "keyword.fn") != nullptr);
  };

  "keyword.let is classified"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    let x = 1\n    0\n");
    expect(find_token(result.tokens, "keyword.let") != nullptr);
  };

  "keyword.if is classified"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    if true:\n        0\n    0\n");
    expect(find_token(result.tokens, "keyword.if") != nullptr);
  };

  "keyword.return is classified"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    return 0\n");
    expect(find_token(result.tokens, "keyword.return") != nullptr);
  };

  "keyword.import is classified"_test = [] {
    auto result = classify_source("test.dao", "import foo\nfn main(): i32\n    0\n");
    expect(find_token(result.tokens, "keyword.import") != nullptr);
  };

  "keyword.mode is classified"_test = [] {
    auto result =
        classify_source("test.dao", "fn main(): i32\n    mode unsafe =>\n        0\n    0\n");
    expect(find_token(result.tokens, "keyword.mode") != nullptr);
  };

  "keyword.resource is classified"_test = [] {
    auto result = classify_source(
        "test.dao", "fn main(): i32\n    resource memory Pool =>\n        0\n    0\n");
    expect(find_token(result.tokens, "keyword.resource") != nullptr);
  };

  "keyword.self for reserved self"_test = [] {
    auto result = classify_source(
        "test.dao",
        "concept Show:\n"
        "    fn show(self): string\n");
    expect(find_token_at(result, "keyword.self", "self") != nullptr)
        << "self should classify as keyword.self";
  };
};

suite<"literal_classification"> literal_classification = [] {
  "literal.number for integers"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    42\n");
    expect(find_token(result.tokens, "literal.number") != nullptr);
  };

  "literal.string is classified"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    \"hello\"\n");
    expect(find_token(result.tokens, "literal.string") != nullptr);
  };
};

suite<"operator_classification"> operator_classification = [] {
  "operator.pipe is classified"_test = [] {
    auto result =
        classify_source("test.dao", "fn f(x: i32): i32 -> x\nfn g(): i32 -> 1 |> f\n");
    expect(find_token(result.tokens, "operator.pipe") != nullptr);
  };

  "operator.arrow is classified"_test = [] {
    auto result = classify_source("test.dao", "fn f(): i32 -> 0\n");
    expect(find_token(result.tokens, "operator.arrow") != nullptr);
  };

  "operator.assignment is classified"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    let x = 1\n    0\n");
    expect(find_token(result.tokens, "operator.assignment") != nullptr);
  };
};

suite<"declaration_classification"> declaration_classification = [] {
  "decl.function on function name"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    0\n");
    expect(find_token(result.tokens, "decl.function") != nullptr);
  };

  "decl.type on class name"_test = [] {
    auto result = classify_source("test.dao", "class Point:\n    x: i32\n");
    expect(find_token(result.tokens, "decl.type") != nullptr);
  };

  "decl.field on class member"_test = [] {
    auto result = classify_source("test.dao", "class Point:\n    x: i32\n");
    expect(find_token(result.tokens, "decl.field") != nullptr);
  };
};

suite<"type_classification"> type_classification = [] {
  "type.builtin for i32"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    0\n");
    expect(find_token(result.tokens, "type.builtin") != nullptr);
  };

  "type.nominal for user types"_test = [] {
    auto result = classify_source("test.dao", "fn f(g: Graph): i32\n    0\n");
    expect(find_token(result.tokens, "type.nominal") != nullptr);
  };

  "qualified type classifies final segment as type.nominal"_test = [] {
    auto result = classify_source("test.dao", "fn f(g: net::graph::Graph): i32\n    0\n");
    expect(find_token_at(result, "type.nominal", "Graph") != nullptr)
        << "final segment should be type.nominal";
    expect(find_token_at(result, "use.module", "net") != nullptr)
        << "leading segment should be use.module";
    expect(find_token_at(result, "use.module", "graph") != nullptr)
        << "middle segment should be use.module";
  };
};

suite<"module_classification"> module_classification = [] {
  "use.module on import path segments"_test = [] {
    auto result = classify_source("test.dao", "import net::http\nfn main(): i32\n    0\n");
    expect(find_token_at(result, "use.module", "net") != nullptr)
        << "first import segment should be use.module";
    expect(find_token_at(result, "decl.module", "http") != nullptr)
        << "last import segment should be decl.module";
  };

  "use.module on qualified name expression"_test = [] {
    // Qualified name expression leading segments require the resolver
    // to classify — structural AST classification is not authoritative
    // for expression-position qualified names.
    auto result = classify_source_resolved(
        "test.dao", "import net::http\nfn main(): i32\n    http::get\n    0\n");
    expect(find_token_at(result, "use.module", "http") != nullptr)
        << "leading segment should be use.module";
  };
};

suite<"variable_classification"> variable_classification = [] {
  "param binder is a declaration site, not a use"_test = [] {
    auto result = classify_source("test.dao", "fn f(x: i32): i32\n    0\n");
    expect(find_token(result.tokens, "use.variable.param") == nullptr);
    expect(find_token_at(result, "decl.variable.param", "x") != nullptr);
  };

  "let binder is a declaration site, not a use"_test = [] {
    auto result = classify_source("test.dao", "fn main(): i32\n    let x = 1\n    0\n");
    expect(find_token(result.tokens, "use.variable.local") == nullptr);
    expect(find_token_at(result, "decl.variable.local", "x") != nullptr);
  };

  "use.field on field access"_test = [] {
    auto result = classify_source("test.dao", "fn f(p: Point): i32\n    p.x\n");
    expect(find_token(result.tokens, "use.field") != nullptr);
  };
};

suite<"mode_resource_classification"> mode_resource_classification = [] {
  "mode.unsafe is classified"_test = [] {
    auto result =
        classify_source("test.dao", "fn main(): i32\n    mode unsafe =>\n        0\n    0\n");
    expect(find_token(result.tokens, "mode.unsafe") != nullptr);
  };

  "resource.kind.memory is classified"_test = [] {
    auto result = classify_source(
        "test.dao", "fn main(): i32\n    resource memory Pool =>\n        0\n    0\n");
    expect(find_token(result.tokens, "resource.kind.memory") != nullptr);
  };

  "resource.binding is classified"_test = [] {
    auto result = classify_source(
        "test.dao", "fn main(): i32\n    resource memory Pool =>\n        0\n    0\n");
    expect(find_token(result.tokens, "resource.binding") != nullptr);
  };
};

suite<"lambda_classification"> lambda_classification = [] {
  "lambda.param is classified"_test = [] {
    auto result =
        classify_source("test.dao", "fn f(x: i32): i32 -> x\nfn g(): i32 -> 1 |> |x| -> x\n");
    expect(find_token(result.tokens, "lambda.param") != nullptr);
  };
};

suite<"punctuation_classification"> punctuation_classification = [] {
  "punctuation is classified"_test = [] {
    auto result = classify_source("test.dao", "fn f(x: i32): i32\n    0\n");
    expect(find_token(result.tokens, "punctuation") != nullptr);
  };
};

suite<"sorted_output"> sorted_output = [] {
  "tokens are sorted by offset"_test = [] {
    auto result = classify_source("test.dao",
                                  "fn main(): i32\n"
                                  "    let x = 1\n"
                                  "    0\n");
    for (size_t i = 1; i < result.tokens.size(); ++i) {
      expect(result.tokens[i].span.offset >= result.tokens[i - 1].span.offset)
          << "tokens not sorted at index " << i;
    }
  };
};

suite<"example_files"> example_files = [] {
  "all examples classify without crash"_test = [] {
    auto root = std::filesystem::path(DAO_SOURCE_DIR);
    auto examples_dir = root / "examples";
    for (const auto& entry : std::filesystem::directory_iterator(examples_dir)) {
      if (entry.path().extension() != ".dao") {
        continue;
      }
      auto contents = read_file(entry.path());
      auto result = classify_source(entry.path().filename().string(), std::move(contents));
      // Every example should produce at least some semantic tokens.
      expect(result.tokens.size() > 0u) << "no tokens for " << entry.path().filename().string();
    }
  };

  "all syntax probes classify without crash"_test = [] {
    auto root = std::filesystem::path(DAO_SOURCE_DIR);
    auto probes_dir = root / "spec" / "syntax_probes";
    for (const auto& entry : std::filesystem::directory_iterator(probes_dir)) {
      if (entry.path().extension() != ".dao") {
        continue;
      }
      auto contents = read_file(entry.path());
      auto result = classify_source(entry.path().filename().string(), std::move(contents));
      expect(result.tokens.size() > 0u) << "no tokens for " << entry.path().filename().string();
    }
  };
};

suite<"taxonomy_coverage"> taxonomy_coverage = [] {
  "all lexically determinable categories appear in hello.dao"_test = [] {
    auto root = std::filesystem::path(DAO_SOURCE_DIR);
    auto contents = read_file(root / "examples" / "hello.dao");
    auto result = classify_source("hello.dao", std::move(contents));

    std::unordered_set<std::string_view> categories;
    for (const auto& tok : result.tokens) {
      categories.insert(tok.kind);
    }

    // hello.dao has: fn main(): i32, print("hello, dao!"), return 0
    expect(categories.contains("keyword.fn")) << "missing keyword.fn";
    expect(categories.contains("keyword.return")) << "missing keyword.return";
    expect(categories.contains("decl.function")) << "missing decl.function";
    expect(categories.contains("literal.number")) << "missing literal.number";
    expect(categories.contains("literal.string")) << "missing literal.string";
    expect(categories.contains("type.builtin")) << "missing type.builtin";
    expect(categories.contains("punctuation")) << "missing punctuation";
    expect(categories.contains("operator.context")) << "missing operator.context";
  };
};

suite<"expansion_classification"> expansion_classification = [] {
  "parameters and let binders are declaration sites"_test = [] {
    auto result = classify_source_resolved(
        "test.dao",
        "fn add(a: i32, b: i32): i32\n"
        "    let total: i32 = a + b\n"
        "    return total\n");
    expect(find_token_at(result, "decl.variable.param", "a") != nullptr);
    expect(find_token_at(result, "decl.variable.param", "b") != nullptr);
    expect(find_token_at(result, "decl.variable.local", "total") != nullptr);
    // Uses keep their use categories.
    expect(find_token_at(result, "use.variable.param", "a") != nullptr);
    expect(find_token_at(result, "use.variable.local", "total") != nullptr);
  };

  "for binders and match bindings are declaration sites"_test = [] {
    auto result = classify_source_resolved("test.dao",
                                           "enum class Shape:\n"
                                           "    Dot\n"
                                           "    Circle(radius: i32)\n"
                                           "fn main(): i32\n"
                                           "    let s: Shape = Shape::Dot\n"
                                           "    match s:\n"
                                           "        Shape::Circle(radius):\n"
                                           "            return radius\n"
                                           "        Shape::Dot:\n"
                                           "            return 0\n"
                                           "    return 1\n");
    expect(find_token_at(result, "decl.variable.local", "radius") != nullptr);
    expect(count_tokens(result.tokens, "use.variant") == 3_ul)
        << "Shape::Dot (expr), Shape::Circle and Shape::Dot (patterns)";
    expect(find_token_at(result, "use.type", "Shape") != nullptr)
        << "enum head used as a value";
    expect(find_token_at(result, "use.variable.local", "radius") != nullptr)
        << "binding used in the arm body";
  };

  "operators receive their expansion categories"_test = [] {
    auto result =
        classify_source_resolved("test.dao",
                                 "enum class Pair:\n"
                                 "    Both(a: i32, b: i32)\n"
                                 "fn f(p: *i32, x: i32, pair: Pair): bool\n"
                                 "    let y: i32 = x + 1 - 2 * 3 / 4 % 5\n"
                                 "    let q: i32 = *p\n"
                                 "    match pair:\n"
                                 "        Pair::Both(a, ..):\n"
                                 "            return a == x\n"
                                 "    return (y == x) and (y != x) or !(y < x) or y >= x\n");
    expect(count_tokens(result.tokens, "operator.arithmetic") == 5_ul);
    expect(count_tokens(result.tokens, "operator.comparison") == 5_ul);
    expect(count_tokens(result.tokens, "operator.logical") == 4_ul) << "and, or, !, or";
    expect(count_tokens(result.tokens, "operator.address") == 2_ul)
        << "pointer type `*i32` and deref `*p`";
    expect(find_token(result.tokens, "operator.range") != nullptr);
  };

  "member access and try are operators"_test = [] {
    auto result = classify_source_resolved(
        "test.dao",
        "class P:\n"
        "    x: i32\n"
        "fn g(p: P): i32\n"
        "    return p.x\n");
    expect(find_token(result.tokens, "operator.member") != nullptr);
    expect(find_token_at(result, "use.field", "x") != nullptr);

    auto propagated = classify_source_resolved("test.dao",
                                               "fn g(): Result<i32, string>\n"
                                               "    return Result::Ok(value = 1)\n"
                                               "fn h(): Result<i32, string>\n"
                                               "    let v: i32 = g()?\n"
                                               "    return Result::Ok(value = v)\n");
    expect(find_token(propagated.tokens, "operator.try") != nullptr);
  };

  "generic brackets are punctuation, comparisons stay comparisons"_test = [] {
    auto result = classify_source_resolved(
        "test.dao",
        "class Box<T>:\n"
        "    item: T\n"
        "fn first<T>(b: Box<T>): T -> b.item\n"
        "fn cmp(a: i32, b: i32): bool -> a < b\n");
    // `<T>` on the class, `<T>` on the function, `<T>` in the parameter
    // type: three opening brackets, three closing.
    expect(count_tokens(result.tokens, "punctuation") >= 6_ul);
    expect(count_tokens(result.tokens, "operator.comparison") == 1_ul) << "only `a < b`";
    expect(find_token_at(result, "decl.type", "T") != nullptr) << "type parameter declaration";
  };

  "bool literals are literals"_test = [] {
    auto result = classify_source("test.dao", "fn t(): bool -> true and false\n");
    expect(count_tokens(result.tokens, "literal.bool") == 2_ul);
  };

  "types used as values are type uses"_test = [] {
    auto result = classify_source_resolved(
        "test.dao",
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "fn origin(): Point -> Point(0, 0)\n");
    expect(find_token_at(result, "use.type", "Point") != nullptr) << "constructor call";
    expect(find_token_at(result, "type.nominal", "Point") != nullptr) << "return type";
  };

  "static method calls paint the type and the method"_test = [] {
    auto result = classify_source_resolved(
        "test.dao",
        "class Counter<T>:\n"
        "    n: i32\n"
        "    fn zero(): Counter<T> -> Counter(0)\n"
        "fn main(): i32\n"
        "    let c = Counter<i32>::zero()\n"
        "    return c.n\n");
    expect(find_token_at(result, "use.type", "Counter") != nullptr) << "static receiver";
    expect(find_token_at(result, "use.function", "zero") != nullptr) << "static method";
    expect(find_token_at(result, "type.builtin", "i32") != nullptr) << "explicit type argument";
    expect(find_token_at(result, "decl.function", "zero") != nullptr)
        << "class methods are walked";
  };

  "qualified enum variants paint the enum and the variant"_test = [] {
    auto result = classify_source_resolved(
        "test.dao",
        "enum Color:\n"
        "    Red\n"
        "    Green\n"
        "fn pick(): Color -> Color::Red\n");
    expect(find_token_at(result, "use.type", "Color") != nullptr);
    expect(find_token_at(result, "use.variant", "Red") != nullptr);
  };

  "conformance and extend bodies are walked"_test = [] {
    auto result = classify_source_resolved(
        "test.dao",
        "concept Shout:\n"
        "    fn shout(self): string\n"
        "class Dog:\n"
        "    name: string\n"
        "    as Shout:\n"
        "        fn shout(self): string -> self.name\n"
        "extend i32 as Shout:\n"
        "    fn shout(self): string -> \"i32\"\n");
    expect(count_tokens(result.tokens, "decl.function") == 3_ul)
        << "concept signature, conformance method, extend method";
    expect(count_tokens(result.tokens, "use.type") >= 2_ul)
        << "`as Shout` in the class and in the extend block";
    expect(find_token_at(result, "use.field", "name") != nullptr)
        << "field access inside a conformance method body";
  };
};

// NOLINTEND(readability-magic-numbers)

// ---------------------------------------------------------------------------
// Qualified paths across modules (CONTRACT_MODULE_SYSTEM.md §6): the
// resolver records what each segment resolved to at that segment's own
// offset, and classification must paint each segment from its own
// record rather than from what the head of the path implies.
// ---------------------------------------------------------------------------

namespace {

/// Classify one document of a multi-module program, so a path through
/// an import binding is classified with the modules it really crosses.
struct ClassifiedProgram {
  Program program;
  ResolveResult resolve_result;
  std::vector<SemanticToken> tokens;
};

auto classify_program(std::vector<std::pair<std::string, std::string>> files,
                      std::string_view document) -> std::unique_ptr<ClassifiedProgram> {
  std::vector<SourceInput> inputs;
  for (auto& [path, text] : files) {
    inputs.push_back({.display_path = path, .text = text, .is_prelude = false});
  }
  auto classified = std::make_unique<ClassifiedProgram>();
  classified->program = build_program(std::move(inputs));
  classified->resolve_result = resolve(classified->program);
  for (const auto& file : classified->program.files) {
    if (file->display_path != document) {
      continue;
    }
    expect(file->lex.diagnostics.empty() && file->parse.diagnostics.empty())
        << "fixture does not parse";
    classified->tokens =
        classify_tokens(file->lex.tokens, file->file(), &classified->resolve_result);
  }
  return classified;
}

/// The categories given to every token spelled `text`, in source order.
auto kinds_of(const ClassifiedProgram& classified, std::string_view text)
    -> std::vector<std::string_view> {
  std::vector<std::string_view> kinds;
  for (const auto& tok : classified.tokens) {
    if (classified.program.source_map.text(tok.span) == text) {
      kinds.push_back(tok.kind);
    }
  }
  return kinds;
}

using Kinds = std::vector<std::string_view>;

} // namespace

suite<"qualified_path_segments"> qualified_path_segments = [] {
  "b::T::m paints module, type, function"_test = [] {
    auto classified = classify_program(
        {{"lib.dao", "module lib\nclass Box:\n  n: i32\n  fn make(): Box -> Box(0)\n"},
         {"main.dao",
          "module app\nimport lib\nfn main(): i32\n  let b: Box = lib::Box::make()\n  return 0\n"}},
        "main.dao");
    expect(kinds_of(*classified, "lib") == Kinds{"decl.module", "use.module"})
        << "the import binding and the head of the path";
    expect(kinds_of(*classified, "Box") == Kinds{"type.nominal", "use.type"})
        << "the exported type, not another module segment";
    expect(kinds_of(*classified, "make") == Kinds{"use.function"});
  };

  "b::E::V paints module, type, variant"_test = [] {
    auto classified = classify_program({{"lib.dao", "module lib\nenum Color:\n  Red\n  Green\n"},
                                        {"main.dao",
                                         "module app\nimport lib\nfn main(): i32\n  let c: Color = "
                                         "lib::Color::Red\n  return 0\n"}},
                                       "main.dao");
    expect(kinds_of(*classified, "lib") == Kinds{"decl.module", "use.module"});
    expect(kinds_of(*classified, "Color") == Kinds{"type.nominal", "use.type"});
    expect(kinds_of(*classified, "Red") == Kinds{"use.variant"})
        << "the variant, not the enum type it is recorded as";
  };
};

auto main() -> int {
} // NOLINT(readability-named-parameter)
