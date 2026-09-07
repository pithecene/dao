#include "backend/llvm/llvm_backend.h"
#include "backend/llvm/llvm_names.h"
#include "backend/llvm/llvm_runtime_hooks.h"
#include "backend/llvm/llvm_type_lowering.h"
#include "frontend/diagnostics/source.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"
#include "ir/hir/hir_builder.h"
#include "ir/hir/hir_context.h"
#include "ir/mir/mir_builder.h"
#include "ir/mir/mir_context.h"
#include "ir/mir/mir_monomorphize.h"
#include "support/test_utils.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <boost/ut.hpp>
#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <optional>

using namespace boost::ut;
using namespace dao;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

// Sentinel declaration identity for testing.
// Never dereferenced — serves only as a distinct pointer value.
// NOLINTBEGIN(performance-no-int-to-ptr)
const auto* kDeclSentinel = reinterpret_cast<const Decl*>(uintptr_t{1});
// NOLINTEND(performance-no-int-to-ptr)

/// Full pipeline: source → MIR → LLVM IR.  Optional prelude sources are
/// separate prelude-group files of the program, as the driver builds it.
struct LlvmTestPipeline {
  Program program;
  ResolveResult resolve_result;
  TypeContext types;
  TypeCheckResult check_result;
  HirContext hir_ctx;
  HirBuildResult hir_result;
  MirContext mir_ctx;
  MirBuildResult mir_result;
  MonomorphizeResult mono_result;
  llvm::LLVMContext llvm_ctx;
  LlvmBackendResult llvm_result;

  explicit LlvmTestPipeline(const std::string& src,
                            std::vector<std::string> prelude_sources = {})
      : program(make_test_program(src, prelude_sources)) {
    if (!program.lexed_and_parsed_cleanly()) {
      return;
    }
    resolve_result = resolve(program);
    check_result = typecheck(program, resolve_result, types);
    hir_result = build_hir(program, resolve_result, check_result, hir_ctx);
    if (hir_result.program != nullptr) {
      mir_result = build_mir(*hir_result.program, mir_ctx, types);
      if (mir_result.module != nullptr) {
        // The driver monomorphizes between MIR and the backend, so the
        // backend never sees a generic template; a helper that skipped
        // it would test a pipeline no build runs.
        mono_result =
            monomorphize(*mir_result.module, mir_ctx, types, mir_result.generic_templates);
        LlvmBackend backend(llvm_ctx);
        llvm_result = backend.lower(*mir_result.module, &program.source_map, program.entry);
      }
    }
  }

  [[nodiscard]] auto ir() const -> std::string {
    std::ostringstream out;
    if (llvm_result.module != nullptr) {
      LlvmBackend::print_ir(out, *llvm_result.module);
    }
    return out.str();
  }

  [[nodiscard]] auto has_errors() const -> bool {
    return std::any_of(llvm_result.diagnostics.begin(),
                       llvm_result.diagnostics.end(),
                       [](const Diagnostic& diag) {
                         return diag.severity == Severity::Error;
                       });
  }
};

