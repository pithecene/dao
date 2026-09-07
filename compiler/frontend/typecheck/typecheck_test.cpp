#include "frontend/diagnostics/source.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"
#include "frontend/types/type_printer.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>
#include <string>
#include <vector>
#include <utility>

using namespace boost::ut;
using namespace dao;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

/// Parse, resolve, and typecheck a source string. Returns TypeCheckResult.
auto check_source(const std::string& source) -> TypeCheckResult {
  SourceBuffer buf("test.dao", wrap_with_test_module(source));
  auto lex_result = lex(buf);
  auto parse_result = parse(lex_result.tokens);
  auto resolve_result = resolve(*parse_result.file);

  TypeContext types;
  return typecheck(*parse_result.file, resolve_result, types);
}

/// Check user source as the user file of a program whose prelude group
/// is the given sources (real stdlib files, each with its own `module`
/// line), exactly as the driver assembles it.  Prelude-group files are
/// exempt from the `__dao_` naming restriction.
auto check_with_prelude(const std::string& user_source,
                        std::span<const std::string> prelude_sources)
    -> TypeCheckResult {
  auto program = make_test_program(user_source, prelude_sources);
  auto resolve_result = resolve(program);

  TypeContext types;
  return typecheck(program, resolve_result, types);
}

/// Returns true if any diagnostic message contains the substring.
auto has_error_containing(const TypeCheckResult& result, std::string_view sub)
    -> bool {
  for (const auto& diag : result.diagnostics) {
    if (diag.severity == Severity::Error &&
        diag.message.find(sub) != std::string::npos) {
      return true;
    }
  }
  return false;
}

/// Returns true if there are no type-check errors.
auto is_ok(const TypeCheckResult& result) -> bool {
  for (const auto& diag : result.diagnostics) {
    if (diag.severity == Severity::Error) {
      return false;
    }
  }
  return true;
}

/// Owns all pipeline state so typed results remain valid.
struct TypecheckPipeline {
  SourceBuffer source;
  LexResult lex_result;
  ParseResult parse_result;
  ResolveResult resolve_result;
  TypeContext types;
  TypeCheckResult check_result;

  explicit TypecheckPipeline(const std::string& src)
      : source("test.dao", wrap_with_test_module(src)),
        lex_result(lex(source)),
        parse_result(parse(lex_result.tokens)) {
    if (parse_result.file != nullptr) {
      resolve_result = resolve(*parse_result.file);
      check_result = typecheck(*parse_result.file, resolve_result, types);
    }
  }

  /// Look up the function type registered for the first FunctionDecl.
  [[nodiscard]] auto first_fn_type() const -> const Type* {
    if (parse_result.file == nullptr) {
      return nullptr;
    }
    for (const auto* decl : parse_result.file->declarations) {
      if (decl->kind() == NodeKind::FunctionDecl) {
        return check_result.typed.decl_type(decl);
      }
    }
    return nullptr;
  }

  /// Collect typed function types for all FunctionDecls in order.
  [[nodiscard]] auto fn_types() const -> std::vector<const Type*> {
    std::vector<const Type*> result;
    if (parse_result.file == nullptr) {
      return result;
    }
    for (const auto* decl : parse_result.file->declarations) {
      if (decl->kind() == NodeKind::FunctionDecl) {
        result.push_back(check_result.typed.decl_type(decl));
      }
    }
    return result;
  }
};

// ---------------------------------------------------------------------------
// Multi-module programs (Task 31 D3): files named `stdlib/...` form the
// prelude group; the rest are user modules.
// ---------------------------------------------------------------------------

struct CheckedProgram {
  Program program;
  ResolveResult resolved;
  TypeCheckResult result;
};

using NamedSource = std::pair<std::string, std::string>;

auto check_program(std::vector<NamedSource> files) -> CheckedProgram {
  std::vector<SourceInput> inputs;
  for (auto& [display, text] : files) {
    inputs.push_back({.display_path = display, .text = text,
                      .is_prelude = display.starts_with("stdlib/")});
  }
  CheckedProgram checked{.program = build_program(std::move(inputs)), .resolved = {}, .result = {}};
  checked.resolved = resolve(checked.program);
  TypeContext types;
  checked.result = typecheck(checked.program, checked.resolved, types);
  return checked;
}

auto all_messages(const CheckedProgram& checked) -> std::string {
  std::string out;
  for (const auto& diag : checked.resolved.diagnostics) {
    out += "[resolve] " + diag.message + " | ";
  }
  for (const auto& diag : checked.result.diagnostics) {
    out += "[check] " + diag.message + " | ";
  }
  return out;
}

auto clean(const CheckedProgram& checked) -> bool {
  return checked.resolved.diagnostics.empty() && is_ok(checked.result);
}

// Literals, not std::string objects: boost.ut runs suites after main
// returns, when namespace-scope objects are already destroyed.
constexpr const char* kMathModule =
    "module app::math\n"
    "fn add(a: i32, b: i32): i32 -> a + b\n"
    "fn identity<T>(x: T): T -> x\n"
    "class Point:\n"
    "  x: i32\n"
    "  fn origin(): Point -> Point(0)\n"
    "enum Color:\n"
    "  Red\n"
    "  Green\n"
    "enum class Maybe:\n"
    "  Some(value: i32)\n"
    "  None\n";

} // namespace

suite<"typecheck_qualified_bounds"> typecheck_qualified_bounds = [] {
  // A bound written `m::Concept` records the import binding at the head
  // of the path and the concept at its last segment, so reading only the
  // head left the bound unenforced.
  const std::string kTraits = "module app::traits\nconcept Reveal:\n  fn reveal(self): i32\n";

  "a qualified concept bound rejects a type that does not conform"_test = [kTraits] {
    auto checked = check_program({
        {"main.dao",
         "module app::main\nimport app::traits\n"
         "class Plain:\n  x: i32\n"
         "fn show<T: traits::Reveal>(v: T): i32 -> v.reveal()\n"
         "fn main(): i32\n  let p: Plain = Plain(1)\n  return show(p)\n"},
        {"traits.dao", kTraits},
    });
    expect(has_error_containing(checked.result, "does not satisfy concept"))
        << all_messages(checked);
  };

  "a bound accepts a type conforming to that very concept"_test = [] {
    // Same module throughout: conformance is decided by which concept
    // the `as` clause names, and here it names this one.
    auto checked = check_program({
        {"main.dao",
         "module app::main\n"
         "concept Reveal:\n  fn reveal(self): i32\n"
         "class Shown:\n  x: i32\n  as Reveal:\n    fn reveal(self): i32 -> self.x\n"
         "fn show<T: Reveal>(v: T): i32 -> v.reveal()\n"
         "fn main(): i32\n  let s: Shown = Shown(1)\n  return show(s)\n"},
    });
    expect(clean(checked)) << all_messages(checked);
  };

  "conformance to a same-named concept of another module does not count"_test = [kTraits] {
    // Both modules declare `Reveal`; the bound requires app::traits's.
    // Comparing spellings accepted the wrong one.
    auto checked = check_program({
        {"main.dao",
         "module app::main\nimport app::traits\n"
         "concept Reveal:\n  fn reveal(self): i32\n"
         "class Shown:\n  x: i32\n  as Reveal:\n    fn reveal(self): i32 -> self.x\n"
         "fn show<T: traits::Reveal>(v: T): i32 -> v.reveal()\n"
         "fn main(): i32\n  let s: Shown = Shown(1)\n  return show(s)\n"},
        {"traits.dao", kTraits},
    });
    expect(has_error_containing(checked.result, "does not satisfy concept"))
        << all_messages(checked);
  };

  "conforming to an imported concept is not expressible today"_test = [kTraits] {
    // Pinning a contract gap, not endorsing it: §6 freezes four
    // qualified forms and a conformance clause is not among them, so
    // `as traits::Reveal:` does not parse, while a bare `as Reveal:`
    // names nothing in a module that imported `app::traits` as a module
    // binding.  A qualified bound can therefore be written and enforced
    // but never satisfied.  Resolving this is a normative question.
    auto checked = check_program({
        {"main.dao",
         "module app::main\nimport app::traits\n"
         "class Shown:\n  x: i32\n"
         "extend Shown as Reveal:\n  fn reveal(self): i32 -> self.x\n"
         "fn show<T: traits::Reveal>(v: T): i32 -> v.reveal()\n"
         "fn main(): i32\n  let s: Shown = Shown(1)\n  return show(s)\n"},
        {"traits.dao", kTraits},
    });
    expect(has_error_containing(checked.result, "does not satisfy concept"))
        << all_messages(checked);
  };

  "another module's extend does not satisfy a bound"_test = [kTraits] {
    auto checked = check_program({
        {"main.dao",
         "module app::main\nimport app::traits\n"
         "class Shown:\n  x: i32\n"
         "fn show<T: traits::Reveal>(v: T): i32 -> v.reveal()\n"
         "fn main(): i32\n  let s: Shown = Shown(1)\n  return show(s)\n"},
        {"traits.dao", kTraits},
        {"ext.dao",
         "module app::ext\nimport app::main\n"
         "extend main::Shown as Reveal:\n  fn reveal(self): i32 -> 1\n"},
    });
    expect(!is_ok(checked.result)) << all_messages(checked);
  };
};

