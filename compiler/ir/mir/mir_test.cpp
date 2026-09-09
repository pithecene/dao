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
#include "ir/mir/mir_printer.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>
#include <sstream>
#include <string>

using namespace boost::ut;
using namespace dao;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

/// Owns all pipeline state so MIR nodes remain valid.
struct MirTestPipeline {
  SourceBuffer source;
  LexResult lex_result;
  ParseResult parse_result;
  ResolveResult resolve_result;
  TypeContext types;
  TypeCheckResult check_result;
  HirContext hir_ctx;
  HirBuildResult hir_result;
  MirContext mir_ctx;
  MirBuildResult mir_result;

  explicit MirTestPipeline(const std::string& src)
      : source("test.dao", wrap_with_test_module(src)),
        lex_result(lex(source)),
        parse_result(parse(lex_result.tokens)) {
    if (parse_result.file != nullptr) {
      resolve_result = resolve(*parse_result.file);
      check_result =
          typecheck(*parse_result.file, resolve_result, types);
      hir_result =
          build_hir(*parse_result.file, resolve_result, check_result,
                    hir_ctx);
      if (hir_result.program != nullptr) {
        mir_result = build_mir(*hir_result.program, mir_ctx, types);
      }
    }
  }

  [[nodiscard]] auto module() const -> MirModule* {
    return mir_result.module;
  }

  [[nodiscard]] auto dump() const -> std::string {
    std::ostringstream out;
    if (mir_result.module != nullptr) {
      print_mir(out, *mir_result.module);
    }
    return out.str();
  }
};

auto contains(const std::string& haystack, std::string_view needle) -> bool {
  return haystack.find(needle) != std::string::npos;
}

