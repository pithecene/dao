#include "frontend/resolve/resolve.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace boost::ut;
using namespace dao;

namespace {

struct ResolvedSource {
  SourceBuffer source;
  LexResult lex_result;
  ParseResult parse_result;
  ResolveResult resolve_result;
};

// Test helper: CONTRACT_SYNTAX_SURFACE.md requires every source file to
// begin with a `module` declaration. Inline test fixtures focus on
// resolver behavior below the module layer, so we prepend a canonical
// `module test` line before lexing. Callers that already supply a
// leading `module ...` line (e.g. the corpus tests below) pass
// `synthetic_module = false`.
auto resolve_source(const std::string& name, std::string contents,
                    bool synthetic_module = true) -> ResolvedSource {
  if (synthetic_module) {
    std::string wrapped = "module test\n";
    wrapped.append(contents);
    contents = std::move(wrapped);
  }
  SourceBuffer source(name, std::move(contents));
  auto lex_result = lex(source);
  ParseResult parse_result;
  ResolveResult resolve_result;

  if (lex_result.diagnostics.empty()) {
    parse_result = parse(lex_result.tokens);
    if (parse_result.file != nullptr) {
      resolve_result = resolve(*parse_result.file);
    }
  }

  return {std::move(source),
          std::move(lex_result),
          std::move(parse_result),
          std::move(resolve_result)};
}

auto has_diagnostic_containing(const ResolveResult& result, const std::string& text) -> bool {
  for (const auto& diag : result.diagnostics) {
    if (diag.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

auto use_resolves_to(const ResolvedSource& result, uint32_t offset, SymbolKind kind) -> bool {
  auto it = result.resolve_result.uses.find(offset);
  if (it == result.resolve_result.uses.end()) {
    return false;
  }
  return it->second->kind == kind;
}

// Find the offset of a substring in the source.
auto find_offset(const ResolvedSource& result, const std::string& text, size_t nth = 0)
    -> uint32_t {
  auto contents = result.source.contents();
  size_t pos = 0;
  for (size_t i = 0; i <= nth; ++i) {
    pos = contents.find(text, pos);
    if (pos == std::string_view::npos) {
      return UINT32_MAX;
    }
    if (i < nth) {
      pos += text.size();
    }
  }
  return static_cast<uint32_t>(pos);
}


// ---------------------------------------------------------------------------
// Multi-module programs (Task 31 D2): files named `stdlib/...` form the
// prelude group; the rest are user modules.
// ---------------------------------------------------------------------------

struct ResolvedProgram {
  Program program;
  ResolveResult result;
};

using NamedSource = std::pair<std::string, std::string>;

auto resolve_program(std::vector<NamedSource> files) -> ResolvedProgram {
  std::vector<SourceInput> inputs;
  for (auto& [display, text] : files) {
    inputs.push_back({.display_path = display, .text = text,
                      .is_prelude = display.starts_with("stdlib/")});
  }
  ResolvedProgram resolved{.program = build_program(std::move(inputs)), .result = {}};
  resolved.result = resolve(resolved.program);
  return resolved;
}

auto file_named(const ResolvedProgram& resolved, std::string_view display) -> const SourceFile& {
  for (const auto& file : resolved.program.files) {
    if (file->display_path == display) {
      return *file;
    }
  }
  throw std::runtime_error("no file " + std::string(display));
}

/// Program offset of the n-th occurrence of `text` in a file.
auto offset_in(const ResolvedProgram& resolved, std::string_view display, std::string_view text,
               size_t occurrence = 0) -> uint32_t {
  const auto& file = file_named(resolved, display);
  auto contents = file.buffer.contents();
  size_t pos = std::string::npos;
  for (size_t i = 0, from = 0; i <= occurrence; ++i, from = pos + 1) {
    pos = contents.find(text, from);
    if (pos == std::string::npos) {
      throw std::runtime_error("no occurrence of " + std::string(text));
    }
  }
  return file.base_offset + static_cast<uint32_t>(pos);
}

auto use_in(const ResolvedProgram& resolved, std::string_view display, std::string_view text,
            size_t occurrence = 0) -> const Symbol* {
  auto it = resolved.result.uses.find(offset_in(resolved, display, text, occurrence));
  return it == resolved.result.uses.end() ? nullptr : it->second;
}

auto messages_of(const ResolvedProgram& resolved) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& diag : resolved.result.diagnostics) {
    out.push_back(diag.message);
  }
  return out;
}

auto joined(const std::vector<std::string>& items) -> std::string {
  std::string out;
  for (const auto& item : items) {
    out += item + " | ";
  }
  return out;
}

// Literals, not std::string objects: boost.ut runs suites after main
// returns, when namespace-scope objects are already destroyed.
constexpr const char* kMathModule =
    "module app::math\n"
    "import app::util\n"
    "fn add(a: i32, b: i32): i32 -> a + b\n"
    "class Point:\n"
    "  x: i32\n"
    "  fn origin(): Point -> Point(0)\n"
    "enum Color:\n"
    "  Red\n"
    "  Green\n";

constexpr const char* kUtilModule = "module app::util\nfn one(): i32 -> 1\n";

} // namespace

suite<"resolve_modules"> resolve_modules = [] {
  "qualified_function_resolves_through_the_import"_test = [] {
    auto resolved = resolve_program({
        {"main.dao", "module app::main\nimport app::math\nfn main(): i32 -> math::add(1, 2)\n"},
        {"math.dao", kMathModule},
        {"util.dao", kUtilModule},
    });
    expect(resolved.result.diagnostics.empty()) << joined(messages_of(resolved));
    const auto* head = use_in(resolved, "main.dao", "math::add");
    expect(head != nullptr && head->kind == SymbolKind::Module);
    expect(head != nullptr && head->decl_as_module()->display == "app::math");
    const auto* add = use_in(resolved, "main.dao", "add(1");
    expect(add != nullptr && add->kind == SymbolKind::Function && add->name == "add");
    expect(add != nullptr && add->module != nullptr && add->module->display == "app::math");
  };

  "qualified_static_method_and_variant"_test = [] {
    auto resolved = resolve_program({
        {"main.dao", "module app::main\nimport app::math\n"
                     "fn main(): i32\n  let p: i32 = math::Point::origin()\n"
                     "  let c: i32 = math::Color::Red\n  return 0\n"},
        {"math.dao", kMathModule},
        {"util.dao", kUtilModule},
    });
    expect(resolved.result.diagnostics.empty()) << joined(messages_of(resolved));
    const auto* point = use_in(resolved, "main.dao", "Point::origin");
    expect(point != nullptr && point->kind == SymbolKind::Type && point->name == "Point");
    const auto* origin = use_in(resolved, "main.dao", "origin()");
    expect(origin != nullptr && origin->name == "Point.origin") << "static method by mangled name";
    const auto* red = use_in(resolved, "main.dao", "Red");
    expect(red != nullptr && red->kind == SymbolKind::Type && red->name == "Color")
        << "variant recorded as its enum for the checker";
  };

  "unknown_export_is_diagnosed"_test = [] {
    auto resolved = resolve_program({
        {"main.dao", "module app::main\nimport app::math\nfn main(): i32 -> math::nope()\n"},
        {"math.dao", kMathModule},
        {"util.dao", kUtilModule},
    });
    expect(messages_of(resolved) == std::vector<std::string>{"module 'app::math' has no export 'nope'"})
        << joined(messages_of(resolved));
  };

  "import_bindings_are_not_reexported"_test = [] {
    auto resolved = resolve_program({
        {"main.dao", "module app::main\nimport app::math\nfn main(): i32 -> math::util::one()\n"},
        {"math.dao", kMathModule},
        {"util.dao", kUtilModule},
    });
    expect(messages_of(resolved) == std::vector<std::string>{"module 'app::math' has no export 'util'"})
        << joined(messages_of(resolved));
  };

  "deeper_path_through_a_binding_is_an_error"_test = [] {
    auto resolved = resolve_program({
        {"main.dao", "module app::main\nimport app::math\nfn main(): i32 -> math::Point::origin::x()\n"},
        {"math.dao", kMathModule},
        {"util.dao", kUtilModule},
    });
    expect(!messages_of(resolved).empty() &&
           messages_of(resolved)[0].starts_with("'math::Point::origin::x': a path through import binding"))
        << joined(messages_of(resolved));
  };

  "import_binding_collides_with_a_declaration"_test = [] {
    auto resolved = resolve_program({
        {"main.dao", "module app::main\nimport app::math\nfn math(): i32 -> 0\nfn main(): i32 -> 0\n"},
        {"math.dao", kMathModule},
        {"util.dao", kUtilModule},
    });
    expect(messages_of(resolved) == std::vector<std::string>{"duplicate top-level declaration 'math'"})
        << joined(messages_of(resolved));
  };

  "import_binding_collides_with_an_import"_test = [] {
    auto resolved = resolve_program({
        {"main.dao", "module app::main\nimport app::math\nimport other::math\nfn main(): i32 -> 0\n"},
        {"math.dao", kMathModule},
        {"util.dao", kUtilModule},
        {"other.dao", "module other::math\n"},
    });
    expect(messages_of(resolved) == std::vector<std::string>{"duplicate top-level declaration 'math'"})
        << joined(messages_of(resolved));
  };

  "prelude_names_are_visible_unqualified_and_shadowable"_test = [] {
    auto resolved = resolve_program({
        {"stdlib/core/helper.dao", "module core::helper\nfn helper(): i32 -> 1\nfn shared(): i32 -> 2\n"},
        {"main.dao", "module app::main\nfn shared(): i32 -> 3\n"
                     "fn main(): i32 -> helper() + shared()\n"},
    });
    expect(resolved.result.diagnostics.empty()) << joined(messages_of(resolved));
    const auto* helper = use_in(resolved, "main.dao", "helper()");
    expect(helper != nullptr && helper->module != nullptr && helper->module->is_prelude);
    const auto* shared = use_in(resolved, "main.dao", "shared()", 1); // 0 is the declaration
    expect(shared != nullptr && shared->module != nullptr && shared->module->display == "app::main")
        << "the module's own declaration shadows the prelude's";
  };

  "prelude_symbols_resolve_through_an_import_too"_test = [] {
    auto resolved = resolve_program({
        {"stdlib/core/vec.dao", "module core::vec\nclass Vec:\n  n: i32\n  fn make(): Vec -> Vec(0)\n"},
        {"main.dao", "module app::main\nimport core::vec\n"
                     "fn main(): i32\n  let a: i32 = vec::Vec::make()\n  let b: i32 = Vec::make()\n  return 0\n"},
    });
    expect(resolved.result.diagnostics.empty()) << joined(messages_of(resolved));
    const auto* qualified = use_in(resolved, "main.dao", "make()");   // recorded at the member
    const auto* unqualified = use_in(resolved, "main.dao", "Vec::make", 1); // recorded at the head
    expect(qualified != nullptr && qualified == unqualified && qualified->name == "Vec.make")
        << "one prelude symbol either way";
  };

  "prelude_class_methods_resolve_regardless_of_module_order"_test = [] {
    // `aaa` sorts before `core::vec` and imports nothing, so its bodies
    // resolve before the prelude's; method names must already be declared.
    auto resolved = resolve_program({
        {"stdlib/core/vec.dao", "module core::vec\nclass Vec:\n  n: i32\n  fn make(): Vec -> Vec(0)\n"},
        {"aaa.dao", "module aaa\nfn main(): i32\n  let v: i32 = Vec::make()\n  return 0\n"},
    });
    expect(resolved.result.diagnostics.empty()) << joined(messages_of(resolved));
    const auto* make = use_in(resolved, "aaa.dao", "Vec::make");
    expect(make != nullptr && make->name == "Vec.make");
  };

  "builtins_cannot_be_redeclared_anywhere"_test = [] {
    auto resolved = resolve_program({
        {"stdlib/core/bad.dao", "module core::bad\nfn null_ptr(): i32 -> 0\n"},
        {"main.dao", "module app::main\nimport app::lib\nclass string:\n  n: i32\nfn main(): i32 -> 0\n"},
        {"lib.dao", "module app::lib\nclass bool:\n  n: i32\n"},
    });
    auto messages = messages_of(resolved);
    std::sort(messages.begin(), messages.end());
    expect(messages == std::vector<std::string>{"duplicate top-level declaration 'bool'",
                                                "duplicate top-level declaration 'null_ptr'",
                                                "duplicate top-level declaration 'string'"})
        << joined(messages);
  };

  "reserved_prefix_is_keyed_on_the_prelude_group"_test = [] {
    auto resolved = resolve_program({
        {"stdlib/core/hooks.dao", "module core::hooks\nextern fn __dao_x(): i32\n"},
        {"main.dao", "module app::main\nfn __dao_y(): i32 -> 0\nfn main(): i32 -> 0\n"},
    });
    expect(messages_of(resolved) == std::vector<std::string>{
                                        "'__dao_y': the '__dao_' prefix is reserved for compiler/runtime use"})
        << joined(messages_of(resolved));
  };

  "modules_own_their_scopes_and_symbols"_test = [] {
    auto resolved = resolve_program({
        {"main.dao", "module app::main\nimport app::util\nfn main(): i32\n  let n: i32 = util::one()\n  return n\n"},
        {"util.dao", kUtilModule},
    });
    expect(resolved.result.diagnostics.empty()) << joined(messages_of(resolved));
    const auto* util = resolved.program.module_named("app::util");
    expect(util != nullptr && util->scope != nullptr && util->scope->lookup_local("one") != nullptr);
    expect(util != nullptr && util->scope->lookup_local("main") == nullptr) << "scopes are per module";
    const auto* n = use_in(resolved, "main.dao", "n\n", 1); // 0 ends `module app::main`
    expect(n != nullptr && n->kind == SymbolKind::Local && n->module != nullptr &&
           n->module->display == "app::main");
  };
};

namespace {
} // namespace

// NOLINTBEGIN(readability-magic-numbers)

suite<"resolve_basic"> resolve_basic = [] {
  "simple identifier resolves to param"_test = [] {
    auto result = resolve_source("test", "fn foo(x: i32): i32 -> x");
    expect(result.resolve_result.diagnostics.empty());

    // 'x' at position of the expression body should resolve to Param
    auto x_use_offset = find_offset(result, "x", 1); // second 'x' is the use
    expect(use_resolves_to(result, x_use_offset, SymbolKind::Param));
  };

  "simple identifier resolves to local"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(): i32\n"
                                 "    let value: i32 = 42\n"
                                 "    value");
    expect(result.resolve_result.diagnostics.empty());