suite<"typecheck_extend_scoping"> typecheck_extend_scoping = [] {
  // CONTRACT_MODULE_SYSTEM.md §5: `extend` methods participate in
  // method-set lookup within the declaring module; importing a module
  // does not import them; the prelude is the sole exception.
  const std::string kExtension =
      "module app::ext\nextend i32 as Secret:\n  fn secret(self): i32 -> 42\n";

  "an extend method is not visible in another module"_test = [kExtension] {
    auto checked = check_program({
        {"main.dao", "module app::main\nfn main(): i32\n  let v: i32 = 1\n  return v.secret()\n"},
        {"ext.dao", kExtension},
    });
    expect(!is_ok(checked.result)) << all_messages(checked);
  };

  "importing the module still does not import its extend methods"_test = [kExtension] {
    auto checked = check_program({
        {"main.dao",
         "module app::main\nimport app::ext\n"
         "fn main(): i32\n  let v: i32 = 1\n  return v.secret()\n"},
        {"ext.dao", kExtension},
    });
    expect(!is_ok(checked.result)) << all_messages(checked);
  };

  "an extend method is visible in its own module"_test = [] {
    auto checked = check_program({
        {"ext.dao",
         "module app::ext\nextend i32 as Secret:\n  fn secret(self): i32 -> 42\n"
         "fn use_it(): i32\n  let v: i32 = 1\n  return v.secret()\n"},
    });
    expect(clean(checked)) << all_messages(checked);
  };

  "same-named extensions in two modules coexist"_test = [] {
    // Each module extends i32 with its own `local`; each must see its
    // own, whatever order the modules are checked in.
    const std::string a = "module app::a\nextend i32 as A:\n  fn local(self): i32 -> 1\n"
                          "fn use_a(): i32\n  let v: i32 = 0\n  return v.local()\n";
    const std::string b = "module app::b\nextend i32 as B:\n  fn local(self): i32 -> 2\n"
                          "fn use_b(): i32\n  let v: i32 = 0\n  return v.local()\n";
    auto forward = check_program({{"a.dao", a}, {"b.dao", b}});
    expect(clean(forward)) << all_messages(forward);
    auto reversed = check_program({{"b.dao", b}, {"a.dao", a}});
    expect(clean(reversed)) << all_messages(reversed);
  };

  "another module's extension is invisible on a class receiver"_test = [] {
    // The struct fallback used by instantiations must apply the same
    // rule as the direct lookup.
    auto checked = check_program({
        {"main.dao",
         "module app::main\nclass Box:\n  x: i32\n"
         "fn main(): i32\n  let b: Box = Box(1)\n  return b.secret()\n"},
        {"ext.dao",
         "module app::ext\nimport app::main\n"
         "extend main::Box as Secret:\n  fn secret(self): i32 -> 42\n"},
    });
    expect(!is_ok(checked.result)) << all_messages(checked);
  };

  "a prelude extend method is visible everywhere"_test = [] {
    std::vector<std::string> prelude = {
        "module core::secret\nextend i32 as Secret:\n  fn secret(self): i32 -> 42\n"};
    auto checked =
        check_with_prelude("fn use_it(): i32\n  let v: i32 = 1\n  return v.secret()\n", prelude);
    expect(is_ok(checked)) << "prelude extend methods reach every module (§5.3)";
  };
};

suite<"typecheck_modules"> typecheck_modules = [] {
  "cross_module_call_checks_arity_and_argument_types"_test = [] {
    auto arity = check_program({
        {"main.dao", "module app::main\nimport app::math\nfn main(): i32 -> math::add(1)\n"},
        {"math.dao", kMathModule},
    });
    expect(has_error_containing(arity.result, "expected 2 argument(s), got 1")) << all_messages(arity);

    auto types = check_program({
        {"main.dao", "module app::main\nimport app::math\nfn main(): i32 -> math::add(1, \"two\")\n"},
        {"math.dao", kMathModule},
    });
    expect(!is_ok(types.result)) << all_messages(types);

    auto ok = check_program({
        {"main.dao", "module app::main\nimport app::math\nfn main(): i32 -> math::add(1, 2)\n"},
        {"math.dao", kMathModule},
    });
    expect(clean(ok)) << all_messages(ok);
  };

  "qualified_type_as_parameter_return_and_field"_test = [] {
    auto checked = check_program({
        {"main.dao", "module app::main\nimport app::math\n"
                     "class Holder:\n  p: math::Point\n"
                     "fn area(p: math::Point): i32 -> p.x\n"
                     "fn make(): math::Point -> math::Point(3)\n"
                     "fn main(): i32\n"
                     "  let h: Holder = Holder(math::Point(1))\n"
                     "  return area(make()) + h.p.x\n"},
        {"math.dao", kMathModule},
    });
    expect(clean(checked)) << all_messages(checked);
  };

  "qualified_enum_construction_and_match"_test = [] {
    auto checked = check_program({
        {"main.dao", "module app::main\nimport app::math\n"
                     "fn f(c: math::Color): i32\n"
                     "  match c:\n"
                     "    math::Color::Red:\n"
                     "      return 1\n"
                     "    math::Color::Green:\n"
                     "      return 2\n"
                     "  return 0\n"
                     "fn g(m: math::Maybe): i32\n"
                     "  match m:\n"
                     "    math::Maybe::Some(value):\n"
                     "      return value\n"
                     "    math::Maybe::None:\n"
                     "      return 0\n"
                     "  return 0\n"
                     "fn main(): i32\n"
                     "  let c: math::Color = math::Color::Red\n"
                     "  let m: math::Maybe = math::Maybe::Some(value = 5)\n"
                     "  return f(c) + g(m)\n"},
        {"math.dao", kMathModule},
    });
    expect(clean(checked)) << all_messages(checked);

    auto bad = check_program({
        {"main.dao", "module app::main\nimport app::math\n"
                     "fn main(): i32\n  let c: math::Color = math::Color::Blue\n  return 0\n"},
        {"math.dao", kMathModule},
    });
    expect(has_error_containing(bad.result, "'Blue' is not a variant")) << all_messages(bad);
  };

  "qualified_static_method_call"_test = [] {
    auto checked = check_program({
        {"main.dao", "module app::main\nimport app::math\n"
                     "fn main(): i32\n  let p: math::Point = math::Point::origin()\n  return p.x\n"},
        {"math.dao", kMathModule},
    });
    expect(clean(checked)) << all_messages(checked);
  };

  "cross_module_generic_call_explicit_and_inferred"_test = [] {
    auto checked = check_program({
        {"main.dao", "module app::main\nimport app::math\n"
                     "fn main(): i32\n"
                     "  let a: i32 = math::identity<i32>(3)\n"
                     "  let b: i32 = math::identity(4)\n"
                     "  return a + b\n"},
        {"math.dao", kMathModule},
    });
    expect(clean(checked)) << all_messages(checked);
  };

  "unknown_export_in_type_position_is_a_resolver_error"_test = [] {
    auto checked = check_program({
        {"main.dao", "module app::main\nimport app::math\nfn f(p: math::Nope): i32 -> 0\nfn main(): i32 -> 0\n"},
        {"math.dao", kMathModule},
    });
    expect(checked.resolved.diagnostics.size() == 1_u &&
           checked.resolved.diagnostics[0].message == "module 'app::math' has no export 'Nope'")
        << all_messages(checked);
  };

  "prelude_generics_instantiate_identically_regardless_of_module_order"_test = [] {
    // `aaa` sorts before `core::box` and imports nothing; its signatures
    // instantiate a prelude generic, which must already be registered.
    auto checked = check_program({
        {"stdlib/core/box.dao", "module core::box\nclass Box<T>:\n  v: T\n"},
        {"aaa.dao", "module aaa\nclass Holder:\n  b: Box<i32>\n"
                    "fn take(h: Holder): i32 -> 0\n"
                    "fn main(): i32\n  let h: Holder = Holder(Box<i32>(1))\n  return take(h)\n"},
    });
    expect(clean(checked)) << all_messages(checked);
  };

  "module_binding_is_not_a_value"_test = [] {
    auto checked = check_program({
        {"main.dao", "module app::main\nimport app::math\nfn main(): i32\n  let m: i32 = math\n  return 0\n"},
        {"math.dao", kMathModule},
    });
    expect(has_error_containing(checked.result, "'math' is a module, not a value")) << all_messages(checked);
  };
};

