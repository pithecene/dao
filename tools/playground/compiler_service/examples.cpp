#include "examples.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dao::playground {

namespace {

constexpr size_t kDaoExtensionLength = 4;

auto read_whole_file(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

auto has_path_separators(const std::string& name) -> bool {
  return name.empty() || name.find('/') != std::string::npos ||
         name.find('\\') != std::string::npos || name.find("..") != std::string::npos;
}

/// A bare `<name>.dao` file name directly under `examples/`.
auto is_single_example_name(const std::string& name) -> bool {
  return !has_path_separators(name) && name.ends_with(".dao") && name.size() > kDaoExtensionLength;
}

/// A bare directory name directly under `examples/`.
auto is_directory_example_name(const std::string& name) -> bool {
  return !has_path_separators(name) && name.find('.') == std::string::npos;
}

struct MultifileProgram {
  nlohmann::json files = nlohmann::json::array();
  std::string document;
  std::string entry_source;
};

/// Load a subdirectory under `examples/` when its `.dao` files declare a
/// single `fn main` between them (one multi-file program rather than a
/// directory of separate probes).
auto load_multifile_directory(const std::filesystem::path& dir, const std::string& dir_name)
    -> std::optional<MultifileProgram> {
  if (!std::filesystem::is_directory(dir)) {
    return std::nullopt;
  }
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".dao") {
      paths.push_back(entry.path());
    }
  }
  std::ranges::sort(paths);

  MultifileProgram program;
  size_t entry_count = 0;
  for (const auto& path : paths) {
    auto source = read_whole_file(path);
    auto display_path = dir_name + "/" + path.filename().string();
    if (source.find("fn main(") != std::string::npos) {
      program.document = display_path;
      program.entry_source = source;
      ++entry_count;
    }
    program.files.push_back({{"path", std::move(display_path)}, {"source", std::move(source)}});
  }
  if (entry_count != 1) {
    return std::nullopt;
  }
  return program;
}

struct ListedExample {
  std::string name;
  bool is_multifile = false;
};

} // namespace

auto examples_list(const nlohmann::json& /*request*/, const ServiceContext& ctx) -> Reply {
  std::vector<ListedExample> listed;
  if (std::filesystem::exists(ctx.examples_dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(ctx.examples_dir)) {
      auto filename = entry.path().filename().string();
      if (entry.is_regular_file() && entry.path().extension() == ".dao") {
        listed.push_back({.name = std::move(filename), .is_multifile = false});
      } else if (entry.is_directory() && is_directory_example_name(filename) &&
                 load_multifile_directory(entry.path(), filename).has_value()) {
        listed.push_back({.name = std::move(filename), .is_multifile = true});
      }
    }
  }
  std::ranges::sort(listed, {}, &ListedExample::name);

  nlohmann::json examples = nlohmann::json::array();
  for (const auto& item : listed) {
    examples.push_back({{"name", item.name}, {"isMultifile", item.is_multifile}});
  }
  return {.status = http_status::ok, .body = {{"examples", std::move(examples)}}};
}

auto example_source(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto name = request["name"].get<std::string>();
  if (is_single_example_name(name)) {
    auto path = ctx.examples_dir / name;
    if (!std::filesystem::is_regular_file(path)) {
      return error_reply(http_status::not_found, "example not found");
    }
    auto contents = read_whole_file(path);
    nlohmann::json files = nlohmann::json::array({{{"path", name}, {"source", contents}}});
    return {.status = http_status::ok,
            .body = {
                {"name", name},
                {"files", std::move(files)},
                {"document", name},
                {"source", std::move(contents)},
            }};
  }
  if (is_directory_example_name(name)) {
    auto loaded = load_multifile_directory(ctx.examples_dir / name, name);
    if (!loaded.has_value()) {
      return error_reply(http_status::not_found, "example not found");
    }
    return {.status = http_status::ok,
            .body = {
                {"name", name},
                {"files", std::move(loaded->files)},
                {"document", std::move(loaded->document)},
                {"source", std::move(loaded->entry_source)},
            }};
  }
  return error_reply(http_status::bad_request, "invalid example name");
}

} // namespace dao::playground