    // 'value' on the last line should resolve to Local
    auto val_use_offset = find_offset(result, "value", 1);
    expect(use_resolves_to(result, val_use_offset, SymbolKind::Local));
  };

  "unknown identifier produces diagnostic"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(): i32\n"
                                 "    unknown_var");
    expect(has_diagnostic_containing(result.resolve_result, "unknown name 'unknown_var'"));
  };

  "forward reference to function"_test = [] {
    auto result = resolve_source("test",
                                 "fn caller(): i32 -> callee()\n"
                                 "fn callee(): i32 -> 0");
    expect(result.resolve_result.diagnostics.empty());

    auto callee_offset = find_offset(result, "callee", 0); // first occurrence is the call
    expect(use_resolves_to(result, callee_offset, SymbolKind::Function));
  };

  "qualified name rejects non-module leading segment"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(): i32 -> 0\n"
                                 "fn main(): i32\n"
                                 "    foo::bar()");
    expect(has_diagnostic_containing(result.resolve_result, "'foo' is not a module"));
  };
};

suite<"resolve_scoping"> resolve_scoping = [] {
  "let binding not visible before declaration"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(): i32\n"
                                 "    let a: i32 = b\n"
                                 "    let b: i32 = 0\n"
                                 "    a");
    // 'b' is used before its let declaration — produces unknown name.
    expect(has_diagnostic_containing(result.resolve_result, "unknown name 'b'"));
    auto b_use_offset = find_offset(result, "b", 0); // first 'b' in the initializer
    auto it = result.resolve_result.uses.find(b_use_offset);
    expect(it == result.resolve_result.uses.end()) << "b should not resolve before its declaration";
  };

  "if block creates new scope"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(x: i32): i32\n"
                                 "    if x > 0:\n"
                                 "        let inner: i32 = 1\n"
                                 "        inner\n"
                                 "    x");
    // 'inner' should resolve inside the if block, 'x' outside
    expect(result.resolve_result.diagnostics.empty());
  };

  "for loop variable scoped to body"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(xs: i32): i32\n"
                                 "    for item in xs:\n"
                                 "        item\n"
                                 "    0");
    expect(result.resolve_result.diagnostics.empty());

    // 'item' on the body line should resolve to Local
    auto item_use_offset = find_offset(result, "item", 1);
    expect(use_resolves_to(result, item_use_offset, SymbolKind::Local));
  };

  "lambda param scoped to lambda body"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(x: i32): i32 -> |y| -> y + x");
    expect(result.resolve_result.diagnostics.empty());

    // 'y' in the body should resolve to LambdaParam
    auto y_use_offset = find_offset(result, "y", 1);
    expect(use_resolves_to(result, y_use_offset, SymbolKind::LambdaParam));
  };

  "nested scopes shadow outer"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(x: i32): i32\n"
                                 "    let x: i32 = 42\n"
                                 "    x");
    // The inner 'x' shadows the parameter — no error, resolves to Local
    expect(result.resolve_result.diagnostics.empty());
    auto x_use_offset = find_offset(result, "x", 2); // third 'x' is the use
    expect(use_resolves_to(result, x_use_offset, SymbolKind::Local));
  };
};