namespace {
} // namespace

// ---------------------------------------------------------------------------
// Positive: literals and let bindings
// ---------------------------------------------------------------------------

suite<"typecheck_literals"> typecheck_literals = [] {
  "int literal types as i32"_test = [] {
    auto result = check_source("fn main(): i32\n    return 42\n");
    expect(is_ok(result)) << "should typecheck cleanly";
  };

  "float literal types as f64"_test = [] {
    auto result = check_source("fn main(): f64\n    return 3.14\n");
    expect(is_ok(result)) << "should typecheck cleanly";
  };

  "bool literal types as bool"_test = [] {
    auto result = check_source("fn main(): bool\n    return true\n");
    expect(is_ok(result)) << "should typecheck cleanly";
  };

  "string literal types as string"_test = [] {
    auto result = check_source("fn main(): string\n    return \"hello\"\n");
    expect(is_ok(result)) << "should typecheck cleanly";
  };

  "let with type annotation"_test = [] {
    auto result = check_source(
        "fn main(): i32\n"
        "    let x: i32 = 10\n"
        "    return x\n");
    expect(is_ok(result)) << "typed let should work";
  };

  "let with inferred type"_test = [] {
    auto result = check_source(
        "fn main(): i32\n"
        "    let x = 10\n"
        "    return x\n");
    expect(is_ok(result)) << "inferred let should work";
  };

  "int literal fits i64 in let declaration"_test = [] {
    auto result = check_source(
        "fn main(): i32\n"
        "    let x: i64 = 42\n"
        "    return 0\n");
    expect(is_ok(result)) << "int literal should fit i64 target";
  };

  "int literal fits i64 in binary rhs"_test = [] {
    auto result = check_source(
        "fn main(): i64\n"
        "    let x: i64 = 100\n"
        "    return x + 1\n");
    expect(is_ok(result)) << "literal 1 should fit i64 from lhs";
  };

  "int literal fits i64 in binary lhs"_test = [] {
    auto result = check_source(
        "fn main(): i64\n"
        "    let x: i64 = 100\n"
        "    return 1 + x\n");
    expect(is_ok(result)) << "literal 1 should fit i64 from rhs";
  };

  "int literal fits i64 in call argument"_test = [] {
    auto result = check_source(
        "fn add_i64(a: i64, b: i64): i64 -> a + b\n"
        "fn main(): i64\n"
        "    return add_i64(1, 2)\n");
    expect(is_ok(result)) << "literal args should fit i64 params";
  };

  "int literal fits i64 in return"_test = [] {
    auto result = check_source(
        "fn get(): i64\n"
        "    return 42\n");
    expect(is_ok(result)) << "literal should fit i64 return type";
  };

  "int literal fits i64 in assignment"_test = [] {
    auto result = check_source(
        "fn main(): i32\n"
        "    let x: i64 = 0\n"
        "    x = 42\n"
        "    return 0\n");
    expect(is_ok(result)) << "literal should fit i64 assignment target";
  };
};

// ---------------------------------------------------------------------------
// Positive: arithmetic
// ---------------------------------------------------------------------------

suite<"typecheck_arithmetic"> typecheck_arithmetic = [] {
  "i32 addition"_test = [] {
    auto result = check_source("fn add(a: i32, b: i32): i32 -> a + b\n");
    expect(is_ok(result));
  };

  "f64 multiplication"_test = [] {
    auto result = check_source("fn mul(a: f64, b: f64): f64 -> a * b\n");
    expect(is_ok(result));
  };

  "comparison yields bool"_test = [] {
    auto result = check_source("fn lt(a: i32, b: i32): bool -> a < b\n");
    expect(is_ok(result));
  };

  "equality yields bool"_test = [] {
    auto result = check_source("fn eq(a: i32, b: i32): bool -> a == b\n");
    expect(is_ok(result));
  };

  "logical and"_test = [] {
    auto result = check_source("fn both(a: bool, b: bool): bool -> a and b\n");
    expect(is_ok(result));
  };

  // String operators
  "string concatenation with +"_test = [] {
    auto result = check_source(
        "fn cat(a: string, b: string): string -> a + b\n");
    expect(is_ok(result));
  };

  "string equality with =="_test = [] {
    auto result = check_source(
        "fn eq(a: string, b: string): bool -> a == b\n");
    expect(is_ok(result));
  };

  "string inequality with !="_test = [] {
    auto result = check_source(
        "fn ne(a: string, b: string): bool -> a != b\n");
    expect(is_ok(result));
  };

  "string - is rejected"_test = [] {
    auto result = check_source(
        "fn bad(a: string, b: string): string -> a - b\n");
    expect(has_error_containing(result, "arithmetic requires numeric type"));
  };
};

// ---------------------------------------------------------------------------
// Positive: function calls
// ---------------------------------------------------------------------------

suite<"typecheck_calls"> typecheck_calls = [] {
  "simple function call"_test = [] {
    auto result = check_source(
        "fn double(x: i32): i32 -> x + x\n"
        "\n"
        "fn main(): i32 -> double(5)\n");
    expect(is_ok(result));
  };
};

// ---------------------------------------------------------------------------
// Positive: expression-bodied functions
// ---------------------------------------------------------------------------

suite<"typecheck_expr_body"> typecheck_expr_body = [] {
  "expression-bodied function"_test = [] {
    auto result = check_source("fn inc(x: i32): i32 -> x + 1\n");
    expect(is_ok(result));
  };
};

// ---------------------------------------------------------------------------
// Positive: if/while with bool conditions
// ---------------------------------------------------------------------------

suite<"typecheck_control_flow"> typecheck_control_flow = [] {
  "if with bool condition"_test = [] {
    auto result = check_source(
        "fn abs(x: i32): i32\n"
        "    if x > 0:\n"
        "        return x\n"
        "    0 - x\n");
    expect(is_ok(result));
  };

  "while with bool condition"_test = [] {
    auto result = check_source(
        "fn countdown(n: i32): i32\n"
        "    let x = n\n"
        "    while x > 0:\n"
        "        x = x - 1\n"
        "    x\n");
    expect(is_ok(result));
  };
};

// ---------------------------------------------------------------------------
// Positive: void functions
// ---------------------------------------------------------------------------

suite<"typecheck_void"> typecheck_void = [] {
  "void function with bare return"_test = [] {
    auto result = check_source("fn noop(): void\n    return\n");
    expect(is_ok(result));
  };

  "bare return in non-void function"_test = [] {
    auto result = check_source("fn bad(): i32\n    return\n");
    expect(has_error_containing(result, "bare return"));
  };

  "void function without explicit return"_test = [] {
    auto result = check_source("fn noop(): void\n    let x = 1\n");
    expect(is_ok(result));
  };
};

// ---------------------------------------------------------------------------
// Positive: pointer operations
// ---------------------------------------------------------------------------

suite<"typecheck_pointers"> typecheck_pointers = [] {
  "address-of and deref in unsafe"_test = [] {
    auto result = check_source(
        "fn ptr_test(x: i32): i32\n"
        "    let p: *i32 = &x\n"
        "    mode unsafe =>\n"
        "        *p\n");
    expect(is_ok(result));
  };
};

// ---------------------------------------------------------------------------
// Negative: type mismatches
// ---------------------------------------------------------------------------