auto count_of(const std::string& haystack, std::string_view needle) -> size_t {
  size_t count = 0;
  for (auto pos = haystack.find(needle); pos != std::string::npos;
       pos = haystack.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

} // namespace

// ---------------------------------------------------------------------------
// Control flow: if
// ---------------------------------------------------------------------------

suite<"mir_if"> mir_if = [] {
  "if lowers to cond_br with then and merge blocks"_test = [] {
    MirTestPipeline pipe(
        "fn test(x: i32): i32\n"
        "    if x > 0:\n"
        "        return x\n"
        "    return 0\n");
    auto dump = pipe.dump();
    expect(contains(dump, "cond_br")) << dump;
    expect(contains(dump, "bb1:")) << dump; // then block
    expect(contains(dump, "bb2:")) << dump; // merge block
    expect(contains(dump, "binary >")) << dump;
  };

  "if-else with both returns has no merge block"_test = [] {
    MirTestPipeline pipe(
        "fn test(x: i32): i32\n"
        "    if x > 0:\n"
        "        return 1\n"
        "    else:\n"
        "        return 0\n");
    auto dump = pipe.dump();
    expect(contains(dump, "cond_br")) << dump;
    expect(contains(dump, "bb1:")) << dump;  // then
    expect(contains(dump, "bb2:")) << dump;  // else
    expect(!contains(dump, "bb3:")) << dump; // no dangling merge
  };

  "if-else with fallthrough has merge block"_test = [] {
    MirTestPipeline pipe(
        "fn test(x: i32): i32\n"
        "    let r: i32 = 0\n"
        "    if x > 0:\n"
        "        r = 1\n"
        "    else:\n"
        "        r = 2\n"
        "    return r\n");
    auto dump = pipe.dump();
    expect(contains(dump, "cond_br")) << dump;
    expect(contains(dump, "bb1:")) << dump; // then
    expect(contains(dump, "bb2:")) << dump; // else
    expect(contains(dump, "bb3:")) << dump; // merge
  };
};

// ---------------------------------------------------------------------------
// Control flow: while
// ---------------------------------------------------------------------------

suite<"mir_while"> mir_while = [] {
  "while lowers to loop blocks"_test = [] {
    MirTestPipeline pipe(
        "fn test(x: i32): i32\n"
        "    let i: i32 = 0\n"
        "    while i < x:\n"
        "        i = i + 1\n"
        "    return i\n");
    auto dump = pipe.dump();
    // Should have: entry -> cond -> body -> back to cond, exit
    expect(contains(dump, "br bb")) << dump;
    expect(contains(dump, "cond_br")) << dump;
    expect(contains(dump, "binary <")) << dump;
    expect(contains(dump, "binary +")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Control flow: return
// ---------------------------------------------------------------------------

suite<"mir_return"> mir_return = [] {
  "return with value"_test = [] {
    MirTestPipeline pipe("fn main(): i32\n    return 42\n");
    auto dump = pipe.dump();
    expect(contains(dump, "const_int 42")) << dump;
    expect(contains(dump, "return %")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Data flow: let with initializer
// ---------------------------------------------------------------------------

suite<"mir_let"> mir_let = [] {
  "let with initializer lowers to store"_test = [] {
    MirTestPipeline pipe(
        "fn main(): i32\n"
        "    let x: i32 = 42\n"
        "    return x\n");
    auto dump = pipe.dump();
    expect(contains(dump, "const_int 42")) << dump;
    expect(contains(dump, "store _")) << dump;
    expect(contains(dump, "load _")) << dump;
  };

  "let without initializer declares local"_test = [] {
    MirTestPipeline pipe(
        "fn main(): i32\n"
        "    let x: i32 = 0\n"
        "    return x\n");
    auto dump = pipe.dump();
    // Local should be declared.
    expect(contains(dump, "local _")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Data flow: assignment
// ---------------------------------------------------------------------------

suite<"mir_assignment"> mir_assignment = [] {
  "assignment lowers to store"_test = [] {
    MirTestPipeline pipe(
        "fn main(): i32\n"
        "    let x: i32 = 0\n"
        "    x = 42\n"
        "    return x\n");
    auto dump = pipe.dump();
    // Two stores: one for init, one for assignment.
    // Count store occurrences.
    size_t count = 0;
    size_t pos = 0;
    while ((pos = dump.find("store _", pos)) != std::string::npos) {
      ++count;
      ++pos;
    }
    expect(count >= 2_ul) << dump;
  };
};

// ---------------------------------------------------------------------------
// Data flow: arithmetic
// ---------------------------------------------------------------------------

suite<"mir_arithmetic"> mir_arithmetic = [] {
  "arithmetic lowers to binary instructions"_test = [] {
    MirTestPipeline pipe(
        "fn add(a: i32, b: i32): i32\n"
        "    return a + b\n");
    auto dump = pipe.dump();
    expect(contains(dump, "binary + %")) << dump;
    expect(contains(dump, ": i32")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Data flow: call
// ---------------------------------------------------------------------------

suite<"mir_call"> mir_call = [] {
  "call lowers with explicit args"_test = [] {
    MirTestPipeline pipe(
        "fn add(a: i32, b: i32): i32 -> a + b\n"
        "fn main(): i32\n"
        "    return add(1, 2)\n");
    auto dump = pipe.dump();
    expect(contains(dump, "call %")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Dao-specific: pipe lowering
// ---------------------------------------------------------------------------

suite<"mir_pipe"> mir_pipe = [] {
  "pipe lowers to call"_test = [] {
    MirTestPipeline pipe(
        "fn double(x: i32): i32 -> x + x\n"
        "fn main(): i32\n"
        "    return 5 |> double\n");
    auto dump = pipe.dump();
    // Pipe should become a call instruction, not a pipe node.
    expect(!contains(dump, "pipe")) << dump;
    expect(contains(dump, "call %")) << dump;
    expect(contains(dump, "const_int 5")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Dao-specific: mode region
// ---------------------------------------------------------------------------

suite<"mir_mode"> mir_mode = [] {
  "mode region preserved as enter/exit"_test = [] {
    MirTestPipeline pipe(
        "fn main(): i32\n"
        "    mode unsafe =>\n"
        "        let x: i32 = 42\n"
        "    return 0\n");
    auto dump = pipe.dump();
    expect(contains(dump, "mode_enter unsafe")) << dump;
    expect(contains(dump, "mode_exit")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Dao-specific: resource region
// ---------------------------------------------------------------------------

suite<"mir_resource"> mir_resource = [] {
  "resource region preserved as enter/exit with metadata"_test = [] {
    MirTestPipeline pipe(
        "fn main(): i32\n"
        "    resource gpu compute =>\n"
        "        let x: i32 = 1\n"
        "    return 0\n");
    auto dump = pipe.dump();
    expect(contains(dump, "resource_enter gpu compute")) << dump;
    expect(contains(dump, "resource_exit gpu compute")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Dao-specific: early return inside region
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Dao-specific: `break` leaves only the regions the loop entered
// ---------------------------------------------------------------------------

suite<"mir_break_unwinding"> mir_break_unwinding = [] {
  "break out of a loop inside a resource block leaves the block open"_test = [] {
    // The block's own end exits it; a break inside the loop must not.
    // One enter, one exit.
    MirTestPipeline pipe("fn f(x: i32): i32\n"
                         "    resource memory pool =>\n"
                         "        while x > 0:\n"
                         "            if x == 3:\n"
                         "                break\n"
                         "            x = x - 1\n"
                         "    return x\n");
    auto dump = pipe.dump();
    expect(eq(count_of(dump, "resource_enter memory pool"), size_t{1})) << dump;
    expect(eq(count_of(dump, "resource_exit memory pool"), size_t{1})) << dump;
  };

  "an owning value leaving a block needs the prelude's copy_out"_test = [] {
    // Without a prelude there is nothing to copy with; the builder says
    // so rather than letting the value dangle past the block's exit.
    MirTestPipeline pipe("fn f(): string\n"
                         "    let out: string = \"\"\n"
                         "    resource memory pool =>\n"
                         "        out = \"made inside\"\n"
                         "    return out\n");
    expect(!pipe.mir_result.diagnostics.empty()) << "no diagnostic: " << pipe.dump();
    expect(!pipe.mir_result.diagnostics.empty() &&
           pipe.mir_result.diagnostics[0].message.find("no prelude `copy_out`") !=
               std::string::npos)
        << (pipe.mir_result.diagnostics.empty() ? "" : pipe.mir_result.diagnostics[0].message);
  };

  "break out of a loop leaves a resource block entered inside the loop"_test = [] {
    // The block is inside the loop body: the break path exits it (the
    // fall-through end of the block exits it too, on its own path).
    MirTestPipeline pipe("fn f(x: i32): i32\n"
                         "    while x > 0:\n"
                         "        resource memory pool =>\n"
                         "            if x == 3:\n"
                         "                break\n"
                         "            x = x - 1\n"
                         "    return x\n");
    auto dump = pipe.dump();
    expect(eq(count_of(dump, "resource_enter memory pool"), size_t{1})) << dump;
    expect(eq(count_of(dump, "resource_exit memory pool"), size_t{2})) << dump;
  };

  "break out of a for loop inside a resource block destroys the iterator once"_test = [] {
    // The loop's exit block destroys the iterator; the break must not
    // destroy it again, nor exit the enclosing block.
    MirTestPipeline pipe("fn gen(): Generator<i32>\n"
                         "    yield 1\n"
                         "    yield 2\n"
                         "fn f(): i32\n"
                         "    let total: i32 = 0\n"
                         "    resource memory pool =>\n"
                         "        for v in gen():\n"
                         "            if v == 2:\n"
                         "                break\n"
                         "            total = total + v\n"
                         "    return total\n");
    auto dump = pipe.dump();
    expect(eq(count_of(dump, "iter_destroy"), size_t{1})) << dump;
    expect(eq(count_of(dump, "resource_exit memory pool"), size_t{1})) << dump;
  };

  "return inside a for loop inside a resource block leaves both"_test = [] {
    // A return leaves every region: the iterator and the block.
    MirTestPipeline pipe("fn gen(): Generator<i32>\n"
                         "    yield 1\n"
                         "fn f(): i32\n"
                         "    resource memory pool =>\n"
                         "        for v in gen():\n"
                         "            if v == 1:\n"
                         "                return v\n"
                         "    return 0\n");
    auto dump = pipe.dump();
    // The return path and the loop's exit block each destroy the iterator.
    expect(eq(count_of(dump, "iter_destroy"), size_t{2})) << dump;
    // The return path and the block's fall-through each exit the block.
    expect(eq(count_of(dump, "resource_exit memory pool"), size_t{2})) << dump;
  };
};

suite<"mir_region_early_return"> mir_region_early_return = [] {
  "early return inside mode emits mode_exit before return"_test = [] {
    MirTestPipeline pipe(
        "fn test(x: i32): i32\n"
        "    mode unsafe =>\n"
        "        if x > 0:\n"
        "            return x\n"
        "    return 0\n");
    auto dump = pipe.dump();
    expect(contains(dump, "mode_enter unsafe")) << dump;

    // There must be TWO mode_exit instructions: one on the early-return
    // path (cleanup before return x) and one on the normal fallthrough
    // path (after the if body). A single mode_exit would mean the
    // early-return cleanup is broken.
    size_t exit_count = 0;
    size_t pos = 0;
    while ((pos = dump.find("mode_exit", pos)) != std::string::npos) {
      ++exit_count;
      ++pos;
    }
    expect(exit_count == 2_ul)
        << "expected 2 mode_exit (early-return + normal), got "
        << exit_count;
  };

  "early return inside resource emits resource_exit before return"_test =
      [] {
    MirTestPipeline pipe(
        "fn test(x: i32): i32\n"
        "    resource memory pool =>\n"
        "        if x > 0:\n"
        "            return x\n"
        "    return 0\n");
    auto dump = pipe.dump();
    expect(contains(dump, "resource_enter memory pool")) << dump;

    // There must be TWO resource_exit instructions: one on the
    // early-return path (cleanup before return x) and one on the
    // normal fallthrough. A single resource_exit would mean the
    // early-return cleanup is broken.
    size_t exit_count = 0;
    size_t pos = 0;
    while ((pos = dump.find("resource_exit memory pool", pos))
               != std::string::npos) {
      ++exit_count;
      ++pos;
    }
    expect(exit_count == 2_ul)
        << "expected 2 resource_exit (early-return + normal), got "
        << exit_count;
  };
};

// ---------------------------------------------------------------------------
// Dao-specific: nested regions
// ---------------------------------------------------------------------------

suite<"mir_nested_regions"> mir_nested_regions = [] {
  "mode inside resource emits correct nesting"_test = [] {
    MirTestPipeline pipe(
        "fn test(): i32\n"
        "    resource memory pool =>\n"
        "        mode unsafe =>\n"
        "            let x: i32 = 1\n"
        "    return 0\n");
    auto dump = pipe.dump();

    // Verify nesting order: resource_enter, mode_enter, mode_exit,
    // resource_exit.
    auto res_enter = dump.find("resource_enter memory pool");
    auto mode_enter = dump.find("mode_enter unsafe");
    auto mode_exit = dump.find("mode_exit");
    auto res_exit = dump.find("resource_exit memory pool");

    expect(res_enter != std::string::npos) << dump;
    expect(mode_enter != std::string::npos) << dump;
    expect(mode_exit != std::string::npos) << dump;
    expect(res_exit != std::string::npos) << dump;

    expect(res_enter < mode_enter) << "resource_enter before mode_enter";
    expect(mode_enter < mode_exit) << "mode_enter before mode_exit";
    expect(mode_exit < res_exit) << "mode_exit before resource_exit";
  };

  "early return in nested regions unwinds in reverse order"_test = [] {
    MirTestPipeline pipe(
        "fn test(x: i32): i32\n"
        "    resource memory pool =>\n"
        "        mode unsafe =>\n"
        "            return x\n"
        "    return 0\n");
    auto dump = pipe.dump();

    // Early return must unwind: mode_exit then resource_exit then return.
    // Find the sequence starting from the early-return block.
    auto mode_exit = dump.find("mode_exit");
    expect(mode_exit != std::string::npos) << dump;
    auto res_exit = dump.find("resource_exit memory pool", mode_exit);
    expect(res_exit != std::string::npos)
        << "resource_exit must follow mode_exit on early return";
    auto ret_pos = dump.find("return %", res_exit);
    expect(ret_pos != std::string::npos)
        << "return must follow resource_exit on early return";
  };
};

// ---------------------------------------------------------------------------
// Dao-specific: sequential regions
// ---------------------------------------------------------------------------

suite<"mir_sequential_regions"> mir_sequential_regions = [] {
  "sequential resource regions do not interleave"_test = [] {
    MirTestPipeline pipe(
        "fn test(): i32\n"
        "    resource memory pool_a =>\n"
        "        let a: i32 = 1\n"
        "    resource memory pool_b =>\n"
        "        let b: i32 = 2\n"
        "    return 0\n");
    auto dump = pipe.dump();

    // First region: enter_a, exit_a
    auto enter_a = dump.find("resource_enter memory pool_a");
    auto exit_a = dump.find("resource_exit memory pool_a");
    // Second region: enter_b, exit_b
    auto enter_b = dump.find("resource_enter memory pool_b");
    auto exit_b = dump.find("resource_exit memory pool_b");

    expect(enter_a != std::string::npos) << dump;
    expect(exit_a != std::string::npos) << dump;
    expect(enter_b != std::string::npos) << dump;
    expect(exit_b != std::string::npos) << dump;

    // exit_a must come before enter_b.
    expect(exit_a < enter_b)
        << "first region must exit before second region enters";
  };
};

// ---------------------------------------------------------------------------
// Dao-specific: lambda
// ---------------------------------------------------------------------------

suite<"mir_lambda"> mir_lambda = [] {
  // NOTE: bare lambdas (e.g. `|x| -> x + 1`) are currently rejected by the
  // type checker without contextual function type information. Once lambda
  // type inference is implemented, this test should be updated to verify
  // that lambda lowers as a nested MirFunction with `lambda` instruction.
  "lambda lowering placeholder — pipeline produces module"_test = [] {
    // This source is well-typed (no lambda) and just proves the
    // pipeline doesn't crash; real lambda MIR coverage is deferred.
    MirTestPipeline pipe("fn main(): i32\n    return 42\n");
    expect(pipe.module() != nullptr);
  };
};

// ---------------------------------------------------------------------------
// Structural: every block ends with terminator
// ---------------------------------------------------------------------------

suite<"mir_structural"> mir_structural = [] {
  "every block ends with terminator"_test = [] {
    MirTestPipeline pipe(
        "fn test(x: i32): i32\n"
        "    if x > 0:\n"
        "        return x\n"
        "    return 0\n");
    auto* mod = pipe.module();
    expect(mod != nullptr);
    for (const auto* fn : mod->functions) {
      for (const auto* block : fn->blocks) {
        expect(!block->insts.empty()) << "block must not be empty";
        if (!block->insts.empty()) {
          expect(is_terminator(block->insts.back()->kind()))
              << "block must end with terminator";
        }
      }
    }
  };

  "values carry types"_test = [] {
    MirTestPipeline pipe(
        "fn main(): i32\n    return 1 + 2\n");
    auto* mod = pipe.module();
    expect(mod != nullptr);
    const auto* fn = mod->functions[0];
    // Check that at least one value-producing instruction has a type.
    bool found_typed = false;
    for (const auto* block : fn->blocks) {
      for (const auto* inst : block->insts) {
        if (inst->result.valid() && inst->type != nullptr) {
          found_typed = true;
        }
      }
    }
    expect(found_typed) << "at least one instruction has type";
  };
};

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

suite<"mir_edge"> mir_edge = [] {
  "empty function"_test = [] {
    MirTestPipeline pipe("fn noop(): void\n    let x: i32 = 1\n");
    auto dump = pipe.dump();
    expect(contains(dump, "fn noop")) << dump;
    // Should have an implicit return terminator.
    expect(contains(dump, "return")) << dump;
  };

  "for loop lowers to iter instructions"_test = [] {
    MirTestPipeline pipe(
        "fn gen(n: i32): Generator<i32>\n"
        "    yield n\n"
        "fn test(): i32\n"
        "    for item in gen(10):\n"
        "        let y: i32 = item\n"
        "    return 0\n");
    auto dump = pipe.dump();
    expect(contains(dump, "iter_init")) << dump;
    expect(contains(dump, "iter_has_next")) << dump;
    expect(contains(dump, "iter_next")) << dump;
    expect(contains(dump, "cond_br")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Generator / yield
// ---------------------------------------------------------------------------

suite<"mir_generator"> mir_generator = [] {
  "yield lowers to MirYield"_test = [] {
    MirTestPipeline pipe(
        "fn range(n: i32): Generator<i32>\n"
        "    let i = 0\n"
        "    while i < n:\n"
        "        yield i\n"
        "        i = i + 1\n");
    auto dump = pipe.dump();
    expect(contains(dump, "yield %")) << dump;
  };

  "for-in over generator extracts element type"_test = [] {
    MirTestPipeline pipe(
        "fn range(n: i32): Generator<i32>\n"
        "    yield n\n"
        "fn main(): i32\n"
        "    let total = 0\n"
        "    for x in range(10):\n"
        "        total = total + x\n"
        "    return total\n");
    auto dump = pipe.dump();
    expect(contains(dump, "iter_init")) << dump;
    expect(contains(dump, "iter_has_next")) << dump;
    // iter_next should produce i32, not Generator<i32>
    expect(contains(dump, "iter_next")) << dump;
    expect(contains(dump, ": i32")) << dump;
  };
};

// ---------------------------------------------------------------------------
// Task 28: MIR concreteness invariant
// ---------------------------------------------------------------------------

suite<"mir_concreteness"> mir_concreteness = [] {
  // After monomorphization, module.functions must contain only
  // type-concrete bodies.  Generic declarations live in
  // generic_templates and are cloned+specialized on demand.
  "monomorphization produces no concreteness diagnostics"_test = [] {
    MirTestPipeline pipe(
        "fn id<T>(x: T): T -> x\n"
        "fn main(): i32\n"
        "    let a: i32 = id<i32>(42)\n"
        "    return a\n");
    // Generic id<T> should be in generic_templates, not
    // module.functions.  Only main and id$i32 (the specialization)
    // should end up in module.functions after monomorphize.
    auto mono = monomorphize(*pipe.module(), pipe.mir_ctx, pipe.types,
                             pipe.mir_result.generic_templates);
    expect(mono.diagnostics.empty())
        << "unexpected concreteness diagnostics: "
        << (mono.diagnostics.empty() ? ""
                                     : mono.diagnostics[0].message);
    // Verify no generic residue in final module.
    for (const auto* fn : pipe.module()->functions) {
      const bool has_generic_return =
          fn->return_type != nullptr &&
          fn->return_type->kind() == TypeKind::GenericParam;
      expect(!has_generic_return)
          << "function " << (fn->symbol != nullptr ? fn->symbol->name : "")
          << " has generic return type";
    }
  };

  // A program with no generics should monomorphize as a no-op
  // with no diagnostics.
  "no-op monomorphization"_test = [] {
    MirTestPipeline pipe(
        "fn add(a: i32, b: i32): i32 -> a + b\n"
        "fn main(): i32\n"
        "    return add(1, 2)\n");
    auto mono = monomorphize(*pipe.module(), pipe.mir_ctx, pipe.types,
                             pipe.mir_result.generic_templates);
    expect(mono.diagnostics.empty());
    expect(pipe.mir_result.generic_templates.empty());
  };

  // Regression: the invariant scan must not stack-overflow on
  // recursive types like `class Node { next: *Node }` where
  // following Struct fields through a Pointer pointee returns to
  // the same Struct.
  "concreteness check handles recursive types"_test = [] {
    MirTestPipeline pipe(
        "class Node:\n"
        "    next: *Node\n"
        "    value: i32\n"
        "fn main(): i32\n"
        "    return 0\n");
    // Should complete without stack overflow and without emitting
    // spurious concreteness diagnostics.
    auto mono = monomorphize(*pipe.module(), pipe.mir_ctx, pipe.types,
                             pipe.mir_result.generic_templates);
    expect(mono.diagnostics.empty())
        << "unexpected concreteness diagnostics on recursive type";
  };

  // Concreteness check catches synthetic residue: manually inject a
  // generic function into module.functions and verify monomorphize
  // emits the internal-error diagnostic.
  "concreteness check fires on synthetic residue"_test = [] {
    MirTestPipeline pipe(
        "fn main(): i32\n"
        "    return 0\n");
    // Manually craft a MirFunction whose return type is a generic
    // parameter (simulating a regression where a generic body slipped
    // into module.functions).
    auto* bad_fn = pipe.mir_ctx.alloc<MirFunction>();
    bad_fn->return_type =
        pipe.types.generic_param(/*binder=*/nullptr, "T", 0);
    bad_fn->span = {};
    bad_fn->is_extern = false;
    pipe.module()->functions.push_back(bad_fn);
    auto mono = monomorphize(*pipe.module(), pipe.mir_ctx, pipe.types,
                             pipe.mir_result.generic_templates);
    // Expect at least one error diagnostic about the invariant.
    bool found = false;
    for (const auto& diag : mono.diagnostics) {
      if (diag.severity == Severity::Error &&
          diag.message.find("MIR concreteness invariant violated") !=
              std::string::npos) {
        found = true;
        break;
      }
    }
    expect(found) << "expected concreteness invariant diagnostic";
  };
};

// NOLINTEND(readability-magic-numbers)

auto main() -> int {} // NOLINT(readability-named-parameter)