suite<"resolve_duplicates"> resolve_duplicates = [] {
  "duplicate top-level functions"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(): i32 -> 0\n"
                                 "fn foo(): i32 -> 1");
    expect(has_diagnostic_containing(result.resolve_result, "duplicate top-level declaration 'foo'"));
  };

  "duplicate parameters"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(x: i32, x: i32): i32 -> x");
    expect(has_diagnostic_containing(result.resolve_result, "duplicate parameter 'x'"));
  };

  "duplicate let in same scope"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(): i32\n"
                                 "    let a: i32 = 1\n"
                                 "    let a: i32 = 2\n"
                                 "    a");
    expect(has_diagnostic_containing(result.resolve_result, "duplicate declaration 'a'"));
  };

  "duplicate lambda parameters"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(): i32 -> |x, x| -> x");
    expect(has_diagnostic_containing(result.resolve_result, "duplicate parameter 'x'"));
  };
};

suite<"resolve_overloads"> resolve_overloads = [] {
  "arity-based overloading is allowed"_test = [] {
    auto result = resolve_source("test",
        "fn foo(a: i32): i32 -> a\n"
        "fn foo(a: i32, b: i32): i32 -> a\n");
    expect(result.resolve_result.diagnostics.empty())
        << "different arities should not be a duplicate error";
  };

  "same-arity duplicate is still rejected"_test = [] {
    auto result = resolve_source("test",
        "fn foo(a: i32): i32 -> a\n"
        "fn foo(b: i32): i32 -> b\n");
    expect(has_diagnostic_containing(result.resolve_result,
        "duplicate top-level declaration 'foo'"));
  };

  "three overloads with different arities"_test = [] {
    auto result = resolve_source("test",
        "fn bar(): i32 -> 0\n"
        "fn bar(a: i32): i32 -> a\n"
        "fn bar(a: i32, b: i32): i32 -> a\n");
    expect(result.resolve_result.diagnostics.empty())
        << "three different arities should all be allowed";
  };

  "same-arity duplicate after overload set exists is rejected"_test = [] {
    auto result = resolve_source("test",
        "fn foo(a: i32): i32 -> a\n"
        "fn foo(a: i32, b: i32): i32 -> a\n"
        "fn foo(x: i32, y: i32): i32 -> x\n");
    expect(has_diagnostic_containing(result.resolve_result,
        "duplicate top-level declaration 'foo'"));
  };

  "non-function duplicate with same name is still rejected"_test = [] {
    auto result = resolve_source("test",
        "fn foo(a: i32): i32 -> a\n"
        "class foo:\n"
        "  x: i32\n");
    expect(has_diagnostic_containing(result.resolve_result,
        "duplicate top-level declaration 'foo'"));
  };
};