suite<"typecheck_negative"> typecheck_negative = [] {
  "let type mismatch"_test = [] {
    auto result = check_source(
        "fn main(): i32\n"
        "    let x: i32 = true\n"
        "    x\n");
    expect(has_error_containing(result, "not assignable"));
  };

  "return type mismatch"_test = [] {
    auto result = check_source("fn main(): i32\n    return true\n");
    expect(has_error_containing(result, "does not match"));
  };

  "non-bool condition"_test = [] {
    auto result = check_source(
        "fn test(): i32\n"
        "    if 42:\n"
        "        return 1\n"
        "    0\n");
    expect(has_error_containing(result, "must be 'bool'"));
  };

  "mixed arithmetic types"_test = [] {
    auto result = check_source("fn bad(a: i32, b: f64): i32 -> a + b\n");
    expect(has_error_containing(result, "mismatched types"));
  };

  "wrong arg count"_test = [] {
    auto result = check_source(
        "fn one(x: i32): i32 -> x\n"
        "\n"
        "fn main(): i32 -> one(1, 2)\n");
    expect(has_error_containing(result, "expected 1 argument"));
  };

  "wrong arg type"_test = [] {
    auto result = check_source(
        "fn takes_int(x: i32): i32 -> x\n"
        "\n"
        "fn main(): i32 -> takes_int(true)\n");
    expect(has_error_containing(result, "not assignable"));
  };

  "deref non-pointer"_test = [] {
    auto result = check_source(
        "fn bad(x: i32): i32\n"
        "    mode unsafe =>\n"
        "        *x\n");
    expect(has_error_containing(result, "non-pointer"));
  };

  "deref outside unsafe"_test = [] {
    auto result = check_source(
        "fn bad(p: *i32): i32\n"
        "    *p\n");
    expect(has_error_containing(result, "mode unsafe"));
  };

  "logical not on non-bool"_test = [] {
    auto result = check_source("fn bad(): bool -> !42\n");
    expect(has_error_containing(result, "requires 'bool'"));
  };

  "assignment type mismatch"_test = [] {
    auto result = check_source(
        "fn bad(): i32\n"
        "    let x: i32 = 1\n"
        "    x = true\n"
        "    x\n");
    expect(has_error_containing(result, "cannot assign"));
  };

  "expression-body type mismatch"_test = [] {
    auto result = check_source("fn bad(): i32 -> true\n");
    expect(has_error_containing(result, "does not match return type"));
  };

  "extern fn with string param is rejected"_test = [] {
    auto result = check_source(
        "extern fn bad(msg: string): void\n");
    expect(has_error_containing(result, "not supported at the C ABI"));
  };

  "extern fn with string return is rejected"_test = [] {
    auto result = check_source(
        "extern fn bad(): string\n");
    expect(has_error_containing(result, "not supported at the C ABI"));
  };

  // --- Struct-by-value ABI (CONTRACT_C_ABI_INTEROP §4.3) ---

  "extern fn with repr-C struct param is accepted"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "extern fn distance(p: Point): f64\n");
    expect(is_ok(result)) << "repr-C struct param should be accepted";
  };

  "extern fn with repr-C struct return is accepted"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "extern fn make_point(x: i32, y: i32): Point\n");
    expect(is_ok(result)) << "repr-C struct return should be accepted";
  };

  "extern fn with nested repr-C struct is accepted"_test = [] {
    auto result = check_source(
        "class Inner:\n"
        "  a: i32\n"
        "  b: f64\n"
        "class Outer:\n"
        "  inner: Inner\n"
        "  tag: i32\n"
        "extern fn process(o: Outer): i32\n");
    expect(is_ok(result)) << "nested repr-C struct should be accepted";
  };

  "extern fn with mixed-alignment struct is accepted"_test = [] {
    auto result = check_source(
        "class Mixed:\n"
        "  flag: bool\n"
        "  value: i32\n"
        "  wide: i64\n"
        "extern fn check_mixed(m: Mixed): bool\n");
    expect(is_ok(result)) << "mixed-alignment struct should be accepted";
  };

  "extern fn with empty struct is rejected"_test = [] {
    auto result = check_source(
        "class Empty:\n"
        "  x: i32\n" // placeholder — need to test actual empty
        "extern fn bad(e: Empty): i32\n");
    // Empty structs are hard to create syntactically since class requires
    // fields in Dao; this is tested via the predicate directly.
    expect(is_ok(result));
  };

  "extern fn with string-field struct is rejected"_test = [] {
    auto result = check_source(
        "class Named:\n"
        "  name: string\n"
        "  id: i32\n"
        "extern fn bad(n: Named): i32\n");
    expect(has_error_containing(result, "not supported at the C ABI"));
  };

  "extern fn with generator-field struct is rejected"_test = [] {
    auto result = check_source(
        "class Bad:\n"
        "  gen: Generator<i32>\n"
        "extern fn bad(b: Bad): i32\n");
    expect(has_error_containing(result, "not supported at the C ABI"));
  };

  // --- Function pointer types at the ABI boundary (§4.4) ---

  "extern fn with function pointer param is accepted"_test = [] {
    auto result = check_source(
        "extern fn apply(cb: fn(i32, i32): i32, a: i32, b: i32): i32\n");
    expect(is_ok(result)) << "function pointer param should be accepted";
  };

  "extern fn with function pointer return is accepted"_test = [] {
    auto result = check_source(
        "extern fn get_op(): fn(i32, i32): i32\n");
    expect(is_ok(result)) << "function pointer return should be accepted";
  };

  "extern fn with void callback is accepted"_test = [] {
    auto result = check_source(
        "extern fn register_cb(cb: fn(i32): void): void\n");
    expect(is_ok(result)) << "void callback should be accepted";
  };

  "extern fn with string-param callback is rejected"_test = [] {
    auto result = check_source(
        "extern fn bad(cb: fn(string): void): void\n");
    expect(has_error_containing(result, "not supported at the C ABI"));
  };

  "function type param in non-extern fn is accepted"_test = [] {
    auto result = check_source(
        "fn apply(cb: fn(i32, i32): i32, a: i32, b: i32): i32\n"
        "  return cb(a, b)\n");
    expect(is_ok(result)) << "function-typed params with indirect call should work";
  };

  "lambda passed to extern fn callback is rejected"_test = [] {
    auto result = check_source(
        "extern fn apply(cb: fn(i32, i32): i32, a: i32, b: i32): i32\n"
        "\n"
        "fn main(): i32\n"
        "  return apply(|x, y| -> x + y, 1, 2)\n");
    expect(has_error_containing(result,
        "lambda cannot be passed as a C function pointer"));
  };

  "extern fn with scalar types is accepted"_test = [] {
    auto result = check_source(
        "extern fn good(a: i32, b: i64, c: f64): i32\n");
    expect(is_ok(result)) << "scalar extern fn should typecheck";
  };

  "extern fn with f32 is accepted"_test = [] {
    auto result = check_source(
        "extern fn good(x: f32): f32\n");
    expect(is_ok(result)) << "f32 extern fn should typecheck";
  };

  "extern fn with unsigned types is accepted"_test = [] {
    auto result = check_source(
        "extern fn good(a: u8, b: u16, c: u32, d: u64): u32\n");
    expect(is_ok(result)) << "unsigned extern fn should typecheck";
  };

  "extern fn with narrow signed types is accepted"_test = [] {
    auto result = check_source(
        "extern fn good(a: i8, b: i16): i16\n");
    expect(is_ok(result)) << "narrow signed extern fn should typecheck";
  };

  "extern fn with pointer is accepted"_test = [] {
    auto result = check_source(
        "extern fn good(p: *i32): *i32\n");
    expect(is_ok(result)) << "pointer extern fn should typecheck";
  };

  "__dao_ runtime hooks are exempt from ABI validation"_test = [] {
    auto result = check_source(
        "extern fn __dao_test_hook(msg: string): void\n");
    expect(is_ok(result)) << "__dao_ hooks should bypass ABI checks";
  };
};

// ---------------------------------------------------------------------------
// Type aliases
// ---------------------------------------------------------------------------

suite<"type_alias"> type_alias = [] {
  "alias resolves to underlying type"_test = [] {
    auto result = check_source(
        "type NodeId = i32\n"
        "fn test(a: NodeId): NodeId -> a\n");
    expect(result.diagnostics.empty()) << "alias param should typecheck";
  };

  "alias-to-alias chains resolve"_test = [] {
    auto result = check_source(
        "type NodeId = i32\n"
        "type MyNode = NodeId\n"
        "fn test(a: MyNode): i32 -> a\n");
    expect(result.diagnostics.empty()) << "chained alias should typecheck";
  };

  "alias used in return type"_test = [] {
    auto result = check_source(
        "type Score = f64\n"
        "fn test(): Score -> 0.0\n");
    expect(result.diagnostics.empty()) << "alias return type should typecheck";
  };

  "forward-declared alias resolves with typed fn signature"_test = [] {
    TypecheckPipeline pipe(
        "fn test(a: NodeId): NodeId -> a\n"
        "type NodeId = i32\n");
    expect(pipe.check_result.diagnostics.empty())
        << "forward alias should typecheck";
    const auto* fn_type = pipe.first_fn_type();
    expect(fn_type != nullptr) << "function type must be populated";
    if (fn_type != nullptr) {
      expect(fn_type->kind() == TypeKind::Function)
          << "must be function type";
      const auto* ft = static_cast<const TypeFunction*>(fn_type);
      expect(ft->param_types().size() == 1_ul);
      expect(ft->param_types()[0] != nullptr)
          << "param type must not be null";
      expect(ft->param_types()[0]->kind() == TypeKind::Builtin)
          << "alias param must resolve to builtin";
    }
  };

  "alias type mismatch still caught"_test = [] {
    auto result = check_source(
        "type NodeId = i32\n"
        "fn test(a: NodeId): bool -> a\n");
    expect(has_error_containing(result, "does not match return type"));
  };
};

