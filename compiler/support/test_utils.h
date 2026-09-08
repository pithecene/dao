#ifndef DAO_SUPPORT_TEST_UTILS_H
#define DAO_SUPPORT_TEST_UTILS_H

#include "frontend/module/program.h"
#include "support/module_utils.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dao {

/// Read the entire contents of a file into a string.
/// Shared across test binaries to eliminate copy-pasted helpers.
inline auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// Prefix for the synthetic module declaration injected by test helpers.
inline constexpr const char* kTestModulePrefix = "module test\n";
inline constexpr uint32_t kTestModulePrefixBytes = 12;

/// Wrap a test source string with a synthetic `module test` declaration.
/// Idempotent: sources that already begin with `module` (after optional
/// whitespace/comments) are returned unchanged.
inline auto wrap_with_test_module(std::string_view src) -> std::string {
  if (starts_with_module(src)) {
    return std::string(src);
  }
  std::string wrapped = kTestModulePrefix;
  wrapped.append(src);
  return wrapped;
}

/// Build a Program from prelude sources (each a real stdlib file with its
/// own `module` line, marked as prelude group) and one user source
/// (wrapped with `module test` if it lacks a module line); `user_file`
/// finds the latter.
inline auto make_test_program(std::string_view user_source,
                              std::span<const std::string> prelude_sources = {})
    -> Program {
  std::vector<SourceInput> inputs;
  inputs.reserve(prelude_sources.size() + 1);
  for (size_t i = 0; i < prelude_sources.size(); ++i) {
    inputs.push_back({.display_path = "prelude" + std::to_string(i) + ".dao",
                      .text = prelude_sources[i],
                      .is_prelude = true});
  }
  inputs.push_back({.display_path = "test.dao",
                    .text = wrap_with_test_module(user_source),
                    .is_prelude = false});
  return build_program(std::move(inputs));
}

/// The stdlib prelude group as source strings (stdlib/core then
/// stdlib/io, sorted) under the given repository root.
inline auto stdlib_prelude_sources(const std::filesystem::path& repo_root)
    -> std::vector<std::string> {
  std::vector<std::string> sources;
  for (auto& input : load_prelude_inputs(repo_root / "stdlib")) {
    sources.push_back(std::move(input.text));
  }
  return sources;
}

/// The user file of a program built by make_test_program.
inline auto user_file(const Program& program) -> const SourceFile& {
  return *program.user_files().front();
}

} // namespace dao

#endif // DAO_SUPPORT_TEST_UTILS_H