suite<"resolve_types"> resolve_types = [] {
  "builtin type resolves"_test = [] {
    auto result = resolve_source("test", "fn foo(x: i32): i32 -> x");
    expect(result.resolve_result.diagnostics.empty());

    // i32 in param type should resolve to Builtin
    // The first 'i32' in "x: i32" — find its offset
    auto int32_offset = find_offset(result, "i32", 0);
    expect(use_resolves_to(result, int32_offset, SymbolKind::Builtin));
  };

  "unknown nominal type does NOT produce diagnostic"_test = [] {
    auto result = resolve_source("test", "fn foo(x: NodeId): i32 -> 0");
    // NodeId is unknown but type-position references are not diagnosed
    expect(result.resolve_result.diagnostics.empty());
  };

  "user-declared type resolves"_test = [] {
    auto result = resolve_source("test",
                                 "class Point:\n"
                                 "    x: i32\n"
                                 "    y: i32\n"
                                 "fn foo(p: Point): i32 -> 0");
    expect(result.resolve_result.diagnostics.empty());

    auto point_offset = find_offset(result, "Point", 1); // second 'Point' is the type use
    expect(use_resolves_to(result, point_offset, SymbolKind::Type));
  };
};

suite<"resolve_imports"> resolve_imports = [] {
  "import binds last segment"_test = [] {
    auto result = resolve_source("test",
                                 "import net::http\n"
                                 "fn foo(): i32 -> 0");
    expect(result.resolve_result.diagnostics.empty());

    // 'http' should be declared as a Module symbol
    // We can verify it doesn't produce diagnostics and is in the uses table
    // when referenced
  };

  "qualified name first segment resolves to module"_test = [] {
    auto result = resolve_source("test",
                                 "import net::http\n"
                                 "fn foo(): i32\n"
                                 "    http::get()");
    expect(result.resolve_result.diagnostics.empty());

    // 'http' in 'http::get()' should resolve to Module
    auto http_offset = find_offset(result, "http", 1); // second 'http' is the use
    expect(use_resolves_to(result, http_offset, SymbolKind::Module));
  };

  "unresolved first segment of qualified name produces diagnostic"_test = [] {
    auto result = resolve_source("test",
                                 "fn foo(): i32\n"
                                 "    unknown::get()");
    expect(has_diagnostic_containing(result.resolve_result, "unknown name 'unknown'"));
  };
};