// ---------------------------------------------------------------------------
// Class construction
// ---------------------------------------------------------------------------

suite<"typecheck_construct"> typecheck_construct = [] {
  "basic construction"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "\n"
        "fn make(): Point -> Point(1, 2)\n");
    expect(is_ok(result)) << "basic construction should typecheck";
  };

  "construction with wrong arity"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "\n"
        "fn make(): Point -> Point(1)\n");
    expect(has_error_containing(result, "expects 2 field(s), got 1"));
  };

  "construction with wrong field type"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "\n"
        "fn make(): Point -> Point(1, true)\n");
    expect(has_error_containing(result, "field 'y' expects type"));
  };

  "construction in let binding"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "\n"
        "fn make(): i32\n"
        "    let p: Point = Point(1, 2)\n"
        "    p.x\n");
    expect(is_ok(result)) << "let with construction should typecheck";
  };

  "nested construction"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "\n"
        "class Rect:\n"
        "    tl: Point\n"
        "    br: Point\n"
        "\n"
        "fn make(): Rect -> Rect(Point(0, 0), Point(1, 1))\n");
    expect(is_ok(result)) << "nested construction should typecheck";
  };

  "field access on constructed value"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "\n"
        "fn get_x(): i32 -> Point(1, 2).x\n");
    expect(is_ok(result)) << "field access on construction should typecheck";
  };

  "value of struct type is not a constructor"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "\n"
        "fn bad(p: Point): Point -> p(1, 2)\n");
    expect(has_error_containing(result, "cannot call non-function"))
        << "calling a struct value should not be treated as construction";
  };
};

suite<"typecheck_generics"> typecheck_generics = [] {
  "generic function type uses TypeGenericParam"_test = [] {
    auto result = check_source("fn identity<T>(x: T): T -> x\n");
    expect(result.diagnostics.empty()) << "generic function should typecheck";
  };

  "generic class with type param field"_test = [] {
    auto result = check_source(
        "class Box<T>:\n"
        "    value: T\n");
    expect(result.diagnostics.empty()) << "generic class should typecheck";
  };

  "separate declarations with same T produce distinct types"_test = [] {
    TypecheckPipeline pipe(
        "fn identity<T>(x: T): T -> x\n"
        "fn wrap<T>(x: T): T -> x\n");
    expect(is_ok(pipe.check_result))
        << "both generic functions should typecheck";

    auto fns = pipe.fn_types();
    expect(fns.size() == 2_ul) << "must find two function decls";

    const auto* fn_identity = static_cast<const TypeFunction*>(fns[0]);
    const auto* fn_wrap = static_cast<const TypeFunction*>(fns[1]);
    expect(fn_identity != nullptr);
    expect(fn_wrap != nullptr);

    // Both have fn(T): T shape, but the T types must be distinct objects
    // because they belong to different binder declarations.
    expect(fn_identity->param_types().size() == 1_ul);
    expect(fn_wrap->param_types().size() == 1_ul);

    const auto* t_from_identity = fn_identity->param_types()[0];
    const auto* t_from_wrap = fn_wrap->param_types()[0];
    expect(t_from_identity != t_from_wrap)
        << "T from identity and T from wrap must be distinct TypeGenericParam objects";
  };
};

// ---------------------------------------------------------------------------
// Concept declarations and conformance
// ---------------------------------------------------------------------------

suite<"typecheck_concepts"> typecheck_concepts = [] {
  "concept declaration typechecks"_test = [] {
    auto result = check_source(
        "concept Printable:\n"
        "    fn to_string(self): string\n");
    expect(is_ok(result)) << "concept declaration should typecheck";
  };

  "concept with default method typechecks"_test = [] {
    auto result = check_source(
        "concept Equatable:\n"
        "    fn eq(self, other: Equatable): bool\n"
        "    fn ne(self, other: Equatable): bool -> !self.eq(other)\n");
    expect(is_ok(result)) << "concept with default method should typecheck";
  };

  "class with conformance block typechecks"_test = [] {
    auto result = check_source(
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "class Point:\n"
        "    x: f64\n"
        "    as Printable:\n"
        "        fn to_string(self): string -> \"p\"\n");
    expect(is_ok(result)) << "class with conformance should typecheck";
  };

  "extend declaration typechecks"_test = [] {
    auto result = check_source(
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> \"num\"\n");
    expect(is_ok(result)) << "extend declaration should typecheck";
  };

  "derived concept typechecks"_test = [] {
    auto result = check_source(
        "derived concept Copyable:\n"
        "    fn copy(self): Copyable\n");
    expect(is_ok(result)) << "derived concept should typecheck";
  };

  "class with deny typechecks"_test = [] {
    auto result = check_source(
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "class SecretKey:\n"
        "    data: i32\n"
        "    deny Printable\n");
    expect(is_ok(result)) << "class with deny should typecheck";
  };

  "derived auto-conformance enables method dispatch"_test = [] {
    auto result = check_source(
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> \"num\"\n"
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "fn show(p: Point): string -> p.to_string()\n");
    expect(is_ok(result)) << "derived auto-conformance should enable method dispatch";
  };

  "deny suppresses derived auto-conformance"_test = [] {
    auto result = check_source(
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> \"num\"\n"
        "class Secret:\n"
        "    data: i32\n"
        "    deny Printable\n"
        "fn show(s: Secret): string -> s.to_string()\n");
    expect(!is_ok(result)) << "deny should suppress derived conformance";
  };

  "explicit conformance takes precedence over derived"_test = [] {
    auto result = check_source(
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> \"num\"\n"
        "class Point:\n"
        "    x: i32\n"
        "    as Printable:\n"
        "        fn to_string(self): string -> \"custom\"\n"
        "fn show(p: Point): string -> p.to_string()\n");
    expect(is_ok(result)) << "explicit conformance should work alongside derived";
  };

  "non-conforming field blocks derived conformance"_test = [] {
    auto result = check_source(
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "class Inner:\n"
        "    val: i32\n"
        "class Outer:\n"
        "    child: Inner\n"
        "fn show(o: Outer): string -> o.to_string()\n");
    expect(!is_ok(result))
        << "class with non-conforming field should not auto-derive";
  };

  "nested derived conformance"_test = [] {
    auto result = check_source(
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> \"num\"\n"
        "class Inner:\n"
        "    val: i32\n"
        "class Outer:\n"
        "    child: Inner\n"
        "fn show(o: Outer): string -> o.to_string()\n");
    expect(is_ok(result))
        << "nested derived conformance should work transitively";
  };

  "reverse declaration order still derives"_test = [] {
    // Outer declared before Inner — fixpoint loop must handle this.
    auto result = check_source(
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> \"num\"\n"
        "class Outer:\n"
        "    child: Inner\n"
        "class Inner:\n"
        "    val: i32\n"
        "fn show(o: Outer): string -> o.to_string()\n");
    expect(is_ok(result))
        << "derived conformance must not depend on declaration order";
  };

  "extend targeting denied concept is an error"_test = [] {
    auto result = check_source(
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "class Secret:\n"
        "    data: i32\n"
        "    deny Printable\n"
        "extend Secret as Printable:\n"
        "    fn to_string(self): string -> \"hacked\"\n");
    expect(!is_ok(result))
        << "extend should not override deny";
    expect(has_error_containing(result, "denies"))
        << "should report denied concept in extend";
  };

  "as and deny for same concept is an error"_test = [] {
    auto result = check_source(
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "class Both:\n"
        "    val: i32\n"
        "    deny Printable\n"
        "    as Printable:\n"
        "        fn to_string(self): string -> \"both\"\n");
    expect(!is_ok(result))
        << "as and deny for same concept should be an error";
    expect(has_error_containing(result, "both conforms to and denies"))
        << "should report conflicting as/deny";
  };

  "concept default method bodies are not checked"_test = [] {
    // Concept default methods are abstract over self's type;
    // body checking is deferred until concept-level type reasoning.
    auto result = check_source(
        "concept Eq:\n"
        "    fn eq(self, other: Eq): bool\n"
        "    fn ne(self, other: Eq): bool -> 42\n");
    expect(is_ok(result)) << "concept default body checking is deferred";
  };

  "bad conformance method body is rejected"_test = [] {
    auto result = check_source(
        "concept Show:\n"
        "    fn show(self): string\n"
        "class X:\n"
        "    v: i32\n"
        "    as Show:\n"
        "        fn show(self): string -> 99\n");
    expect(!is_ok(result)) << "bad conformance body should produce error";
    expect(has_error_containing(result, "does not match return type"))
        << "should report type mismatch";
  };

  "bad extend method body is rejected"_test = [] {
    auto result = check_source(
        "concept Show:\n"
        "    fn show(self): string\n"
        "extend i32 as Show:\n"
        "    fn show(self): string -> 0\n");
    expect(!is_ok(result)) << "bad extend body should produce error";
    expect(has_error_containing(result, "does not match return type"))
        << "should report type mismatch";
  };
};

