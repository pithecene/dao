#include "examples.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

// NOLINTBEGIN(readability-magic-numbers)
namespace dao::playground {

namespace {

/// A bare `<name>.dao` file name: no directories, no parent references.
auto is_example_name(const std::string& name) -> bool {
  return name.ends_with(".dao") && name.size() > 4 && name.find('/') == std::string::npos &&
         name.find('\\') == std::string::npos && name.find("..") == std::string::npos;
}

} // namespace

auto examples_list(const nlohmann::json& /*request*/, const ServiceContext& ctx) -> Reply {
  std::vector<std::string> names;
  if (std::filesystem::exists(ctx.examples_dir)) {
    for (const auto& entry : std::filesystem::directory_iterator(ctx.examples_dir)) {
      if (entry.path().extension() == ".dao") {
        names.push_back(entry.path().filename().string());
      }
    }
  }
  std::ranges::sort(names);

  nlohmann::json examples = nlohmann::json::array();
  for (const auto& name : names) {
    examples.push_back({{"name", name}});
  }
  return {.status = http_status::ok, .body = {{"examples", examples}}};
}

auto example_source(const nlohmann::json& request, const ServiceContext& ctx) -> Reply {
  auto name = request["name"].get<std::string>();
  if (!is_example_name(name)) {
    return error_reply(http_status::bad_request, "invalid example name");
  }
  auto path = ctx.examples_dir / name;
  if (!std::filesystem::exists(path)) {
    return error_reply(http_status::not_found, "example not found");
  }
  std::ifstream file(path);
  std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  return {.status = http_status::ok, .body = {{"name", name}, {"source", contents}}};
}

} // namespace dao::playground
// NOLINTEND(readability-magic-numbers)