suite<"resolve_class"> resolve_class = [] {
  "class fields declared"_test = [] {
    auto result = resolve_source("test",
                                 "class Point:\n"
                                 "    x: i32\n"
                                 "    y: i32");
    expect(result.resolve_result.diagnostics.empty());
  };

  "duplicate class field"_test = [] {
    auto result = resolve_source("test",
                                 "class Point:\n"
                                 "    x: i32\n"
                                 "    x: i32");
    expect(has_diagnostic_containing(result.resolve_result, "duplicate declaration 'x'"));
  };
};

suite<"resolve_corpus"> resolve_corpus = [] {
  "all examples resolve without spurious diagnostics"_test = [] {
    std::filesystem::path root(DAO_SOURCE_DIR);
    auto examples_dir = root / "examples";
    // Each example is resolved as the user file of a program whose
    // prelude group is the real stdlib, exactly as the driver builds it.
    auto prelude = stdlib_prelude_sources(root);

    for (const auto& entry : std::filesystem::directory_iterator(examples_dir)) {
      if (entry.path().extension() != ".dao") {
        continue;
      }

      auto program = make_test_program(read_file(entry.path()), prelude);
      auto resolve_result = resolve(program);

      // No value-position diagnostics should fire on example files.
      // Skip prelude-origin diagnostics.
      for (const auto& diag : resolve_result.diagnostics) {
        if (program.source_map.is_prelude(diag.span.offset)) {
          continue;
        }
        expect(false) << entry.path().filename().string() << ": " << diag.message;
      }
    }
  };

  "all syntax probes resolve without spurious diagnostics"_test = [] {
    std::filesystem::path root(DAO_SOURCE_DIR);
    auto probes_dir = root / "spec" / "syntax_probes";

    for (const auto& entry : std::filesystem::directory_iterator(probes_dir)) {
      if (entry.path().extension() != ".dao") {
        continue;
      }

      // Syntax probes already declare their own `module` per the
      // contract, so they parse as standalone single-file units with
      // no synthetic wrapper needed.
      auto contents = read_file(entry.path());
      auto result =
          resolve_source(entry.path().filename().string(),
                         std::move(contents), /*synthetic_module=*/false);

      for (const auto& diag : result.resolve_result.diagnostics) {
        expect(false) << entry.path().filename().string() << ": " << diag.message;
      }
    }
  };
};