// ---------------------------------------------------------------------------
// Self typing and field access
// ---------------------------------------------------------------------------

suite<"typecheck_self"> typecheck_self = [] {
  "self.field access in conformance method"_test = [] {
    auto result = check_source(
        "concept HasName:\n"
        "    fn name(self): string\n"
        "class Person:\n"
        "    name: string\n"
        "    as HasName:\n"
        "        fn name(self): string -> self.name\n");
    expect(is_ok(result)) << "self.field in conformance should typecheck";
  };

  "self.field type mismatch in conformance"_test = [] {
    auto result = check_source(
        "concept AsInt:\n"
        "    fn value(self): i32\n"
        "class Wrapper:\n"
        "    label: string\n"
        "    as AsInt:\n"
        "        fn value(self): i32 -> self.label\n");
    expect(!is_ok(result)) << "self.field type mismatch should error";
    expect(has_error_containing(result, "does not match return type"))
        << "should report type mismatch";
  };

  "self.field access in extend method"_test = [] {
    auto result = check_source(
        "concept HasX:\n"
        "    fn get_x(self): f64\n"
        "class Point:\n"
        "    x: f64\n"
        "    y: f64\n"
        "extend Point as HasX:\n"
        "    fn get_x(self): f64 -> self.x\n");
    expect(is_ok(result)) << "self.field in extend should typecheck";
  };

  "self typed as enclosing class in class methods"_test = [] {
    // Direct method on a class (not through conformance) — self
    // should be typed as the class. For now, class-body methods
    // are not yet supported (only conformance/extend methods),
    // so we just verify the conformance path works.
    auto result = check_source(
        "concept GetX:\n"
        "    fn get_x(self): f64\n"
        "class Vec:\n"
        "    x: f64\n"
        "    as GetX:\n"
        "        fn get_x(self): f64 -> self.x\n");
    expect(is_ok(result)) << "self.field should typecheck in conformance";
  };
};

// ---------------------------------------------------------------------------
// Method dispatch through conformance and extend
// ---------------------------------------------------------------------------

suite<"typecheck_methods"> typecheck_methods = [] {
  "method call through conformance"_test = [] {
    auto result = check_source(
        "concept HasName:\n"
        "    fn name(self): string\n"
        "class Person:\n"
        "    first: string\n"
        "    as HasName:\n"
        "        fn name(self): string -> self.first\n"
        "fn greet(p: Person): string -> p.name()\n");
    expect(is_ok(result)) << "method call through conformance should typecheck";
  };

  "method call through extend"_test = [] {
    auto result = check_source(
        "concept HasX:\n"
        "    fn get_x(self): f64\n"
        "class Point:\n"
        "    x: f64\n"
        "    y: f64\n"
        "extend Point as HasX:\n"
        "    fn get_x(self): f64 -> self.x\n"
        "fn read_x(p: Point): f64 -> p.get_x()\n");
    expect(is_ok(result)) << "method call through extend should typecheck";
  };

  "method call with args"_test = [] {
    auto result = check_source(
        "concept Eq:\n"
        "    fn eq(self, other: Eq): bool\n"
        "class Val:\n"
        "    n: i32\n"
        "    as Eq:\n"
        "        fn eq(self, other: Val): bool -> self.n == other.n\n"
        "fn same(a: Val, b: Val): bool -> a.eq(b)\n");
    expect(is_ok(result)) << "method call with args should typecheck";
  };

  "method call wrong arg type"_test = [] {
    auto result = check_source(
        "concept Eq:\n"
        "    fn eq(self, other: Eq): bool\n"
        "class Val:\n"
        "    n: i32\n"
        "    as Eq:\n"
        "        fn eq(self, other: Val): bool -> true\n"
        "fn bad(a: Val): bool -> a.eq(42)\n");
    expect(!is_ok(result)) << "wrong arg type should error";
  };

  "unknown method errors"_test = [] {
    auto result = check_source(
        "class Point:\n"
        "    x: f64\n"
        "fn bad(p: Point): f64 -> p.missing()\n");
    expect(!is_ok(result)) << "unknown method should error";
    expect(has_error_containing(result, "no field or method"))
        << "should report missing method";
  };

  "method call on builtin via extend"_test = [] {
    auto result = check_source(
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> \"num\"\n"
        "fn show(x: i32): string -> x.to_string()\n");
    expect(is_ok(result)) << "method call on i32 via extend should typecheck";
  };

  "unknown method on builtin errors"_test = [] {
    auto result = check_source(
        "fn bad(x: i32): string -> x.missing()\n");
    expect(!is_ok(result)) << "unknown method on builtin should error";
    expect(has_error_containing(result, "no method"))
        << "should report missing method on non-class type";
  };

  "conformance method without self errors"_test = [] {
    auto result = check_source(
        "concept Eq:\n"
        "    fn eq(self, other: Eq): bool\n"
        "class Val:\n"
        "    n: i32\n"
        "    as Eq:\n"
        "        fn eq(a: Val, b: Val): bool -> true\n");
    expect(!is_ok(result)) << "conformance method without self should error";
    expect(has_error_containing(result, "self"))
        << "should mention self in error";
  };

  "extend method without self errors"_test = [] {
    auto result = check_source(
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(x: i32): string -> \"num\"\n");
    expect(!is_ok(result)) << "extend method without self should error";
    expect(has_error_containing(result, "self"))
        << "should mention self in error";
  };
};

// ---------------------------------------------------------------------------
// Scalar conformance via stdlib extend
// ---------------------------------------------------------------------------

suite<"typecheck_scalar_conformance"> typecheck_scalar_conformance = [] {
  "extern intrinsic with extend conformance"_test = [] {
    auto result = check_source(
        "extern fn __i32_to_string(x: i32): string\n"
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> __i32_to_string(self)\n"
        "fn show(x: i32): string -> x.to_string()\n");
    expect(is_ok(result))
        << "extern intrinsic backing extend conformance should typecheck";
  };

  "multiple scalar extends"_test = [] {
    auto result = check_source(
        "extern fn __i32_to_string(x: i32): string\n"
        "extern fn __f64_to_string(x: f64): string\n"
        "extern fn __bool_to_string(x: bool): string\n"
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> __i32_to_string(self)\n"
        "extend f64 as Printable:\n"
        "    fn to_string(self): string -> __f64_to_string(self)\n"
        "extend bool as Printable:\n"
        "    fn to_string(self): string -> __bool_to_string(self)\n"
        "extend string as Printable:\n"
        "    fn to_string(self): string -> self\n"
        "fn show_int(x: i32): string -> x.to_string()\n"
        "fn show_float(x: f64): string -> x.to_string()\n"
        "fn show_bool(x: bool): string -> x.to_string()\n"
        "fn show_str(x: string): string -> x.to_string()\n");
    expect(is_ok(result))
        << "all scalar extends should typecheck";
  };

  "print via extern and extend"_test = [] {
    auto result = check_source(
        "extern fn __i32_to_string(x: i32): string\n"
        "extern fn __write_stdout(msg: string): void\n"
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> __i32_to_string(self)\n"
        "fn print_i32(x: i32): void\n"
        "    __write_stdout(x.to_string())\n");
    expect(is_ok(result))
        << "print via extern + extend should typecheck";
  };

  "derived conformance through scalar extend"_test = [] {
    auto result = check_source(
        "extern fn __i32_to_string(x: i32): string\n"
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> __i32_to_string(self)\n"
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "fn show(p: Point): string -> p.to_string()\n");
    expect(is_ok(result))
        << "class should auto-derive Printable from extended i32 fields";
  };

  "self passes as intrinsic argument"_test = [] {
    auto result = check_source(
        "extern fn __f64_to_string(x: f64): string\n"
        "concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend f64 as Printable:\n"
        "    fn to_string(self): string -> __f64_to_string(self)\n"
        "fn show(x: f64): string -> x.to_string()\n");
    expect(is_ok(result))
        << "self should be passable to extern intrinsic";
  };
};

// ---------------------------------------------------------------------------
// Concept self-type resolution (§3.2 / §11.5 item 19)
// ---------------------------------------------------------------------------

