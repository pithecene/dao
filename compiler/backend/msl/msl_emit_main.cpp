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

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_located_errors(const dao::SourceMap& source_map,
                          std::span<const dao::Diagnostic> diags) {
  std::vector<dao::Diagnostic> ordered(diags.begin(), diags.end());
  std::ranges::stable_sort(ordered, {}, [](const dao::Diagnostic& d) { return d.span.offset; });
  for (const auto& diag : ordered) {
    if (diag.severity != dao::Severity::Error) {
      continue;
    }
    const auto* file = source_map.file_for(diag.span.offset);
    if (file != nullptr) {
      const auto loc = source_map.locate(diag.span.offset);
      std::cerr << file->display_path << ":" << loc.line << ":" << loc.col;
    } else {
      std::cerr << "<program>";
    }
    std::cerr << ": error: " << diag.message << "\n";
  }
}

auto write_output_file(const std::filesystem::path& path, std::string_view contents) -> bool {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::cerr << "error: cannot open output file: " << path << "\n";
    return false;
  }
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return out.good();
}

void print_usage() {
  std::cerr << "usage: dao_msl_emit <root.dao> <kernel>... [--bundle-out FILE.metal] "
               "[--manifest-out FILE.tsv] [--stdlib-root DIR]\n";
}

} // namespace

auto main(int argc, char* argv[]) -> int {
  if (argc < 3) {
    print_usage();
    return EXIT_FAILURE;
  }

  std::filesystem::path root_file;
  std::filesystem::path bundle_out;
  std::filesystem::path manifest_out;
  std::filesystem::path stdlib_root = std::filesystem::path(DAO_SOURCE_DIR) / "stdlib";
  std::vector<std::string> kernel_storage;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    auto next_val = [&]() -> std::string_view {
      if (i + 1 >= argc) {
        std::cerr << "error: " << arg << " requires a value\n";
        std::exit(EXIT_FAILURE);
      }
      return argv[++i];
    };
    if (arg == "--bundle-out") {
      bundle_out = next_val();
    } else if (arg == "--manifest-out") {
      manifest_out = next_val();
    } else if (arg == "--stdlib-root") {
      stdlib_root = next_val();
    } else if (arg.starts_with("--")) {
      std::cerr << "error: unknown option " << arg << "\n";
      return EXIT_FAILURE;
    } else if (root_file.empty()) {
      root_file = arg;
    } else {
      kernel_storage.emplace_back(arg);
    }
  }

  if (root_file.empty() || kernel_storage.empty()) {
    print_usage();
    return EXIT_FAILURE;
  }
  if (!std::filesystem::exists(root_file)) {
    std::cerr << "error: file not found: " << root_file << "\n";
    return EXIT_FAILURE;
  }

  dao::ProgramOptions options;
  options.stdlib_root = stdlib_root;
  options.entry_policy = dao::EntryPolicy::Optional;

  auto program = dao::load_program_from_root(root_file, options);
  auto assembly_diags = dao::assembly_diagnostics(program);
  bool has_errors = false;
  for (const auto& d : assembly_diags.unlocated) {
    if (d.severity == dao::Severity::Error) {
      std::cerr << "error: " << d.message << "\n";
      has_errors = true;
    }
  }
  if (!assembly_diags.located.empty()) {
    print_located_errors(program.source_map, assembly_diags.located);
    for (const auto& d : assembly_diags.located) {
      if (d.severity == dao::Severity::Error) {
        has_errors = true;
      }
    }
  }
  if (has_errors || !program.lexed_and_parsed_cleanly()) {
    return EXIT_FAILURE;
  }

  auto resolve_res = dao::resolve(program);
  dao::TypeContext types;
  auto check_res = dao::typecheck(program, resolve_res, types);
  std::vector<dao::Diagnostic> fe_diags = resolve_res.diagnostics;
  fe_diags.insert(fe_diags.end(), check_res.diagnostics.begin(), check_res.diagnostics.end());
  for (const auto& d : fe_diags) {
    if (d.severity == dao::Severity::Error) {
      has_errors = true;
    }
  }
  if (has_errors) {
    print_located_errors(program.source_map, fe_diags);
    return EXIT_FAILURE;
  }

  dao::HirContext hir_ctx;
  auto hir_res = dao::build_hir(program, resolve_res, check_res, hir_ctx);
  if (hir_res.program == nullptr || !hir_res.diagnostics.empty()) {
    print_located_errors(program.source_map, hir_res.diagnostics);
    return EXIT_FAILURE;
  }

  dao::MirContext mir_ctx;
  auto mir_res = dao::build_mir(*hir_res.program, mir_ctx, types);
  if (mir_res.module == nullptr || !mir_res.diagnostics.empty()) {
    print_located_errors(program.source_map, mir_res.diagnostics);
    return EXIT_FAILURE;
  }
  auto mono_res = dao::monomorphize(*mir_res.module, mir_ctx, types, mir_res.generic_templates);
  if (!mono_res.diagnostics.empty()) {
    print_located_errors(program.source_map, mono_res.diagnostics);
    return EXIT_FAILURE;
  }

  std::vector<std::string_view> kernel_views;
  kernel_views.reserve(kernel_storage.size());
  for (const auto& name : kernel_storage) {
    kernel_views.push_back(name);
  }

  auto outcome = dao::MslBackend::lower_kernels(*mir_res.module, kernel_views, &program.source_map);
  if (!outcome.rejections.empty()) {
    std::vector<dao::Diagnostic> diags;
    diags.reserve(outcome.rejections.size());
    for (const auto& rej : outcome.rejections) {
      diags.push_back(rej.to_diagnostic());
    }
    print_located_errors(program.source_map, diags);
    return EXIT_FAILURE;
  }

  const std::string manifest = dao::MslBackend::render_manifest_tsv(outcome);
  if (!bundle_out.empty() && !write_output_file(bundle_out, outcome.bundle_source)) {
    return EXIT_FAILURE;
  }
  if (!manifest_out.empty() && !write_output_file(manifest_out, manifest)) {
    return EXIT_FAILURE;
  }
  if (bundle_out.empty() && manifest_out.empty()) {
    std::cout << outcome.bundle_source;
  }
  return EXIT_SUCCESS;
}
