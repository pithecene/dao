#include "backend/msl/msl_backend.h"

#include "frontend/module/program.h"
#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"
#include "frontend/types/type_context.h"
#include "ir/hir/hir_builder.h"
#include "ir/hir/hir_context.h"
#include "ir/mir/mir_builder.h"
#include "ir/mir/mir_context.h"
#include "ir/mir/mir_monomorphize.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using namespace boost::ut;
using namespace dao;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

struct MslTestPipeline {
  Program program;
  ResolveResult resolve_res;
  TypeContext types;
  TypeCheckResult check_res;
  HirContext hir_ctx;
  HirBuildResult hir_res;
  MirContext mir_ctx;
  MirBuildResult mir_res;

  explicit MslTestPipeline(std::vector<SourceInput> inputs)
      : program(build_program(std::move(inputs))) {
    resolve_res = resolve(program);
    check_res = typecheck(program, resolve_res, types);
    hir_res = build_hir(program, resolve_res, check_res, hir_ctx);
    if (hir_res.program != nullptr) {
      mir_res = build_mir(*hir_res.program, mir_ctx, types);
      if (mir_res.module != nullptr) {
        (void)monomorphize(*mir_res.module, mir_ctx, types, mir_res.generic_templates);
      }
    }
  }

  static auto single(const std::string& src) -> MslTestPipeline {
    return MslTestPipeline({SourceInput{
        .display_path = "kernel_fixture.dao",
        .text = wrap_with_test_module(src),
        .is_prelude = false,
    }});
  }

  [[nodiscard]] auto lower(std::initializer_list<std::string_view> names) const
      -> MslBackendOutcome {
    std::vector<std::string_view> vec(names);
    return MslBackend::lower_kernels(*mir_res.module, vec, &program.source_map);
  }
};

auto contains(const std::string& haystack, std::string_view needle) -> bool {
  return haystack.find(needle) != std::string::npos;
}

} // namespace