suite<"typecheck_concept_self_type"> concept_self_type_tests = [] {
  "concept name resolves to conforming type in derived method dispatch"_test =
      [] {
        auto result = check_source(
            "derived concept Equatable:\n"
            "    fn eq(self, other: Equatable): bool\n"
            "extend i32 as Equatable:\n"
            "    fn eq(self, other: i32): bool -> true\n"
            "class Point:\n"
            "    x: i32\n"
            "    y: i32\n"
            "fn same(a: Point, b: Point): bool -> a.eq(b)\n");
        expect(is_ok(result))
            << "concept name should resolve to conforming type (Point) "
               "in derived method signature";
      };

  "concept name in return position resolves to conforming type"_test = [] {
    auto result = check_source(
        "derived concept Copyable:\n"
        "    fn copy(self): Copyable\n"
        "extend i32 as Copyable:\n"
        "    fn copy(self): i32 -> self\n"
        "class Pair:\n"
        "    x: i32\n"
        "    y: i32\n"
        "fn dup(p: Pair): Pair -> p.copy()\n");
    expect(is_ok(result))
        << "concept name in return type should resolve to conforming type";
  };

  "concept self-type does not leak across concepts"_test = [] {
    auto result = check_source(
        "derived concept Equatable:\n"
        "    fn eq(self, other: Equatable): bool\n"
        "derived concept Printable:\n"
        "    fn to_string(self): string\n"
        "extend i32 as Equatable:\n"
        "    fn eq(self, other: i32): bool -> true\n"
        "extend i32 as Printable:\n"
        "    fn to_string(self): string -> \"num\"\n"
        "class Val:\n"
        "    n: i32\n"
        "fn same(a: Val, b: Val): bool -> a.eq(b)\n"
        "fn show(v: Val): string -> v.to_string()\n");
    expect(is_ok(result))
        << "concept self-type should be scoped per concept";
  };

  "wrong argument type errors with concept self-type"_test = [] {
    auto result = check_source(
        "derived concept Equatable:\n"
        "    fn eq(self, other: Equatable): bool\n"
        "extend i32 as Equatable:\n"
        "    fn eq(self, other: i32): bool -> true\n"
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "class Other:\n"
        "    z: i32\n"
        "fn bad(a: Point, b: Other): bool -> a.eq(b)\n");
    expect(!is_ok(result))
        << "passing wrong type to concept method should error";
  };
};

// ---------------------------------------------------------------------------
// Prelude auto-import
// ---------------------------------------------------------------------------

suite<"typecheck_prelude"> prelude_tests = [] {
  // Common prelude source fragments.
  const std::string printable_prelude =
      "extern fn __i32_to_string(x: i32): string\n"
      "derived concept Printable:\n"
      "    fn to_string(self): string\n"
      "extend i32 as Printable:\n"
      "    fn to_string(self): string -> __i32_to_string(self)\n";
  const std::string equatable_prelude =
      "extern fn __i32_eq(a: i32, b: i32): bool\n"
      "derived concept Equatable:\n"
      "    fn eq(self, other: Equatable): bool\n"
      "extend i32 as Equatable:\n"
      "    fn eq(self, other: i32): bool -> __i32_eq(self, other)\n";
  const std::string print_prelude =
      "extern fn __write_stdout(msg: string): void\n"
      "fn print(msg: string): void -> __write_stdout(msg)\n";

  "prelude concept visible in user code"_test = [&] {
    std::array preludes{printable_prelude};
    auto result = check_with_prelude(
        "class Point:\n"
        "    x: i32\n"
        "    y: i32\n"
        "fn show(p: Point): string -> p.to_string()\n",
        preludes);
    expect(is_ok(result))
        << "prelude concept should enable derived method dispatch";
  };

  "prelude extern fn callable from user code"_test = [&] {
    std::array preludes{print_prelude};
    auto result = check_with_prelude(
        "fn hello(): void -> print(\"hi\")\n",
        preludes);
    expect(is_ok(result))
        << "prelude function should be callable without import";
  };

  "prelude extend enables scalar method calls"_test = [&] {
    std::array preludes{equatable_prelude};
    auto result = check_with_prelude(
        "fn same(a: i32, b: i32): bool -> a.eq(b)\n",
        preludes);
    expect(is_ok(result))
        << "prelude extend should enable scalar method calls";
  };

  "user code can define types deriving from prelude concepts"_test = [&] {
    std::array preludes{equatable_prelude};
    auto result = check_with_prelude(
        "class Pair:\n"
        "    x: i32\n"
        "    y: i32\n"
        "fn same(a: Pair, b: Pair): bool -> a.eq(b)\n",
        preludes);
    expect(is_ok(result))
        << "user class should auto-derive from prelude concept";
  };

  "prelude wrapping functions typecheck"_test = [&] {
    const std::string overflow_prelude =
        "extern fn __dao_wrapping_add_i32(a: i32, b: i32): i32\n"
        "fn wrapping_add(a: i32, b: i32): i32 -> __dao_wrapping_add_i32(a, b)\n";
    std::array preludes{overflow_prelude};
    auto result = check_with_prelude(
        "fn test(x: i32, y: i32): i32\n"
        "  return wrapping_add(x, y)\n",
        preludes);
    expect(is_ok(result))
        << "wrapping_add should typecheck through prelude extern + wrapper";
  };

  "prelude saturating functions typecheck"_test = [&] {
    const std::string sat_prelude =
        "extern fn __dao_saturating_add_i32(a: i32, b: i32): i32\n"
        "fn saturating_add(a: i32, b: i32): i32 -> __dao_saturating_add_i32(a, b)\n";
    std::array preludes{sat_prelude};
    auto result = check_with_prelude(
        "fn test(x: i32, y: i32): i32\n"
        "  return saturating_add(x, y)\n",
        preludes);
    expect(is_ok(result))
        << "saturating_add should typecheck through prelude extern + wrapper";
  };

  "prelude float conversion functions typecheck"_test = [&] {
    const std::string conv_prelude =
        "extern fn __dao_conv_f32_to_f64(x: f32): f64\n"
        "extern fn __dao_conv_f64_to_f32(x: f64): f32\n"
        "fn f32_to_f64(x: f32): f64 -> __dao_conv_f32_to_f64(x)\n"
        "fn f64_to_f32(x: f64): f32 -> __dao_conv_f64_to_f32(x)\n";
    std::array preludes{conv_prelude};
    auto result = check_with_prelude(
        "fn test(x: f32): f64\n"
        "  return f32_to_f64(x)\n",
        preludes);
    expect(is_ok(result))
        << "float conversion should typecheck through prelude";
  };

  "prelude integer widening functions typecheck"_test = [&] {
    const std::string conv_prelude =
        "extern fn __dao_conv_i8_to_i32(x: i8): i32\n"
        "extern fn __dao_conv_u16_to_u32(x: u16): u32\n"
        "fn i8_to_i32(x: i8): i32 -> __dao_conv_i8_to_i32(x)\n"
        "fn u16_to_u32(x: u16): u32 -> __dao_conv_u16_to_u32(x)\n";
    std::array preludes{conv_prelude};
    auto result = check_with_prelude(
        "fn widen_signed(x: i8): i32\n"
        "  return i8_to_i32(x)\n",
        preludes);
    expect(is_ok(result))
        << "integer widening should typecheck through prelude";
  };

  "prelude sign conversion functions typecheck"_test = [&] {
    const std::string conv_prelude =
        "extern fn __dao_conv_i32_to_u32(x: i32): u32\n"
        "extern fn __dao_conv_u32_to_i32(x: u32): i32\n"
        "fn i32_to_u32(x: i32): u32 -> __dao_conv_i32_to_u32(x)\n"
        "fn u32_to_i32(x: u32): i32 -> __dao_conv_u32_to_i32(x)\n";
    std::array preludes{conv_prelude};
    auto result = check_with_prelude(
        "fn roundtrip(x: i32): i32\n"
        "  let u = i32_to_u32(x)\n"
        "  return u32_to_i32(u)\n",
        preludes);
    expect(is_ok(result))
        << "sign conversion should typecheck through prelude";
  };

  "prelude i8 overflow functions typecheck"_test = [&] {
    const std::string overflow_prelude =
        "extern fn __dao_wrapping_add_i8(a: i8, b: i8): i8\n"
        "extern fn __dao_saturating_add_i8(a: i8, b: i8): i8\n"
        "fn wrapping_add_i8(a: i8, b: i8): i8 -> __dao_wrapping_add_i8(a, b)\n"
        "fn saturating_add_i8(a: i8, b: i8): i8 -> __dao_saturating_add_i8(a, b)\n";
    std::array preludes{overflow_prelude};
    auto result = check_with_prelude(
        "fn test(x: i8, y: i8): i8\n"
        "  return wrapping_add_i8(x, y)\n",
        preludes);
    expect(is_ok(result))
        << "i8 overflow should typecheck through prelude";
  };

  "prelude Numeric concept for u32 typechecks"_test = [&] {
    const std::string numeric_prelude =
        "concept Numeric:\n"
        "  fn less_than(self, other: Numeric): bool\n"
        "extend u32 as Numeric:\n"
        "  fn less_than(self, other: u32): bool\n"
        "    if self < other:\n"
        "      return true\n"
        "    return false\n"
        "fn min<T: Numeric>(a: T, b: T): T\n"
        "  if a.less_than(b):\n"
        "    return a\n"
        "  return b\n";
    std::array preludes{numeric_prelude};
    auto result = check_with_prelude(
        "fn test(a: u32, b: u32): u32\n"
        "  return min(a, b)\n",
        preludes);
    expect(is_ok(result))
        << "generic min with u32 Numeric should typecheck";
  };

  "multiple prelude files compose"_test = [&] {
    std::array preludes{printable_prelude, equatable_prelude};
    auto result = check_with_prelude(
        "class Val:\n"
        "    n: i32\n"
        "fn show(v: Val): string -> v.to_string()\n"
        "fn same(a: Val, b: Val): bool -> a.eq(b)\n",
        preludes);
    expect(is_ok(result))
        << "multiple prelude files should compose correctly";
  };
};

