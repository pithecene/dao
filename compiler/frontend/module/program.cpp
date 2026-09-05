#include "frontend/module/program.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

namespace dao {

auto Program::file_nodes() const -> std::vector<const FileNode*> {
  std::vector<const FileNode*> nodes;
  nodes.reserve(files.size());
  for (const auto& file : files) {
    if (file->parse.file != nullptr) {
      nodes.push_back(file->parse.file);
    }
  }
  return nodes;
}

auto Program::user_files() const -> std::vector<const SourceFile*> {
  std::vector<const SourceFile*> result;
  for (const auto& file : files) {
    if (!file->is_prelude) {
      result.push_back(file.get());
    }
  }
  return result;
}

auto Program::lexed_and_parsed_cleanly() const -> bool {
  return std::ranges::all_of(files, [](const auto& file) {
    return file->lex.diagnostics.empty() && file->parse.diagnostics.empty() &&
           file->parse.file != nullptr;
  });
}

auto build_program(std::vector<SourceInput> inputs) -> Program {
  Program program;

  std::vector<uint64_t> sizes;
  sizes.reserve(inputs.size());
  uint64_t total_bytes = 0;
  for (const auto& input : inputs) {
    sizes.push_back(input.text.size());
    total_bytes += input.text.size();
  }
  if (!position_budget_fits(sizes)) {
    program.diagnostics.push_back(Diagnostic::error(
        Span{}, "program exceeds the 4 GiB offset space: " +
                    std::to_string(inputs.size()) + " files, " +
                    std::to_string(total_bytes) + " bytes"));
    return program;
  }

  for (size_t idx = 0; idx < inputs.size(); ++idx) {
    auto& input = inputs[idx];
    auto base = base_offset_for(std::span<const uint64_t>(sizes).first(idx));
    auto file = std::make_unique<SourceFile>(SourceFile{
        .file_id = static_cast<uint32_t>(idx),
        .display_path = input.display_path,
        .buffer = SourceBuffer(input.display_path, std::move(input.text)),
        .base_offset = base,
        .is_prelude = input.is_prelude,
    });
    file->lex = lex(file->buffer, file->base_offset);
    if (file->lex.diagnostics.empty()) {
      file->parse = parse(file->lex.tokens);
    }
    program.files.push_back(std::move(file));
  }

  for (const auto& file : program.files) {
    program.source_map.add(file.get());
  }
  return program;
}

auto read_source_input(const std::filesystem::path& path, bool is_prelude) -> SourceInput {
  std::ifstream file(path);
  if (!file) {
    std::cerr << "error: could not open: " << path << "\n";
    std::exit(EXIT_FAILURE);
  }
  return {.display_path = path.filename().string(),
          .text = {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()},
          .is_prelude = is_prelude};
}

auto load_prelude_inputs(const std::filesystem::path& stdlib_root) -> std::vector<SourceInput> {
  std::vector<SourceInput> inputs;
  const std::filesystem::path dirs[] = {stdlib_root / "core", stdlib_root / "io"};
  for (const auto& dir : dirs) {
    if (!std::filesystem::exists(dir)) {
      continue;
    }
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      if (entry.path().extension() == ".dao") {
        paths.push_back(entry.path());
      }
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
      inputs.push_back(read_source_input(path, /*is_prelude=*/true));
    }
  }
  return inputs;
}

} // namespace dao