suite<"msl_backend"> msl_backend_suite = [] {
  "lowers arbitrary pure scalar i32 functions with provenance and checked arithmetic"_test = [] {
    MslTestPipeline pipe = MslTestPipeline::single(
        "fn custom_affine(x: i32, bias: i32): i32\n"
        "  let half: i32 = x / 2\n"
        "  return bias + 3 * half\n");
    auto out = pipe.lower({"custom_affine"});
    expect(out.rejections.empty());
    expect(out.kernels.size() == 1_ul);

    const auto& k = out.kernels.front();
    expect(k.operation_name == "custom_affine");
    expect(k.target == kMslTargetId);
    expect(k.origin.module_name == "test");
    expect(k.origin.file == "kernel_fixture.dao");
    expect(k.origin.line >= 2_u);
    expect(k.signature.param_names == std::vector<std::string>{"x", "bias"});
    expect(k.signature.param_types == std::vector<std::string>{"i32", "i32"});
    expect(k.signature.return_type == "i32");
    expect(k.signature.effects == kMslKernelEffectProfile);

    expect(contains(out.bundle_source, "kernel void custom_affine("));
    expect(contains(out.bundle_source, "dao_div_i32("));
    expect(contains(out.bundle_source, "dao_mul_i32("));
    expect(contains(out.bundle_source, "dao_add_i32("));
    expect(contains(out.bundle_source, "atomic_fetch_or_explicit(dao_status, 1u"));
  };

  "lowers multi-block conditional kernels without goto"_test = [] {
    MslTestPipeline pipe = MslTestPipeline::single(
        "fn piecewise_step(w: i32): i32\n"
        "  let acc: i32 = w - 10\n"
        "  if acc >= 0:\n"
        "    return 100 + acc\n"
        "  return -acc\n");
    auto out = pipe.lower({"piecewise_step"});
    expect(out.rejections.empty());
    expect(out.kernels.size() == 1_ul);
    expect(contains(out.bundle_source, "switch (dao_block)"));
    expect(contains(out.bundle_source, "dao_neg_i32("));
    expect(!contains(out.bundle_source, "goto "));
  };

  "lowers forecast_plans reference kernels directly from authored Dao source"_test = [] {
    ProgramOptions opts;
    opts.stdlib_root = std::filesystem::path(DAO_SOURCE_DIR) / "stdlib";
    opts.entry_policy = EntryPolicy::Required;
    auto prog = load_program_from_root(
        std::filesystem::path(DAO_SOURCE_DIR) / "examples/forecast_plans.dao", opts);
    auto res = resolve(prog);
    TypeContext types;
    auto tc = typecheck(prog, res, types);
    HirContext hir_ctx;
    auto hir = build_hir(prog, res, tc, hir_ctx);
    MirContext mir_ctx;
    auto mir = build_mir(*hir.program, mir_ctx, types);
    (void)monomorphize(*mir.module, mir_ctx, types, mir.generic_templates);

    std::vector<std::string_view> names{"score_v1", "high_wind", "adjust"};
    auto out = MslBackend::lower_kernels(*mir.module, names, &prog.source_map);
    expect(out.rejections.empty());
    expect(out.kernels.size() == 3_ul);
    expect(out.kernels[0].origin.module_name == "forecast_plans");
    expect(out.kernels[0].operation_name == "score_v1");
    expect(out.kernels[1].operation_name == "high_wind");
    expect(out.kernels[2].operation_name == "adjust");

    const std::string tsv = MslBackend::render_manifest_tsv(out.kernels);
    expect(contains(tsv, "kernel\tforecast_plans\tscore_v1\t"));
    expect(contains(tsv, "kernel\tforecast_plans\thigh_wind\t"));
    expect(contains(tsv, "kernel\tforecast_plans\tadjust\t"));
  };

  "diagnoses unsupported types, calls, loops, and missing or ambiguous kernels"_test = [] {
    MslTestPipeline float_pipe = MslTestPipeline::single("fn bad_float(x: f64): f64 -> x\n");
    auto r1 = float_pipe.lower({"bad_float"});
    expect(r1.kernels.empty());
    expect(r1.rejections.size() == 1_ul);
    expect(contains(r1.rejections.front().message, "return type"));

    MslTestPipeline call_pipe = MslTestPipeline::single(
        "fn helper(x: i32): i32 -> x + 1\n"
        "fn caller(x: i32): i32 -> helper(x)\n");
    auto r2 = call_pipe.lower({"caller"});
    expect(r2.kernels.empty());
    expect(!r2.rejections.empty());
    expect(contains(r2.rejections.front().message, "function calls are unsupported"));

    MslTestPipeline loop_pipe = MslTestPipeline::single(
        "fn spin(n: i32): i32\n"
        "  let i: i32 = 0\n"
        "  while i < n:\n"
        "    i = i + 1\n"
        "  return i\n");
    auto r3 = loop_pipe.lower({"spin"});
    expect(r3.kernels.empty());
    expect(!r3.rejections.empty());
    expect(contains(r3.rejections.front().message, "cyclic control flow"));

    auto r4 = loop_pipe.lower({"nonexistent_kernel"});
    expect(r4.kernels.empty());
    expect(r4.rejections.size() == 1_ul);
    expect(contains(r4.rejections.front().message, "was not found"));

    MslTestPipeline ambig_pipe({
        SourceInput{.display_path = "a.dao",
                    .text = "module a\nfn same(x: i32): i32 -> x + 1\n",
                    .is_prelude = false},
        SourceInput{.display_path = "b.dao",
                    .text = "module b\nfn same(x: i32): i32 -> x + 2\n",
                    .is_prelude = false},
    });
    auto r5 = ambig_pipe.lower({"same"});
    expect(r5.kernels.empty());
    expect(r5.rejections.size() == 1_ul);
    expect(contains(r5.rejections.front().message, "ambiguous"));

    auto r6 = ambig_pipe.lower({"b::same"});
    expect(r6.rejections.empty());
    expect(r6.kernels.size() == 1_ul);
    expect(r6.kernels.front().origin.module_name == "b");

    auto r7 = ambig_pipe.lower({"a::same", "b::same"});
    expect(r7.bundle_source.empty());
    expect(r7.rejections.size() == 1_ul);
    expect(contains(r7.rejections.front().message, "collides across"));

    auto r8 = ambig_pipe.lower({"a::same", "a::same"});
    expect(r8.bundle_source.empty());
    expect(r8.rejections.size() == 1_ul);
    expect(contains(r8.rejections.front().message, "duplicate kernel request"));

    MslTestPipeline alias_req_pipe = MslTestPipeline::single("fn k(x: i32): i32 -> x + 1\n");
    auto r9 = alias_req_pipe.lower({"k", "test::k"});
    expect(r9.bundle_source.empty());
    expect(r9.rejections.size() == 1_ul);
    expect(contains(r9.rejections.front().message, "resolves to the same function"));
  };
};

auto main(int argc, const char** argv) -> int {
  return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv}) ? 1 : 0;
}

// NOLINTEND(readability-magic-numbers)