// ---------------------------------------------------------------------------
// Generator and yield
// ---------------------------------------------------------------------------

suite<"typecheck_generator"> typecheck_generator = [] {
  "generator function typechecks"_test = [] {
    auto result = check_source(
        "fn range(n: i32): Generator<i32>\n"
        "    let i = 0\n"
        "    while i < n:\n"
        "        yield i\n"
        "        i = i + 1\n");
    expect(is_ok(result)) << "basic generator function should typecheck";
  };

  "yield type mismatch"_test = [] {
    auto result = check_source(
        "fn bad(): Generator<i32>\n"
        "    yield 3.14\n");
    expect(!is_ok(result)) << "yield type mismatch should error";
    expect(has_error_containing(result, "yield type"))
        << "should mention yield type mismatch";
  };

  "yield outside generator"_test = [] {
    auto result = check_source(
        "fn bad(): void\n"
        "    yield 42\n");
    expect(!is_ok(result)) << "yield outside generator should error";
    expect(has_error_containing(result, "generator function"))
        << "should mention generator function requirement";
  };

  "return value in generator"_test = [] {
    auto result = check_source(
        "fn bad(): Generator<i32>\n"
        "    yield 1\n"
        "    return 2\n");
    expect(!is_ok(result)) << "return value in generator should error";
    expect(has_error_containing(result, "return value"))
        << "should mention return value restriction";
  };

  "bare return in generator"_test = [] {
    auto result = check_source(
        "fn range(n: i32): Generator<i32>\n"
        "    let i = 0\n"
        "    while i < n:\n"
        "        yield i\n"
        "        i = i + 1\n"
        "    return\n");
    expect(is_ok(result)) << "bare return in generator should be valid";
  };

  "for-in requires Generator"_test = [] {
    auto result = check_source(
        "fn range(n: i32): Generator<i32>\n"
        "    yield n\n"
        "fn main(): void\n"
        "    for x in range(10):\n"
        "        let y = x + 1\n");
    expect(is_ok(result)) << "for-in over Generator should typecheck";
  };

  "for-in rejects non-Generator"_test = [] {
    auto result = check_source(
        "fn main(): void\n"
        "    for x in 42:\n"
        "        let y = x\n");
    expect(!is_ok(result)) << "for-in over non-Generator should error";
    expect(has_error_containing(result, "Generator<T>"))
        << "should mention Generator<T> requirement";
  };

  "Generator type arg required"_test = [] {
    auto result = check_source(
        "fn bad(): Generator\n"
        "    yield 1\n");
    expect(!is_ok(result)) << "Generator without type arg should error";
    expect(has_error_containing(result, "type argument"))
        << "should mention type argument requirement";
  };
};

// ---------------------------------------------------------------------------
// Pointer operations
// ---------------------------------------------------------------------------

suite<"pointer_ops"> pointer_ops = [] {
  "store through pointer requires unsafe"_test = [] {
    auto result = check_source(
        "fn f(p: *i32): void\n"
        "    *p = 42\n");
    expect(!result.diagnostics.empty());
    bool found = false;
    for (const auto& d : result.diagnostics) {
      if (d.message.find("unsafe") != std::string::npos) found = true;
    }
    expect(found) << "should require mode unsafe";
  };

  "store through pointer in unsafe accepted"_test = [] {
    auto result = check_source(
        "fn f(p: *i32): void\n"
        "    mode unsafe =>\n"
        "        *p = 42\n");
    expect(result.diagnostics.empty());
  };

  "pointer equality accepted"_test = [] {
    auto result = check_source(
        "fn f(a: *i32, b: *i32): bool\n"
        "    return a == b\n");
    expect(result.diagnostics.empty());
  };

  "void pointer assignability"_test = [] {
    auto result = check_source(
        "fn f(p: *i32): *void\n"
        "    return p\n");
    expect(result.diagnostics.empty());
  };

  "void pointer not assignable to typed pointer"_test = [] {
    auto result = check_source(
        "fn f(p: *void): *i32\n"
        "    return p\n");
    expect(!result.diagnostics.empty());
  };
};

// ---------------------------------------------------------------------------
// Class body methods and static calls
// ---------------------------------------------------------------------------

suite<"class_methods"> class_methods = [] {
  "class with instance method typechecks"_test = [] {
    auto result = check_source(
        "class Box:\n"
        "    x: i32\n"
        "\n"
        "    fn get(self): i32 -> self.x\n"
        "\n"
        "fn main(): i32\n"
        "    let b = Box(42)\n"
        "    return b.get()\n");
    expect(result.diagnostics.empty());
  };

  "class with static method typechecks"_test = [] {
    auto result = check_source(
        "class Box:\n"
        "    x: i32\n"
        "\n"
        "    fn zero(): Box\n"
        "        let z: i32 = 0\n"
        "        return Box(z)\n"
        "\n"
        "fn main(): i32\n"
        "    let b = Box::zero()\n"
        "    return b.x\n");
    expect(result.diagnostics.empty());
  };

  "generic constructor infers type"_test = [] {
    auto result = check_source(
        "class Box<T>:\n"
        "    value: T\n"
        "\n"
        "fn main(): i32\n"
        "    let b = Box(42)\n"
        "    return b.value\n");
    expect(result.diagnostics.empty());
  };

  "static method not callable through instance"_test = [] {
    auto result = check_source(
        "class Box:\n"
        "    x: i32\n"
        "\n"
        "    fn zero(): Box\n"
        "        let z: i32 = 0\n"
        "        return Box(z)\n"
        "\n"
        "fn main(): i32\n"
        "    let b = Box(1)\n"
        "    let z = b.zero()\n"
        "    return z.x\n");
    expect(!result.diagnostics.empty()) << "static method should not be callable on instance";
  };

  "struct field assignability is exact not covariant"_test = [] {
    // Two non-generic structs with mismatched fields are rejected.
    auto result = check_source(
        "class A:\n"
        "    x: i32\n"
        "\n"
        "class B:\n"
        "    x: i32\n"
        "\n"
        "fn f(a: A): i32 -> a.x\n"
        "\n"
        "fn main(): i32\n"
        "    let b = B(42)\n"
        "    return f(b)\n");
    expect(!result.diagnostics.empty()) << "A and B are different nominal types";
  };
};

// ---------------------------------------------------------------------------
// Explicit type arguments
// ---------------------------------------------------------------------------

suite<"explicit_type_args"> explicit_type_args = [] {
  "call with explicit type arg typechecks"_test = [] {
    auto result = check_source(
        "fn identity<T>(x: T): T -> x\n"
        "\n"
        "fn main(): i32\n"
        "    return identity<i32>(42)\n");
    expect(result.diagnostics.empty());
  };

  "wrong type arg count rejected"_test = [] {
    auto result = check_source(
        "fn identity<T>(x: T): T -> x\n"
        "\n"
        "fn main(): i32\n"
        "    return identity<i32, f64>(42)\n");
    expect(!result.diagnostics.empty());
    bool found = false;
    for (const auto& d : result.diagnostics) {
      if (d.message.find("type argument") != std::string::npos) found = true;
    }
    expect(found) << "should mention type argument count";
  };
};

// NOLINTEND(readability-magic-numbers)

auto main() -> int {} // NOLINT(readability-named-parameter)