suite<"resolve_generics"> resolve_generics = [] {
  "generic type param resolves in param type"_test = [] {
    auto result = resolve_source("test",
                                 "fn identity<T>(x: T): T -> x");
    expect(result.resolve_result.diagnostics.empty())
        << "no resolve errors for generic function";

    // 'T' in the param type position should resolve to GenericParam.
    // The first 'T' is the declaration; the second is in 'x: T'.
    auto t_param_offset = find_offset(result, "T", 1);
    expect(use_resolves_to(result, t_param_offset, SymbolKind::GenericParam))
        << "T in param type resolves to GenericParam";
  };

  "generic type param resolves in return type"_test = [] {
    auto result = resolve_source("test",
                                 "fn identity<T>(x: T): T -> x");
    expect(result.resolve_result.diagnostics.empty());

    // 'T' in return type position (third occurrence).
    auto t_ret_offset = find_offset(result, "T", 2);
    expect(use_resolves_to(result, t_ret_offset, SymbolKind::GenericParam))
        << "T in return type resolves to GenericParam";
  };

  "generic class type param resolves in field type"_test = [] {
    auto result = resolve_source("test",
                                 "class Box<T>:\n"
                                 "    value: T\n");
    expect(result.resolve_result.diagnostics.empty())
        << "no resolve errors for generic class";

    // 'T' in the field type should resolve to GenericParam.
    auto t_field_offset = find_offset(result, "T", 1);
    expect(use_resolves_to(result, t_field_offset, SymbolKind::GenericParam))
        << "T in field type resolves to GenericParam";
  };

  "duplicate type param is an error"_test = [] {
    auto result = resolve_source("test",
                                 "fn bad<T, T>(x: T): T -> x");
    expect(has_diagnostic_containing(result.resolve_result, "duplicate type parameter"))
        << "duplicate type parameter should be an error";
  };
};

