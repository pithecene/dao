#include "frontend/module/program.h"
#include "frontend/module/module_graph.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unordered_map>

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

auto Program::module_named(std::string_view display) const -> ModuleInfo* {
  auto it = by_display.find(display);
  return it == by_display.end() ? nullptr : it->second;
}

auto canonical_or_self(const std::filesystem::path& path) -> std::filesystem::path {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(path, ec);
  return ec ? path.lexically_normal() : canonical;
}

namespace {

// ---------------------------------------------------------------------------
// Layout and graph over a complete input set
// ---------------------------------------------------------------------------

auto assemble(std::vector<SourceInput> inputs, const GraphInputs& graph) -> Program {
  Program program;

  // file_id order is the prelude group first, then lexical in the
  // display path (§8.4): a pure function of the file set, so the offset
  // space and every output are independent of input order, and prelude
  // declarations precede user files for the passes that still walk one
  // shared scope in file order.
  std::ranges::stable_sort(inputs, {}, [](const SourceInput& input) {
    return std::pair{!input.is_prelude, std::string_view(input.display_path)};
  });

  std::vector<uint64_t> sizes;
  sizes.reserve(inputs.size());
  uint64_t total_bytes = 0;
  for (const auto& input : inputs) {
    sizes.push_back(input.text.size());
    total_bytes += input.text.size();
  }
  if (!position_budget_fits(sizes)) {
    program.diagnostics.push_back(Diagnostic::error(
        Span{}, "program exceeds the 4 GiB offset space: " + std::to_string(inputs.size()) +
                    " files, " + std::to_string(total_bytes) + " bytes"));
    return program;
  }

  for (size_t idx = 0; idx < inputs.size(); ++idx) {
    auto& input = inputs[idx];
    auto base = base_offset_for(std::span<const uint64_t>(sizes).first(idx));
    auto file = std::make_unique<SourceFile>(SourceFile{
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
  build_module_graph(program, graph);
  return program;
}

auto display_path_for(const std::filesystem::path& path,
                      const std::filesystem::path& display_root) -> std::string {
  // Files are ordered by display path (§8.4), so the path must be
  // NORMALIZED first: `./a.dao`, `b/../a.dao`, and `a.dao` name one
  // file and must sort as one spelling, or the same set supplied two
  // ways orders differently and the output differs with it.
  auto normalized = canonical_or_self(path);
  if (!display_root.empty()) {
    auto relative = normalized.lexically_relative(canonical_or_self(display_root));
    if (!relative.empty() && *relative.begin() != "..") {
      return relative.generic_string();
    }
  }
  return normalized.generic_string();
}

// ---------------------------------------------------------------------------
// Root-file discovery (§8.2–§8.3)
//
// Discovery reads a file's imports by parsing it on its own; the final
// offset-space layout parses it again once the file set is complete.
// The second parse is what the passes see, so a parse error surfaces
// there, not here.
// ---------------------------------------------------------------------------

auto import_identities(const SourceInput& input) -> std::vector<std::string> {
  SourceBuffer buffer(input.display_path, input.text);
  auto lexed = lex(buffer);
  if (!lexed.diagnostics.empty()) {
    return {};
  }
  auto parsed = parse(lexed.tokens);
  if (parsed.file == nullptr) {
    return {};
  }
  std::vector<std::string> identities;
  for (const auto* import : parsed.file->imports) {
    identities.push_back(module_display(import->path.segments));
  }
  return identities;
}

/// `<root>/a/b/c.dao` for the first root that has it.
auto locate_module(const std::string& identity, const std::vector<std::filesystem::path>& roots)
    -> std::optional<std::filesystem::path> {
  std::filesystem::path relative;
  for (size_t start = 0; start <= identity.size();) {
    auto end = identity.find("::", start);
    relative /= identity.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (end == std::string::npos) {
      break;
    }
    start = end + 2;
  }
  relative += ".dao";
  for (const auto& root : roots) {
    auto candidate = root / relative;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

struct Discovery {
  std::vector<SourceInput> inputs;
  std::unordered_map<std::string, std::string> display_by_canonical; // loaded files
  GraphInputs graph;

  void add_prelude(const std::filesystem::path& stdlib_root) {
    const auto display_root = stdlib_root.parent_path();
    for (const auto& path : prelude_files(stdlib_root)) {
      add(path, read_source_input(path, /*is_prelude=*/true, display_root));
    }
  }

  auto add(const std::filesystem::path& path, SourceInput input) -> const std::string& {
    auto canonical = canonical_or_self(path).generic_string();
    auto [it, inserted] = display_by_canonical.emplace(canonical, input.display_path);
    if (inserted) {
      inputs.push_back(std::move(input));
    }
    return it->second;
  }

  auto loaded(const std::filesystem::path& path) const -> bool {
    return display_by_canonical.contains(canonical_or_self(path).generic_string());
  }
};

} // namespace

auto build_program(std::vector<SourceInput> inputs,
                   std::optional<std::string> entry,
                   EntryPolicy entry_policy) -> Program {
  return assemble(std::move(inputs),
                  GraphInputs{.entry = std::move(entry), .entry_policy = entry_policy});
}

auto load_program_from_root(const std::filesystem::path& root_file, const ProgramOptions& options)
    -> Program {
  std::vector<std::filesystem::path> roots{root_file.parent_path()};
  roots.insert(roots.end(), options.module_roots.begin(), options.module_roots.end());
  if (!options.stdlib_root.empty()) {
    roots.push_back(options.stdlib_root);
  }

  Discovery discovery;
  if (!options.stdlib_root.empty()) {
    discovery.add_prelude(options.stdlib_root);
  }
  for (const auto& root : roots) {
    discovery.graph.searched_roots.push_back(root.empty() ? "." : root.generic_string());
  }

  std::deque<SourceInput> pending;
  pending.push_back(read_source_input(root_file, /*is_prelude=*/false));
  discovery.graph.root_display = pending.front().display_path;
  // A root file names a program the driver was asked to compile, exactly
  // as an explicit file set does, so it owes the same entry point (§8.1).
  discovery.graph.entry_policy = EntryPolicy::Required;
  discovery.add(root_file, pending.front());

  while (!pending.empty()) {
    auto input = std::move(pending.front());
    pending.pop_front();
    for (const auto& identity : import_identities(input)) {
      auto path = locate_module(identity, roots);
      if (!path) {
        continue; // the graph reports it, naming the roots searched
      }
      if (discovery.loaded(*path)) {
        discovery.graph.located.push_back(
            {.identity = identity,
             .display_path = discovery.display_by_canonical.at(
                 canonical_or_self(*path).generic_string())});
        continue;
      }
      // The display path read_source_input derived is kept as it is:
      // overwriting it with the search spelling would key a discovered
      // file by how the invocation happened to reach it, so the same
      // program ordered files differently under a relative root than an
      // absolute one (CONTRACT_MODULE_SYSTEM.md §8.4).
      auto found = read_source_input(*path, /*is_prelude=*/false);
      discovery.graph.located.push_back({.identity = identity, .display_path = found.display_path});
      pending.push_back(found);
      discovery.add(*path, std::move(found));
    }
  }
  return assemble(std::move(discovery.inputs), discovery.graph);
}

auto load_program_from_files(const std::vector<std::filesystem::path>& files,
                             const ProgramOptions& options) -> Program {
  Discovery discovery;
  if (!options.stdlib_root.empty()) {
    discovery.add_prelude(options.stdlib_root);
  }
  for (const auto& path : files) {
    discovery.add(path, read_source_input(path, /*is_prelude=*/false));
  }
  discovery.graph.entry = options.entry;
  discovery.graph.entry_policy = EntryPolicy::Required;
  return assemble(std::move(discovery.inputs), discovery.graph);
}

auto read_text_file(const std::filesystem::path& path) -> std::string {
  // Checked before the open, not after: opening a directory succeeds and
  // throws on the first read, which terminates the process instead of
  // reporting anything the caller can act on.
  std::error_code unused;
  if (!std::filesystem::is_regular_file(path, unused)) {
    std::cerr << "error: not a source file: " << path << "\n";
    std::exit(EXIT_FAILURE);
  }
  std::ifstream file(path);
  if (!file) {
    std::cerr << "error: could not open: " << path << "\n";
    std::exit(EXIT_FAILURE);
  }
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

auto read_source_input(const std::filesystem::path& path,
                       bool is_prelude,
                       const std::filesystem::path& display_root) -> SourceInput {
  return {.display_path = display_path_for(path, display_root),
          .text = read_text_file(path),
          .is_prelude = is_prelude};
}

auto prelude_files(const std::filesystem::path& stdlib_root) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> files;
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
    std::ranges::sort(paths);
    files.insert(files.end(), paths.begin(), paths.end());
  }
  return files;
}

auto load_prelude_inputs(const std::filesystem::path& stdlib_root) -> std::vector<SourceInput> {
  std::vector<SourceInput> inputs;
  const auto display_root = stdlib_root.parent_path();
  for (const auto& path : prelude_files(stdlib_root)) {
    inputs.push_back(read_source_input(path, /*is_prelude=*/true, display_root));
  }
  return inputs;
}

} // namespace dao