auto contains(const std::string& haystack, std::string_view needle) -> bool {
  return haystack.find(needle) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// Type lowering
// ---------------------------------------------------------------------------


/// The same pipeline over an explicit multi-module program: files named
/// `stdlib/...` form the prelude group, the rest are user modules, and
/// the entry is `entry` or the unique `fn main`.
struct LlvmProgramPipeline {
  Program program;
  ResolveResult resolve_result;
  TypeContext types;
  TypeCheckResult check_result;
  HirContext hir_ctx;
  HirBuildResult hir_result;
  MirContext mir_ctx;
  MirBuildResult mir_result;
  MonomorphizeResult mono_result;
  llvm::LLVMContext llvm_ctx;
  LlvmBackendResult llvm_result;

  explicit LlvmProgramPipeline(std::vector<std::pair<std::string, std::string>> files,
                               std::optional<std::string> entry = {}) {
    std::vector<SourceInput> inputs;
    for (auto& [display, text] : files) {
      inputs.push_back({.display_path = display, .text = text,
                        .is_prelude = display.starts_with("stdlib/")});
    }
    program = build_program(std::move(inputs), std::move(entry));
    if (!program.lexed_and_parsed_cleanly() || !program.diagnostics.empty()) {
      return;
    }
    resolve_result = resolve(program);
    check_result = typecheck(program, resolve_result, types);
    hir_result = build_hir(program, resolve_result, check_result, hir_ctx);
    if (hir_result.program != nullptr) {
      mir_result = build_mir(*hir_result.program, mir_ctx, types);
      if (mir_result.module != nullptr) {
        // The driver monomorphizes between MIR and the backend, so the
        // backend never sees a generic template; a helper that skipped
        // it would test a pipeline no build runs.
        mono_result =
            monomorphize(*mir_result.module, mir_ctx, types, mir_result.generic_templates);
        LlvmBackend backend(llvm_ctx);
        llvm_result = backend.lower(*mir_result.module, &program.source_map, program.entry);
      }
    }
  }

  [[nodiscard]] auto ir() const -> std::string {
    std::ostringstream out;
    if (llvm_result.module != nullptr) {
      LlvmBackend::print_ir(out, *llvm_result.module);
    }
    return out.str();
  }

  [[nodiscard]] auto function(const std::string& name) const -> llvm::Function* {
    return llvm_result.module == nullptr ? nullptr : llvm_result.module->getFunction(name);
  }

  /// Every stage's diagnostics, for failure messages.
  [[nodiscard]] auto problems() const -> std::string {
    std::string out;
    auto add = [&](const char* stage, const std::vector<Diagnostic>& diags) {
      for (const auto& diag : diags) {
        out += std::string("[") + stage + "] " + diag.message + " | ";
      }
    };
    add("program", program.diagnostics);
    for (const auto& file : program.files) {
      add("lex", file->lex.diagnostics);
      add("parse", file->parse.diagnostics);
    }
    add("resolve", resolve_result.diagnostics);
    add("check", check_result.diagnostics);
    add("hir", hir_result.diagnostics);
    add("mir", mir_result.diagnostics);
    add("mono", mono_result.diagnostics);
    add("llvm", llvm_result.diagnostics);
    return out;
  }
};


suite<"module_naming"> module_naming = [] {
  "names_follow_symbol_identity"_test = [] {
    ModuleInfo lib{.display = "app::lib", .is_prelude = false};
    ModuleInfo entry{.display = "app::main", .is_prelude = false};
    ModuleInfo prelude{.display = "core::x", .is_prelude = true};
    Symbol plain{.kind = SymbolKind::Function, .name = "f", .decl_span = {}, .decl = nullptr, .module = &lib};
    Symbol lib_main{.kind = SymbolKind::Function, .name = "main", .decl_span = {}, .decl = nullptr, .module = &lib};
    Symbol entry_main{.kind = SymbolKind::Function, .name = "main", .decl_span = {}, .decl = nullptr, .module = &entry};
    Symbol builtin{.kind = SymbolKind::Function, .name = "null_ptr", .decl_span = {}, .decl = nullptr, .module = nullptr};
    Symbol method{.kind = SymbolKind::Function, .name = "Vec.push$i32", .decl_span = {}, .decl = nullptr, .module = &prelude};
    expect(llvm_function_name(plain, &entry) == "app::lib::f");
    expect(llvm_function_name(lib_main, &entry) == "app::lib::main") << "main elsewhere is not main";
    expect(llvm_function_name(entry_main, &entry) == "main");
    expect(llvm_function_name(builtin, &entry) == "null_ptr") << "no owning module: as written";
    expect(llvm_function_name(method, &entry) == "core::x::Vec.push$i32") << "method and instantiation mangling kept";
  };

  "intrinsics_are_recognised_by_identity"_test = [] {
    ModuleInfo user{.display = "app::main", .is_prelude = false};
    ModuleInfo prelude{.display = "core::builtins", .is_prelude = true};
    Symbol user_size_of{.kind = SymbolKind::Function, .name = "size_of", .decl_span = {}, .decl = nullptr, .module = &user};
    Symbol prelude_size_of{.kind = SymbolKind::Function, .name = "size_of$i32", .decl_span = {}, .decl = nullptr, .module = &prelude};
    Symbol builtin{.kind = SymbolKind::Function, .name = "ptr_cast", .decl_span = {}, .decl = nullptr, .module = nullptr};
    Symbol prelude_other{.kind = SymbolKind::Function, .name = "size_offset", .decl_span = {}, .decl = nullptr, .module = &prelude};
    expect(!is_builtin_intrinsic(user_size_of)) << "a user module's size_of is an ordinary function";
    expect(is_builtin_intrinsic(prelude_size_of));
    expect(is_builtin_intrinsic(builtin));
    expect(!is_builtin_intrinsic(prelude_other)) << "prefix alone is not a match";
  };

  "two_modules_may_declare_the_same_function"_test = [] {
    LlvmProgramPipeline pipe({
        {"x.dao", "module a::x\nfn add(p: i32, q: i32): i32 -> p + q\n"},
        {"y.dao", "module a::y\nfn add(p: i32, q: i32): i32 -> p * q\n"},
        {"main.dao", "module a::main\nimport a::x\nimport a::y\nfn main(): i32 -> x::add(2, 3) + y::add(2, 3)\n"},
    });
    expect(pipe.llvm_result.module != nullptr) << "lowering failed: " << pipe.problems();
    expect(pipe.function("a::x::add") != nullptr && pipe.function("a::y::add") != nullptr)
        << "both add functions lowered under module-qualified names";
    expect(pipe.function("main") != nullptr);
    auto ir = pipe.ir();
    expect(ir.find("call i32 @\"a::x::add\"") != std::string::npos &&
           ir.find("call i32 @\"a::y::add\"") != std::string::npos)
        << "main calls each module's add";
  };

  "main_outside_the_entry_module_is_an_ordinary_function"_test = [] {
    LlvmProgramPipeline pipe({
        {"lib.dao", "module a::lib\nfn main(): i32 -> 7\n"},
        {"app.dao", "module a::app\nimport a::lib\nfn main(): i32 -> lib::main()\n"},
    }, "a::app");
    expect(pipe.llvm_result.module != nullptr) << "lowering failed: " << pipe.problems();
    expect(pipe.function("main") != nullptr);
    expect(pipe.function("a::lib::main") != nullptr) << "the other main is module-qualified";
    expect(pipe.ir().find("call i32 @\"a::lib::main\"") != std::string::npos);
  };
};

suite<"type_lowering"> type_lowering = [] {
  "builtin scalars lower correctly"_test = [] {
    llvm::LLVMContext ctx;
    LlvmTypeLowering lowering(ctx);
    TypeContext types;

    expect(lowering.lower(types.i32()) != nullptr);
    expect(lowering.lower(types.i64()) != nullptr);
    expect(lowering.lower(types.f32()) != nullptr);
    expect(lowering.lower(types.f64()) != nullptr);
    expect(lowering.lower(types.bool_type()) != nullptr);
    expect(lowering.lower(types.u8()) != nullptr);
    expect(lowering.lower(types.u64()) != nullptr);
  };

  "void type lowers"_test = [] {
    llvm::LLVMContext ctx;
    LlvmTypeLowering lowering(ctx);
    TypeContext types;

    auto* lowered = lowering.lower(types.void_type());
    expect(lowered != nullptr);
    expect(lowered->isVoidTy());
  };

  "pointer type lowers"_test = [] {
    llvm::LLVMContext ctx;
    LlvmTypeLowering lowering(ctx);
    TypeContext types;

    auto* lowered = lowering.lower(types.pointer_to(types.i32()));
    expect(lowered != nullptr);
    expect(lowered->isPointerTy());
  };

  "string type lowers to struct"_test = [] {
    llvm::LLVMContext ctx;
    LlvmTypeLowering lowering(ctx);
    TypeContext types;

    auto* str = types.named_type(nullptr, "string", {});
    auto* lowered = lowering.lower(str);
    expect(lowered != nullptr);
    expect(lowered->isStructTy());
  };

  "null type returns error"_test = [] {
    llvm::LLVMContext ctx;
    LlvmTypeLowering lowering(ctx);

    expect(lowering.lower(nullptr) == nullptr);
    expect(!lowering.error().empty());
  };

  "generic param fails"_test = [] {
    llvm::LLVMContext ctx;
    LlvmTypeLowering lowering(ctx);
    TypeContext types;

    auto* gp = types.generic_param(kDeclSentinel, "T", 0);
    expect(lowering.lower(gp) == nullptr);
    expect(contains(lowering.error(), "generic"));
  };
};

// ---------------------------------------------------------------------------
// Simple function lowering
// ---------------------------------------------------------------------------

suite<"simple_functions"> simple_functions = [] {
  "empty function with i32 return"_test = [] {
    LlvmTestPipeline pipe(
        "fn answer(): i32\n"
        "  return 42\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "define i32 @\"test::answer\"()")) << ir;
    expect(contains(ir, "ret i32 42")) << ir;
  };

  "fieldless variant of a payload-bearing enum passes inline as an argument"_test = [] {
    // The value is a tagged struct; spelled as a bare tag it tripped LLVM's
    // aggregate-initializer check when folded into a constant construct.
    LlvmTestPipeline pipe("enum class K:\n"
                          "  A(v: i64)\n"
                          "  B\n"
                          "class Inst:\n"
                          "  kind: K\n"
                          "  name: string\n"
                          "fn make(): Inst\n"
                          "  return Inst(K.B, \"\")\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << ir;
    expect(contains(ir, "define")) << ir;
  };

  "match on a payload-bearing enum with a fieldless arm lowers"_test = [] {
    LlvmTestPipeline pipe("enum class K:\n"
                          "  A(v: i64)\n"
                          "  B\n"
                          "fn pick(k: K): i64\n"
                          "  match k:\n"
                          "    K.A(v):\n"
                          "      return v\n"
                          "    K.B:\n"
                          "      return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << ir;
    expect(contains(ir, "icmp eq")) << ir;
  };

  "one generic serves same-named types from two modules"_test = [] {
    // Distinct declarations are distinct types (§11), so their
    // specializations cannot share a symbol: one definition would then
    // be called with the other's struct type.
    LlvmProgramPipeline pipe({
        {"lib.dao", "module app::lib\nfn ident<T>(v: T): T -> v\n"},
        {"a.dao",
         "module app::a\n"
         "class Box:\n  value: i32\n"},
        {"main.dao",
         "module app::main\n"
         "import app::lib\n"
         "import app::a\n"
         "class Box:\n  tag: i32\n"
         "fn main(): i32\n"
         "  let mine = lib::ident(Box(2))\n"
         "  let theirs = lib::ident(a::Box(3))\n"
         "  return mine.tag + theirs.value\n"},
    });
    auto ir = pipe.ir();
    size_t definitions = 0;
    for (size_t at = ir.find("define"); at != std::string::npos; at = ir.find("define", at + 1)) {
      auto line = ir.substr(at, ir.find('\n', at) - at);
      if (line.find("ident$") != std::string::npos) {
        ++definitions;
      }
    }
    expect(definitions == 2_ul) << "expected two specializations, got " << definitions << "\n"
                                << pipe.problems() << ir;
  };

  "externs differing only in pointee type are diagnosed"_test = [] {
    // `*i32` and `*f64` both lower to LLVM's opaque `ptr`, so comparing
    // lowered types calls these compatible and lets one declaration
    // answer for the other; the source signatures are what decide
    // (CONTRACT_C_ABI_INTEROP.md §5).  It takes two modules, since one
    // module cannot declare the same name twice.
    LlvmProgramPipeline pipe({
        {"a.dao",
         "module app::a\n"
         "extern fn take(p: *i32): i32\n"
         "fn use_a(p: *i32): i32 -> take(p)\n"},
        {"main.dao",
         "module app::main\n"
         "extern fn take(p: *f64): i32\n"
         "fn use_main(p: *f64): i32 -> take(p)\n"
         "fn main(): i32\n"
         "  return 0\n"},
    });
    expect(contains(pipe.problems(), "conflicting signatures"))
        << "conflicting extern accepted: " << pipe.problems();
  };

  "two modules declaring one extern emit one declaration"_test = [] {
    // An extern keeps its C symbol name exactly as written, so the same
    // extern in two modules is the same symbol — declared once.
    LlvmTestPipeline pipe("extern fn puts(s: string): i32\n"
                          "fn a(): i32 -> puts(\"x\")\n"
                          "fn b(): i32 -> puts(\"y\")\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << ir;
    size_t declarations = 0;
    for (size_t at = ir.find("declare"); at != std::string::npos; at = ir.find("declare", at + 1)) {
      auto line = ir.substr(at, ir.find('\n', at) - at);
      if (line.find("@puts(") != std::string::npos) {
        ++declarations;
      }
    }
    expect(declarations == 1_ul) << "expected one @puts declaration, got " << declarations << "\n"
                                 << ir;
    expect(ir.find("@puts.1") == std::string::npos) << "a renamed duplicate:\n" << ir;
  };

  "void function"_test = [] {
    LlvmTestPipeline pipe(
        "fn noop(): void\n"
        "  return\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "define void @\"test::noop\"()")) << ir;
    expect(contains(ir, "ret void")) << ir;
  };

  "function with params"_test = [] {
    LlvmTestPipeline pipe(
        "fn add(a: i32, b: i32): i32\n"
        "  return a + b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "define i32 @\"test::add\"(i32 %a, i32 %b)")) << ir;
    expect(contains(ir, "add")) << ir;
    expect(contains(ir, "ret i32")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Arithmetic and comparisons
// ---------------------------------------------------------------------------

suite<"arithmetic"> arithmetic = [] {
  "signed integer arithmetic uses checked intrinsics"_test = [] {
    LlvmTestPipeline pipe(
        "fn calc(x: i32, y: i32): i32\n"
        "  return x * y + x - y\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // Signed ops use overflow-checking intrinsics.
    expect(contains(ir, "llvm.smul.with.overflow.i32")) << ir;
    expect(contains(ir, "llvm.sadd.with.overflow.i32")) << ir;
    expect(contains(ir, "llvm.ssub.with.overflow.i32")) << ir;
    // Overflow branches to trap.
    expect(contains(ir, "overflow.trap")) << ir;
    expect(contains(ir, "llvm.trap")) << ir;
  };

  "float arithmetic"_test = [] {
    LlvmTestPipeline pipe(
        "fn calc(x: f64, y: f64): f64\n"
        "  return x + y\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "fadd")) << ir;
  };

  "comparison produces i1"_test = [] {
    LlvmTestPipeline pipe(
        "fn lt(a: i32, b: i32): bool\n"
        "  return a < b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "icmp slt")) << ir;
  };

  "float == uses ordered equal (oeq)"_test = [] {
    LlvmTestPipeline pipe(
        "fn feq(a: f64, b: f64): bool\n"
        "  return a == b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "fcmp oeq")) << ir;
  };

  "float != uses unordered not-equal (une)"_test = [] {
    LlvmTestPipeline pipe(
        "fn fne(a: f64, b: f64): bool\n"
        "  return a != b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // UNE: true if either operand is NaN OR operands are not equal.
    // ONE would incorrectly return false for NaN comparisons.
    expect(contains(ir, "fcmp une")) << ir;
  };

  "float < uses ordered less-than (olt)"_test = [] {
    LlvmTestPipeline pipe(
        "fn flt(a: f64, b: f64): bool\n"
        "  return a < b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "fcmp olt")) << ir;
  };

  "float >= uses ordered greater-or-equal (oge)"_test = [] {
    LlvmTestPipeline pipe(
        "fn fge(a: f64, b: f64): bool\n"
        "  return a >= b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "fcmp oge")) << ir;
  };

  // f32 shares the same IEEE 754 predicate selection as f64.
  "f32 == uses ordered equal (oeq)"_test = [] {
    LlvmTestPipeline pipe(
        "fn feq(a: f32, b: f32): bool\n"
        "  return a == b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "fcmp oeq")) << ir;
  };

  "f32 != uses unordered not-equal (une)"_test = [] {
    LlvmTestPipeline pipe(
        "fn fne(a: f32, b: f32): bool\n"
        "  return a != b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "fcmp une")) << ir;
  };

  "f32 < uses ordered less-than (olt)"_test = [] {
    LlvmTestPipeline pipe(
        "fn flt(a: f32, b: f32): bool\n"
        "  return a < b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "fcmp olt")) << ir;
  };

  // String operators dispatch to runtime hooks.
  "string == calls __dao_eq_string"_test = [] {
    LlvmTestPipeline pipe(
        "fn eq(a: string, b: string): bool\n"
        "  return a == b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "call i1 @__dao_eq_string(")) << ir;
  };

  "string + calls __dao_str_concat"_test = [] {
    LlvmTestPipeline pipe(
        "fn cat(a: string, b: string): string\n"
        "  return a + b\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "call %dao.string @__dao_str_concat(")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Control flow
// ---------------------------------------------------------------------------

suite<"control_flow"> control_flow = [] {
  "if-else produces conditional branch"_test = [] {
    LlvmTestPipeline pipe(
        "fn abs(x: i32): i32\n"
        "  if x < 0:\n"
        "    return 0 - x\n"
        "  else:\n"
        "    return x\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "br i1")) << ir;
    expect(contains(ir, "ret i32")) << ir;
  };

  "while loop"_test = [] {
    LlvmTestPipeline pipe(
        "fn countdown(n: i32): i32\n"
        "  let x: i32 = n\n"
        "  while x > 0:\n"
        "    x = x - 1\n"
        "  return x\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "br i1")) << ir;
    expect(contains(ir, "icmp sgt")) << ir;
  };
};

// ---------------------------------------------------------------------------
// String literals
// ---------------------------------------------------------------------------

suite<"strings"> strings = [] {
  "string constant creates global"_test = [] {
    LlvmTestPipeline pipe(
        "fn print(msg: string): void\n"
        "  return\n"
        "\n"
        "fn main(): i32\n"
        "  print(\"hello\")\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "@.str")) << ir;
    expect(contains(ir, "hello")) << ir;
    expect(contains(ir, "dao.string")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Function calls
// ---------------------------------------------------------------------------

suite<"calls"> calls = [] {
  "direct function call"_test = [] {
    LlvmTestPipeline pipe(
        "fn double(x: i32): i32\n"
        "  return x + x\n"
        "\n"
        "fn main(): i32\n"
        "  return double(21)\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "call i32 @\"test::double\"")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Extern declarations (no body)
// ---------------------------------------------------------------------------

suite<"externs"> externs = [] {
  "extern declaration produces declare"_test = [] {
    LlvmTestPipeline pipe(
        "extern fn print(msg: string): void\n"
        "\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "declare void @print")) << ir; // extern: as written
  };

  "extern fn with struct param uses ABI-coerced types"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "extern fn distance(p: Point): f64\n"
        "\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // Point { i32, i32 } = 8 bytes → coerced to single i64 param.
    expect(contains(ir, "declare double @distance(i64)")) << ir;
  };

  "extern fn returning struct uses ABI-coerced return"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "extern fn make_point(x: i32, y: i32): Point\n"
        "\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // Point { i32, i32 } = 8 bytes → coerced to i64 return.
    expect(contains(ir, "declare i64 @make_point(i32, i32)")) << ir;
  };

  "extern fn with large struct uses byval"_test = [] {
    LlvmTestPipeline pipe(
        "class Inner:\n"
        "  a: i32\n"
        "  b: f64\n"
        "\n"
        "class Outer:\n"
        "  inner: Inner\n"
        "  tag: i32\n"
        "\n"
        "extern fn process(o: Outer): i32\n"
        "\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // Outer { Inner { i32, f64 }, i32 } = 24 bytes → indirect (byval).
    expect(contains(ir, "declare i32 @process(ptr byval(%dao.Outer))")) << ir;
  };

  // SSE classification: pure-float structs use XMM registers.
  "extern fn with f64 struct uses SSE coercion"_test = [] {
    LlvmTestPipeline pipe(
        "class F64Wrap:\n"
        "  val: f64\n"
        "\n"
        "extern fn unwrap(w: F64Wrap): f64\n"
        "\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // { f64 } → SSE class → coerced to double.
    expect(contains(ir, "declare double @unwrap(double)")) << ir;
  };

  "extern fn with two f32 fields uses SSE vector coercion"_test = [] {
    LlvmTestPipeline pipe(
        "class F32Pair:\n"
        "  a: f32\n"
        "  b: f32\n"
        "\n"
        "extern fn sum_pair(p: F32Pair): f32\n"
        "\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // { f32, f32 } → SSE class → coerced to <2 x float>.
    expect(contains(ir, "declare float @sum_pair(<2 x float>)")) << ir;
  };

  // Nested struct classification: recursive field walk preserves SSE.
  "extern fn with nested f64 struct uses SSE coercion"_test = [] {
    LlvmTestPipeline pipe(
        "class F64Wrap:\n"
        "  val: f64\n"
        "\n"
        "class NestedF64:\n"
        "  w: F64Wrap\n"
        "\n"
        "extern fn unwrap_nested(n: NestedF64): f64\n"
        "\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // { { f64 } } → recursive walk → SSE class → double.
    expect(contains(ir, "declare double @unwrap_nested(double)")) << ir;
  };

  // Mixed nested: SSE + INTEGER across eightbytes.
  "extern fn with mixed nested struct coerces correctly"_test = [] {
    LlvmTestPipeline pipe(
        "class F64Wrap:\n"
        "  val: f64\n"
        "\n"
        "class MixedNested:\n"
        "  w: F64Wrap\n"
        "  tag: i32\n"
        "\n"
        "extern fn get_tag(m: MixedNested): i32\n"
        "\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // { { f64 }, i32 } = 16 bytes: EB0=SSE(double), EB1=INTEGER(i32).
    expect(contains(ir, "declare i32 @get_tag(double, i32)")) << ir;
  };

  // Function pointer params lower to ptr.
  "extern fn with callback param lowers to ptr"_test = [] {
    LlvmTestPipeline pipe(
        "extern fn apply(cb: fn(i32, i32): i32, a: i32, b: i32): i32\n"
        "\n"
        "fn main(): i32\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // Function pointer param → ptr in LLVM IR.
    expect(contains(ir, "declare i32 @apply(ptr, i32, i32)")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Module structure
// ---------------------------------------------------------------------------

suite<"module_structure"> module_structure = [] {
  "module contains all functions"_test = [] {
    LlvmTestPipeline pipe(
        "fn foo(): i32\n"
        "  return 1\n"
        "\n"
        "fn bar(): i32\n"
        "  return 2\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "@\"test::foo\"")) << ir;
    expect(contains(ir, "@\"test::bar\"")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Class field access
// ---------------------------------------------------------------------------

suite<"field_access"> field_access = [] {
  "field read via extractvalue"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: f64\n"
        "  y: f64\n"
        "\n"
        "fn getx(p: Point): f64\n"
        "  return p.x\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "%dao.Point")) << ir;
  };

  "second field read"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: f64\n"
        "  y: f64\n"
        "\n"
        "fn gety(p: Point): f64\n"
        "  return p.y\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "%dao.Point")) << ir;
  };

  "field write and read"_test = [] {
    LlvmTestPipeline pipe(
        "class Vec2:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn set_and_get(v: Vec2): i32\n"
        "  v.x = 42\n"
        "  return v.y\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "getelementptr inbounds %dao.Vec2")) << ir;
    expect(contains(ir, "store i32")) << ir;
  };

  "pointer-to-struct field store (Deref -> Field)"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn setx(p: *Point): void\n"
        "  mode unsafe =>\n"
        "    (*p).x = 99\n"
        "  return\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "deref.ptr")) << ir;
    expect(contains(ir, "getelementptr inbounds %dao.Point")) << ir;
    expect(contains(ir, "store i32")) << ir;
  };

  "pointer-to-struct field read (Deref then extractvalue)"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn gety(p: *Point): i32\n"
        "  mode unsafe =>\n"
        "    return (*p).y\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "deref.ptr")) << ir;
    expect(contains(ir, "extractvalue %dao.Point")) << ir;
  };

  "struct type appears in LLVM IR"_test = [] {
    LlvmTestPipeline pipe(
        "class Pair:\n"
        "  a: i32\n"
        "  b: i64\n"
        "\n"
        "fn first(p: Pair): i32\n"
        "  return p.a\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "%dao.Pair = type { i32, i64 }")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Unsupported constructs — must fail explicitly
// ---------------------------------------------------------------------------

suite<"unsupported_constructs"> unsupported_constructs = [] {
  "mode parallel is rejected"_test = [] {
    LlvmTestPipeline pipe(
        "fn test(): i32\n"
        "  mode parallel =>\n"
        "    return 1\n"
        "  return 0\n");
    expect(pipe.has_errors()) << "mode parallel should fail";
  };

  "mode unsafe is accepted"_test = [] {
    LlvmTestPipeline pipe(
        "fn test(x: i32): i32\n"
        "  mode unsafe =>\n"
        "    return x\n"
        "  return 0\n");
    expect(!pipe.has_errors()) << "mode unsafe should be no-op";
  };

  "resource memory calls domain hooks"_test = [] {
    LlvmTestPipeline pipe(
        "fn test(): i32\n"
        "  resource memory Arena =>\n"
        "    return 1\n"
        "  return 0\n");
    expect(!pipe.has_errors()) << "resource memory should compile";
    auto ir = pipe.ir();
    expect(contains(ir, "__dao_mem_resource_enter")) << ir;
    expect(contains(ir, "__dao_mem_resource_exit")) << ir;
  };

  "resource gpu is rejected"_test = [] {
    LlvmTestPipeline pipe(
        "fn test(): i32\n"
        "  resource gpu compute =>\n"
        "    return 1\n"
        "  return 0\n");
    expect(pipe.has_errors()) << "resource gpu should fail";
  };

  "field assignment via store"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn sety(p: Point): i32\n"
        "  p.y = 1\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "getelementptr inbounds %dao.Point")) << ir;
    expect(contains(ir, "store i32")) << ir;
  };

  "field read via extractvalue"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn gety(p: Point): i32\n"
        "  return p.y\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "extractvalue %dao.Point")) << ir;
    expect(contains(ir, "ret i32")) << ir;
  };

  "address-of field via GEP"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn addr(p: Point): *i32\n"
        "  mode unsafe =>\n"
        "    return &p.y\n"
        "  return &p.x\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "getelementptr inbounds %dao.Point")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Class construction
// ---------------------------------------------------------------------------

suite<"construction"> construction = [] {
  "basic construction produces struct value"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn make(): Point\n"
        "  return Point(1, 2)\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "%dao.Point")) << ir;
    expect(contains(ir, "ret %dao.Point")) << ir;
  };

  "construction and field access roundtrip"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn get_x(): i32\n"
        "  let p: Point = Point(10, 20)\n"
        "  return p.x\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "store %dao.Point")) << ir;
    expect(contains(ir, "extractvalue")) << ir;
  };

  "nested construction"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "class Rect:\n"
        "  tl: Point\n"
        "  br: Point\n"
        "\n"
        "fn make(): Rect\n"
        "  return Rect(Point(0, 0), Point(1, 1))\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "%dao.Rect")) << ir;
    expect(contains(ir, "%dao.Point")) << ir;
    expect(contains(ir, "ret %dao.Rect")) << ir;
  };

  "pass constructed value to function"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn get_x(p: Point): i32\n"
        "  return p.x\n"
        "\n"
        "fn main(): i32\n"
        "  return get_x(Point(42, 0))\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "%dao.Point")) << ir;
    expect(contains(ir, "call i32 @\"test::get_x\"")) << ir;
  };

  "construction with non-constant args uses insertvalue"_test = [] {
    LlvmTestPipeline pipe(
        "class Point:\n"
        "  x: i32\n"
        "  y: i32\n"
        "\n"
        "fn make(a: i32, b: i32): Point\n"
        "  return Point(a, b)\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "insertvalue")) << ir;
    expect(contains(ir, "%dao.Point")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Runtime ABI — hook declarations and string shape
// ---------------------------------------------------------------------------

suite<"runtime_abi"> runtime_abi = [] {
  "runtime hooks are declared with correct signatures"_test = [] {
    llvm::LLVMContext ctx;
    auto module = std::make_unique<llvm::Module>("abi_test", ctx);
    LlvmTypeLowering types(ctx);
    LlvmRuntimeHooks hooks(*module, types);
    hooks.declare_all();

    // IO hooks
    auto* write_fn = module->getFunction("__dao_io_write_stdout");
    expect(write_fn != nullptr) << "write_stdout declared";
    expect(write_fn->getReturnType()->isVoidTy()) << "returns void";
    expect(write_fn->arg_size() == 1u) << "1 param";
    expect(write_fn->getArg(0)->getType()->isPointerTy()) << "string ptr param";

    auto* write_err = module->getFunction("__dao_io_write_stderr");
    expect(write_err != nullptr) << "write_stderr declared";
    expect(write_err->getReturnType()->isVoidTy()) << "returns void";

    auto* read_fn = module->getFunction("__dao_io_read_file");
    expect(read_fn != nullptr) << "read_file declared";
    expect(read_fn->getReturnType()->isStructTy()) << "returns dao.string";

    auto* write_file_fn = module->getFunction("__dao_io_write_file");
    expect(write_file_fn != nullptr) << "write_file declared";
    expect(write_file_fn->getReturnType()->isIntegerTy(1)) << "returns bool";
    expect(write_file_fn->arg_size() == 2u) << "2 params";

    auto* exists_fn = module->getFunction("__dao_io_file_exists");
    expect(exists_fn != nullptr) << "file_exists declared";
    expect(exists_fn->getReturnType()->isIntegerTy(1)) << "returns bool";

    // Equality hooks
    auto* eq_i32 = module->getFunction("__dao_eq_i32");
    expect(eq_i32 != nullptr) << "eq_i32 declared";
    expect(eq_i32->getReturnType()->isIntegerTy(1)) << "returns i1";
    expect(eq_i32->arg_size() == 2u) << "2 params";

    auto* eq_f64 = module->getFunction("__dao_eq_f64");
    expect(eq_f64 != nullptr) << "eq_f64 declared";
    expect(eq_f64->getReturnType()->isIntegerTy(1)) << "returns i1";

    auto* eq_bool = module->getFunction("__dao_eq_bool");
    expect(eq_bool != nullptr) << "eq_bool declared";

    auto* eq_str = module->getFunction("__dao_eq_string");
    expect(eq_str != nullptr) << "eq_string declared";
    expect(eq_str->arg_size() == 2u) << "2 ptr params";

    // Conversion hooks
    auto* conv_i32 = module->getFunction("__dao_conv_i32_to_string");
    expect(conv_i32 != nullptr) << "conv_i32_to_string declared";
    expect(conv_i32->getReturnType()->isStructTy()) << "returns dao.string";
    expect(conv_i32->arg_size() == 1u) << "1 param";

    auto* conv_f64 = module->getFunction("__dao_conv_f64_to_string");
    expect(conv_f64 != nullptr) << "conv_f64_to_string declared";

    auto* conv_bool = module->getFunction("__dao_conv_bool_to_string");
    expect(conv_bool != nullptr) << "conv_bool_to_string declared";

    // String operation hooks
    auto* char_at = module->getFunction("__dao_str_char_at");
    expect(char_at != nullptr) << "str_char_at declared";
    expect(char_at->getReturnType()->isIntegerTy(32)) << "returns i32";
    expect(char_at->arg_size() == 2u) << "2 params (str_ptr, i64)";

    auto* substr = module->getFunction("__dao_str_substring");
    expect(substr != nullptr) << "str_substring declared";
    expect(substr->getReturnType()->isStructTy()) << "returns dao.string";
    expect(substr->arg_size() == 3u) << "3 params (str_ptr, i64, i64)";

    auto* indexof = module->getFunction("__dao_str_index_of");
    expect(indexof != nullptr) << "str_index_of declared";
    expect(indexof->getReturnType()->isIntegerTy(64)) << "returns i64";

    auto* sw = module->getFunction("__dao_str_starts_with");
    expect(sw != nullptr) << "str_starts_with declared";
    expect(sw->getReturnType()->isIntegerTy(1)) << "returns i1";

    auto* ew = module->getFunction("__dao_str_ends_with");
    expect(ew != nullptr) << "str_ends_with declared";

    auto* cmp = module->getFunction("__dao_str_compare");
    expect(cmp != nullptr) << "str_compare declared";
    expect(cmp->getReturnType()->isIntegerTy(32)) << "returns i32";
  };

  "string ABI shape matches contract"_test = [] {
    llvm::LLVMContext ctx;
    LlvmTypeLowering types(ctx);

    auto* str_type = types.string_type();
    expect(str_type != nullptr) << "string type exists";
    expect(str_type->isStructTy()) << "is struct";
    expect(str_type->getStructName() == "dao.string") << "named dao.string";
    expect(str_type->getNumElements() == 2u) << "2 fields: ptr + len";
    expect(str_type->getElementType(0)->isPointerTy()) << "field 0 is ptr";
    expect(str_type->getElementType(1)->isIntegerTy(64)) << "field 1 is i64";
  };

  "is_runtime_hook recognizes all hooks"_test = [] {
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_io_write_stdout"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_io_write_stderr"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_io_read_file"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_io_write_file"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_io_file_exists"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_eq_i32"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_eq_f64"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_eq_bool"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_eq_string"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_conv_i32_to_string"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_conv_f64_to_string"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_conv_bool_to_string"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_str_char_at"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_str_substring"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_str_index_of"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_str_starts_with"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_str_ends_with"));
    expect(LlvmRuntimeHooks::is_runtime_hook("__dao_str_compare"));
    expect(!LlvmRuntimeHooks::is_runtime_hook("__write_stdout"));
    expect(!LlvmRuntimeHooks::is_runtime_hook("user_function"));
  };

  "prelude print generates correct runtime call"_test = [] {
    // The extern declaration lives in a prelude-group file, so the
    // resolver allows the __dao_ prefix.
    LlvmTestPipeline pipe(
        "fn print(msg: string): void -> __dao_io_write_stdout(msg)\n"
        "\n"
        "fn main(): void\n"
        "  print(\"hello\")\n",
        {"module core::io\nextern fn __dao_io_write_stdout(msg: string): void\n"});
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "__dao_io_write_stdout")) << ir;
    expect(contains(ir, "dao.string")) << ir;
  };

  "prelude length generates str_length call"_test = [] {
    LlvmTestPipeline pipe(
        "fn length(s: string): i64 -> __dao_str_length(s)\n"
        "\n"
        "fn main(): i64\n"
        "  return length(\"hello\")\n",
        {"module core::string\nextern fn __dao_str_length(s: string): i64\n"});
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "__dao_str_length")) << ir;
    expect(contains(ir, "call i64")) << ir;
  };

  "runtime hooks pre-declared by backend unconditionally"_test = [] {
    // Even without any prelude extern declarations, runtime hooks
    // should be present in the module because LlvmBackend::lower()
    // calls LlvmRuntimeHooks::declare_all() unconditionally.
    LlvmTestPipeline pipe(
        "fn main(): void\n"
        "  return\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // IO
    expect(contains(ir, "declare void @__dao_io_write_stdout")) << ir;
    // Equality
    expect(contains(ir, "declare i1 @__dao_eq_i32")) << ir;
    expect(contains(ir, "declare i1 @__dao_eq_i64")) << ir;
    expect(contains(ir, "declare i1 @__dao_eq_f64")) << ir;
    // Conversion (to_string)
    expect(contains(ir, "@__dao_conv_i32_to_string")) << ir;
    expect(contains(ir, "@__dao_conv_i64_to_string")) << ir;
    // Numeric conversions
    expect(contains(ir, "declare double @__dao_conv_i32_to_f64(i32)")) << ir;
    expect(contains(ir, "declare i64 @__dao_conv_i32_to_i64(i32)")) << ir;
    expect(contains(ir, "declare i32 @__dao_conv_f64_to_i32(double)")) << ir;
    expect(contains(ir, "declare i32 @__dao_conv_i64_to_i32(i64)")) << ir;
    // String
    expect(contains(ir, "@__dao_str_length")) << ir;
    // Overflow (wrapping + saturating)
    expect(contains(ir, "declare i32 @__dao_wrapping_add_i32(i32, i32)")) << ir;
    expect(contains(ir, "declare i64 @__dao_wrapping_add_i64(i64, i64)")) << ir;
    expect(contains(ir, "declare i32 @__dao_saturating_add_i32(i32, i32)")) << ir;
    expect(contains(ir, "declare i64 @__dao_saturating_add_i64(i64, i64)")) << ir;
    // Numeric conversions (new)
    expect(contains(ir, "declare double @__dao_conv_f32_to_f64(float)")) << ir;
    expect(contains(ir, "declare float @__dao_conv_f64_to_f32(double)")) << ir;
    expect(contains(ir, "declare i64 @__dao_conv_f64_to_i64(double)")) << ir;
    expect(contains(ir, "declare i32 @__dao_conv_i8_to_i32(i8)")) << ir;
    expect(contains(ir, "declare i32 @__dao_conv_i32_to_u32(i32)")) << ir;
    // Narrow overflow (i8/i16)
    expect(contains(ir, "declare i8 @__dao_wrapping_add_i8(i8, i8)")) << ir;
    expect(contains(ir, "declare i16 @__dao_saturating_add_i16(i16, i16)")) << ir;
  };
};

// ---------------------------------------------------------------------------
// Generator / coroutine lowering
// ---------------------------------------------------------------------------

suite<"generators"> generators = [] {
  "generator declares init and resume functions"_test = [] {
    LlvmTestPipeline pipe(
        "fn single(): Generator<i32>\n"
        "  yield 42\n"
        "\n"
        "fn main(): i32\n"
        "  for x in single():\n"
        "    return x\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // Init function: returns a generator fat pair.
    expect(contains(ir, "define %dao.generator @\"test::single\"()")) << ir;
    // Resume function: takes ptr, returns void.
    expect(contains(ir, "define void @\"test::single.resume\"(ptr")) << ir;
  };

  "generator init allocates frame"_test = [] {
    LlvmTestPipeline pipe(
        "fn single(): Generator<i32>\n"
        "  yield 1\n"
        "\n"
        "fn main(): i32\n"
        "  for x in single():\n"
        "    return x\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "__dao_gen_alloc")) << ir;
    // Init returns a { ptr, ptr } generator pair.
    expect(contains(ir, "ret %dao.generator")) << ir;
  };

  "generator resume has state dispatch"_test = [] {
    LlvmTestPipeline pipe(
        "fn single(): Generator<i32>\n"
        "  yield 42\n"
        "\n"
        "fn main(): i32\n"
        "  for x in single():\n"
        "    return x\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    expect(contains(ir, "switch i32")) << ir;
    expect(contains(ir, "yield.ptr")) << ir;
  };

  "for-loop over generator produces iterator ops"_test = [] {
    LlvmTestPipeline pipe(
        "fn nums(): Generator<i32>\n"
        "  yield 1\n"
        "  yield 2\n"
        "\n"
        "fn main(): i32\n"
        "  let sum: i32 = 0\n"
        "  for x in nums():\n"
        "    sum = sum + x\n"
        "  return sum\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // Consumer calls resume, reads done flag, reads yield slot.
    expect(contains(ir, "call void %")) << ir;
    expect(contains(ir, "done.ptr")) << ir;
    expect(contains(ir, "yield.val")) << ir;
    // Destroy calls __dao_gen_free.
    expect(contains(ir, "__dao_gen_free")) << ir;
  };

  "early return from for-loop frees generator frame"_test = [] {
    LlvmTestPipeline pipe(
        "fn single(): Generator<i32>\n"
        "  yield 42\n"
        "\n"
        "fn main(): i32\n"
        "  for x in single():\n"
        "    return x\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // The early-return path must call __dao_gen_free before returning.
    // Count occurrences: one for the early-return path, one for the
    // normal loop-exit path.
    size_t count = 0;
    std::string::size_type pos = 0;
    while ((pos = ir.find("__dao_gen_free", pos)) != std::string::npos) {
      ++count;
      ++pos;
    }
    expect(count >= 2u) << "expected >=2 gen_free calls (early + normal), got "
                        << count << "\n" << ir;
  };

  "init passes alignof to gen_alloc"_test = [] {
    LlvmTestPipeline pipe(
        "fn single(): Generator<i32>\n"
        "  yield 1\n"
        "\n"
        "fn main(): i32\n"
        "  for x in single():\n"
        "    return x\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // Alignment is computed via GEP-from-null offsetof trick, not
    // hardcoded.  LLVM constant-folds the GEP to a ConstantExpr,
    // so we check for the { i8, %frame } wrapper struct pattern.
    expect(contains(ir, "{ i8, %\"dao.gen.test::single\" }")) << ir;
  };

  "range generator with params"_test = [] {
    LlvmTestPipeline pipe(
        "fn range(start: i32, end: i32): Generator<i32>\n"
        "  let i: i32 = start\n"
        "  while i < end:\n"
        "    yield i\n"
        "    i = i + 1\n"
        "\n"
        "fn main(): i32\n"
        "  let sum: i32 = 0\n"
        "  for x in range(0, 5):\n"
        "    sum = sum + x\n"
        "  return sum\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors";
    // Init function takes params.
    expect(contains(ir, "define %dao.generator @\"test::range\"(i32 %start, i32 %end)")) << ir;
    // Resume function.
    expect(contains(ir, "define void @\"test::range.resume\"(ptr")) << ir;
    // Frame type exists.
    expect(contains(ir, "dao.gen.test::range")) << ir;
  };

  "generator through storage"_test = [] {
    LlvmTestPipeline pipe(
        "fn single(): Generator<i32>\n"
        "  yield 42\n"
        "\n"
        "fn main(): i32\n"
        "  let g: Generator<i32> = single()\n"
        "  for x in g:\n"
        "    return x\n"
        "  return 0\n");
    auto ir = pipe.ir();
    expect(!pipe.has_errors()) << "no backend errors: " << ir;
    // Generator pair survives store/load through local.
    expect(contains(ir, "%dao.generator")) << ir;
    expect(contains(ir, "gen.frame")) << ir;
    expect(contains(ir, "gen.resume")) << ir;
    expect(contains(ir, "__dao_gen_free")) << ir;
  };
};

// NOLINTEND(readability-magic-numbers)

auto main() -> int {} // NOLINT(readability-named-parameter)