// ---------------------------------------------------------------------------
// Concept resolution
// ---------------------------------------------------------------------------

suite<"resolve_concepts"> resolve_concepts = [] {
  "concept name resolves as Concept symbol"_test = [] {
    auto result = resolve_source("test",
        "concept Printable:\n"
        "    fn to_string(self): string\n");
    expect(result.resolve_result.diagnostics.empty())
        << "concept declaration should resolve cleanly";
  };

  "concept used as constraint resolves"_test = [] {
    auto result = resolve_source("test",
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "fn print<T: Printable>(x: T): void\n"
        "    return\n");
    expect(result.resolve_result.diagnostics.empty())
        << "concept constraint should resolve";
    // The 'Printable' in the constraint should resolve to a Concept symbol.
    auto constraint_offset = find_offset(result, "Printable", 1);
    expect(use_resolves_to(result, constraint_offset, SymbolKind::Concept))
        << "Printable constraint resolves to Concept symbol";
  };

  "conformance block resolves concept name"_test = [] {
    auto result = resolve_source("test",
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "class Point:\n"
        "    x: f64\n"
        "    as Printable:\n"
        "        fn to_string(self): string -> \"p\"\n");
    expect(result.resolve_result.diagnostics.empty())
        << "conformance block should resolve";
    // 'Printable' after 'as' should resolve to Concept symbol.
    auto conf_offset = find_offset(result, "Printable", 1);
    expect(use_resolves_to(result, conf_offset, SymbolKind::Concept))
        << "conformance concept name resolves to Concept symbol";
  };

  "extend declaration resolves concept and type"_test = [] {
    auto result = resolve_source("test",
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> \"num\"\n");
    expect(result.resolve_result.diagnostics.empty())
        << "extend declaration should resolve";
    auto ext_offset = find_offset(result, "Printable", 1);
    expect(use_resolves_to(result, ext_offset, SymbolKind::Concept))
        << "extend concept name resolves to Concept symbol";
  };
};

// ---------------------------------------------------------------------------
// Reserved prefix enforcement
// ---------------------------------------------------------------------------

suite<"reserved_prefix"> reserved_prefix = [] {
  "reserved __dao_ prefix rejected in user code"_test = [] {
    auto result = resolve_source("test.dao",
        "fn __dao_evil(): void\n"
        "  return\n");
    expect(result.resolve_result.diagnostics.size() == 1_u)
        << "should reject __dao_ prefix";
    expect(result.resolve_result.diagnostics[0].message.find("__dao_")
           != std::string::npos)
        << "error message mentions __dao_";
  };

  "reserved __dao_ prefix allowed in prelude files"_test = [] {
    // The extern declaration lives in a prelude-group file; the user
    // file only calls it.
    std::vector<std::string> prelude = {
        "module core::equality\n"
        "extern fn __dao_eq_i32(a: i32, b: i32): bool\n"};
    auto program = make_test_program("fn main(): void\n  return\n", prelude);
    auto resolve_result = resolve(program);
    for (const auto& diag : resolve_result.diagnostics) {
      expect(diag.message.find("__dao_") == std::string::npos)
          << "prelude __dao_ should not be rejected: " << diag.message;
    }
  };

  "reserved __dao_ prefix rejected in user file even with a prelude"_test = [] {
    std::vector<std::string> prelude = {"module core::probe\nfn zero(): i32 -> 0\n"};
    auto program = make_test_program(
        "extern fn __dao_mine(a: i32): bool\nfn main(): void\n  return\n", prelude);
    auto resolve_result = resolve(program);
    expect(has_diagnostic_containing(resolve_result, "__dao_"))
        << "user __dao_ declaration must be rejected";
  };
};

// NOLINTEND(readability-magic-numbers)

// ---------------------------------------------------------------------------
// Prelude qualified exports (CONTRACT_MODULE_SYSTEM.md §7.2, §7.3, §7.5):
// prelude declarations are visible unqualified everywhere and prelude
// modules see one another as one namespace, but a qualified path reaches
// only what the named module itself declares.
// ---------------------------------------------------------------------------

suite<"prelude_qualified_exports"> prelude_qualified_exports_suite = [] {
  const std::vector<std::string> prelude = {
      "module core::vector\nfn make(): i32 -> 1\n",
      "module core::printable\nfn show(): i32 -> 2\n",
  };

  "a prelude module exports only what it declares"_test = [prelude] {
    auto program = make_test_program(
        "import core::vector\nfn use_it(): i32\n  return vector::show()\n", prelude);
    auto resolved = resolve(program);
    expect(has_diagnostic_containing(resolved, "has no export 'show'"))
        << "core::vector must not export core::printable's `show`";
  };

  "a prelude module's own export resolves qualified"_test = [prelude] {
    auto program = make_test_program(
        "import core::vector\nfn use_it(): i32\n  return vector::make()\n", prelude);
    auto resolved = resolve(program);
    expect(!has_diagnostic_containing(resolved, "has no export")) << "core::vector exports `make`";
  };

  "prelude names stay visible unqualified across prelude modules"_test = [prelude] {
    auto program = make_test_program("fn use_it(): i32\n  return make() + show()\n", prelude);
    auto resolved = resolve(program);
    expect(!has_diagnostic_containing(resolved, "unknown name"))
        << "prelude declarations are visible unqualified (§7.2, §7.3)";
  };
};

// ---------------------------------------------------------------------------
// Qualified forms reach the right export (CONTRACT_MODULE_SYSTEM.md §6):
// an overload set is selected by the call's arity, and a static member that
// does not exist is not silently answered with its type.
// ---------------------------------------------------------------------------

suite<"qualified_export_selection"> qualified_export_selection_suite = [] {
  const std::vector<std::string> prelude = {};

  "a qualified call binds the overload its arity names"_test = [] {
    // Three arity-distinct overloads; each call must reach its own.
    const std::string lib = "module lib\nfn f(): i32 -> 0\nfn f(a: i32): i32 -> a\n"
                            "fn f(a: i32, b: i32): i32 -> a + b\n";
    const std::string app = "module app\nimport lib\n"
                            "fn use_all(): i32\n  return lib::f() + lib::f(1) + lib::f(1, 2)\n";
    for (bool lib_first : {true, false}) {
      std::vector<SourceInput> inputs;
      if (lib_first) {
        inputs = {{.display_path = "lib.dao", .text = lib, .is_prelude = false},
                  {.display_path = "app.dao", .text = app, .is_prelude = false}};
      } else {
        inputs = {{.display_path = "app.dao", .text = app, .is_prelude = false},
                  {.display_path = "lib.dao", .text = lib, .is_prelude = false}};
      }
      auto program = build_program(std::move(inputs));
      auto resolved = resolve(program);
      // Each call site resolves to a function of the matching arity.
      std::vector<size_t> arities;
      for (const auto& [offset, sym] : resolved.uses) {
        if (sym == nullptr || sym->decl == nullptr || sym->name.substr(0, 1) != "f") {
          continue;
        }
        const auto* decl = sym->decl_as_decl();
        if (decl->is<FunctionDecl>()) {
          arities.push_back(decl->as<FunctionDecl>().params.size());
        }
      }
      std::ranges::sort(arities);
      arities.erase(std::unique(arities.begin(), arities.end()), arities.end());
      expect(arities == std::vector<size_t>{0, 1, 2})
          << "declaration order " << (lib_first ? "lib first" : "app first") << " reached arities "
          << arities.size();
    }
  };

  "an unknown static member of an imported type is diagnosed"_test = [] {
    auto program = build_program(
        {{.display_path = "lib.dao",
          .text = "module lib\nclass P:\n  x: i32\n",
          .is_prelude = false},
         {.display_path = "app.dao",
          .text = "module app\nimport lib\nfn f(): lib::P\n  return lib::P::missing(1)\n",
          .is_prelude = false}});
    auto resolved = resolve(program);
    expect(has_diagnostic_containing(resolved, "has no static member 'missing'"))
        << "a missing static member must not resolve to its type";
  };
};

auto main() -> int {
}
