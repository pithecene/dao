// Drives the compiler service through `dispatch` — no HTTP — over every
// example program.  What it guards:
//   - every route in the tooling surface is bound and validates input
//   - every reply matches the response shape the surface declares
//   - every lexical token of every example receives a semantic token
//   - hover, definition, references, symbols, and completions answer
//     over real programs
//   - every example runs and prints its golden output
//     (testdata/examples/<name>.out; DAO_UPDATE_GOLDENS=1 rewrites them),
//     except those listed with a reason in
//     testdata/examples/known_failures.txt

#include "pipeline.h"
#include "run.h"
#include "service.h"

#include "analysis/tooling_surface.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace dao::playground;
using nlohmann::json;

namespace {

auto repo_root() -> std::filesystem::path { return DAO_SOURCE_DIR; }
auto golden_dir() -> std::filesystem::path { return repo_root() / "testdata" / "examples"; }
auto service_context() -> ServiceContext {
  return {.repo_root = repo_root(), .examples_dir = repo_root() / "examples"};
}

// ---------------------------------------------------------------------------
// Shape checking against the tooling surface
// ---------------------------------------------------------------------------

using Problems = std::vector<std::string>;

void check_value(const json& value, std::string_view type, const std::string& path,
                 Problems& problems);

void check_shape(const json& value, const dao::tooling::ShapeSpec& shape, const std::string& path,
                 Problems& problems) {
  if (!value.is_object()) {
    problems.push_back(path + ": expected object " + std::string(shape.name));
    return;
  }
  std::set<std::string> declared;
  for (const auto& field : shape.fields) {
    declared.emplace(field.name);
    auto it = value.find(std::string(field.name));
    if (it == value.end()) {
      if (!field.optional) {
        problems.push_back(path + ": missing field " + std::string(field.name));
      }
      continue;
    }
    check_value(*it, field.type, path + "." + std::string(field.name), problems);
  }
  for (const auto& [key, _] : value.items()) {
    if (!declared.contains(key)) {
      problems.push_back(path + ": undeclared field " + key);
    }
  }
}

void check_value(const json& value, std::string_view type, const std::string& path,
                 Problems& problems) {
  using namespace dao::tooling;
  if (type.ends_with(" | null")) {
    if (!value.is_null()) {
      check_value(value, type.substr(0, type.size() - 7), path, problems);
    }
    return;
  }
  if (type.ends_with("[]")) {
    if (!value.is_array()) {
      problems.push_back(path + ": expected array of " + std::string(type));
      return;
    }
    auto element = type.substr(0, type.size() - 2);
    for (size_t i = 0; i < value.size(); ++i) {
      check_value(value[i], element, path + "[" + std::to_string(i) + "]", problems);
    }
    return;
  }
  auto expect_enum = [&](auto& allowed) {
    if (!value.is_string() || !std::ranges::contains(allowed, value.get<std::string>())) {
      problems.push_back(path + ": " + value.dump() + " is not a " + std::string(type));
    }
  };
  if (type == "string") {
    if (!value.is_string()) {
      problems.push_back(path + ": expected string");
    }
  } else if (type == "number") {
    if (!value.is_number()) {
      problems.push_back(path + ": expected number");
    }
  } else if (type == "boolean") {
    if (!value.is_boolean()) {
      problems.push_back(path + ": expected boolean");
    }
  } else if (type == "TokenKind") {
    if (!value.is_string() || !token_group(value.get<std::string>())) {
      problems.push_back(path + ": " + value.dump() + " is not a listed token kind");
    }
  } else if (type == "LexicalCategory") {
    expect_enum(kLexicalCategories);
  } else if (type == "DiagnosticSeverity") {
    expect_enum(kDiagnosticSeverities);
  } else if (const auto* shape = find_shape(type)) {
    check_shape(value, *shape, path, problems);
  } else {
    problems.push_back(path + ": surface declares unknown type " + std::string(type));
  }
}

auto joined(const Problems& problems) -> std::string {
  std::string out;
  for (const auto& problem : problems) {
    out += "\n  " + problem;
  }
  return out;
}

/// Dispatch a route and assert the reply matches the surface's response
/// type for it.
auto call(std::string_view route_name, const json& request) -> Reply {
  const auto* route = dao::tooling::find_route(route_name);
  expect(route != nullptr) << "no such route " << route_name;
  auto reply = dispatch(route_name, request, service_context());
  expect(reply.status == http_status::ok)
      << route_name << " replied " << reply.status << ": " << reply.body.dump();
  Problems problems;
  check_value(reply.body, route->response, std::string(route_name), problems);
  expect(problems.empty()) << joined(problems);
  return reply;
}

// ---------------------------------------------------------------------------
// Example corpus
// ---------------------------------------------------------------------------

struct Example {
  std::string name;
  std::string source;
};

auto load_examples() -> std::vector<Example> {
  std::vector<Example> examples;
  auto list = call("examples", json::object());
  for (const auto& entry : list.body["examples"]) {
    auto name = entry["name"].get<std::string>();
    auto source = call("example", {{"name", name}});
    examples.push_back({.name = name, .source = source.body["source"].get<std::string>()});
  }
  return examples;
}

/// `<name>.dao <reason>` per line; blank lines and `#` comments ignored.
auto load_known_failures() -> std::map<std::string, std::string> {
  std::map<std::string, std::string> failures;
  std::ifstream file(golden_dir() / "known_failures.txt");
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    auto split = line.find(' ');
    failures[line.substr(0, split)] =
        split == std::string::npos ? "" : line.substr(split + 1);
  }
  return failures;
}

/// Offsets of the first `count` semantic tokens whose kind starts with `prefix`.
auto sample_offsets(const json& semantic_tokens, std::string_view prefix, size_t count)
    -> std::vector<uint32_t> {
  std::vector<uint32_t> offsets;
  for (const auto& token : semantic_tokens) {
    if (offsets.size() == count) {
      break;
    }
    if (token["kind"].get<std::string>().starts_with(prefix)) {
      offsets.push_back(token["offset"].get<uint32_t>());
    }
  }
  return offsets;
}

auto minimal_request(std::string_view shape_name) -> json {
  if (shape_name == "void") {
    return json::object();
  }
  if (shape_name == "ExampleName") {
    return {{"name", "hello.dao"}};
  }
  json request = json::object();
  for (const auto& field : dao::tooling::find_shape(shape_name)->fields) {
    if (field.optional) {
      continue;
    }
    if (field.type == "string") {
      request[std::string(field.name)] = "";
    } else if (field.type == "number") {
      request[std::string(field.name)] = 0;
    } else if (field.type == "boolean") {
      request[std::string(field.name)] = false;
    }
  }
  return request;
}

} // namespace

suite<"playground_service"> playground_service_suite = [] {
  "every_route_is_bound_and_validates_its_request"_test = [] {
    for (const auto& route : dao::tooling::kRoutes) {
      auto reply = dispatch(route.name, minimal_request(route.request), service_context());
      expect(reply.status != http_status::not_found) << route.name << " is not bound";
      if (route.request != "void") {
        auto rejected = dispatch(route.name, json::object(), service_context());
        expect(rejected.status == http_status::bad_request)
            << route.name << " accepted an empty request body";
      }
    }
    expect(dispatch("nonexistent", json::object(), service_context()).status == http_status::not_found);
    expect(dispatch("hover", {{"source", "x"}, {"offset", "0"}}, service_context()).status ==
           http_status::bad_request)
        << "a string offset must be rejected";
  };

  "examples_analyze_with_every_token_classified"_test = [] {
    auto known_failures = load_known_failures();
    for (const auto& example : load_examples()) {
      auto reply = call("analyze", {{"source", example.source}});
      const auto& body = reply.body;

      std::set<uint32_t> classified;
      for (const auto& token : body["semanticTokens"]) {
        classified.insert(token["offset"].get<uint32_t>());
      }
      for (const auto& token : body["tokens"]) {
        auto offset = token["offset"].get<uint32_t>();
        expect(classified.contains(offset))
            << example.name << ": no semantic token for " << token["text"].get<std::string>()
            << " (" << token["kind"].get<std::string>() << ") at line "
            << token["line"].get<uint32_t>();
      }

      if (!known_failures.contains(example.name)) {
        expect(body["diagnostics"].empty())
            << example.name << " has diagnostics: " << body["diagnostics"].dump();
        expect(!body["llvm_ir"].get<std::string>().empty())
            << example.name << " produced no LLVM IR";
      }
    }
  };

  "navigation_and_completion_answer_over_the_examples"_test = [] {
    constexpr size_t kUsesPerExample = 3;
    constexpr size_t kDeclsPerExample = 2;
    for (const auto& example : load_examples()) {
      auto analysis = call("analyze", {{"source", example.source}});
      const auto& semantic_tokens = analysis.body["semanticTokens"];

      call("documentSymbols", {{"source", example.source}});

      auto uses = sample_offsets(semantic_tokens, "use.", kUsesPerExample);
      auto decls = sample_offsets(semantic_tokens, "decl.function", kDeclsPerExample);
      for (auto offset : uses) {
        json request = {{"source", example.source}, {"offset", offset}};
        call("hover", request);
        call("gotoDef", request);
      }
      for (auto offset : decls) {
        call("references", {{"source", example.source}, {"offset", offset}});
      }
      // `main` is declared in every example and its references include
      // the declaration itself.  (Concept requirements and extension
      // methods do not answer references yet, so other declarations are
      // only shape-checked above.)
      auto main_refs = call("references", {{"source", example.source},
                                           {"offset", example.source.find("fn main") + 3}});
      expect(!main_refs.body.empty()) << example.name << ": no references for main";

      // Scope completion at the end of the buffer, and member completion
      // just after the first `.`.  Members are guaranteed only for class
      // receivers (structs.dao's first `.` reads a field); builtin
      // receivers offer none yet.
      auto end = static_cast<uint32_t>(example.source.size());
      call("completions", {{"source", example.source}, {"offset", end}});
      auto dots = sample_offsets(semantic_tokens, "operator.member", 1);
      if (!dots.empty()) {
        auto members =
            call("completions", {{"source", example.source}, {"offset", dots.front() + 1}});
        if (example.name == "structs.dao") {
          expect(!members.body.empty()) << example.name << ": no members offered after `.`";
        }
      }
    }
  };

  "positions_carry_file_identity"_test = [] {
    // hello.dao: `print` is declared in the prelude, `main` in the buffer.
    auto hello = call("example", {{"name", "hello.dao"}}).body["source"].get<std::string>();
    auto analysis = call("analyze", {{"source", hello}});
    expect(analysis.body["file"].get<std::string>() == kDocumentPath);
    expect(analysis.body["module"].get<std::string>() == "hello")
        << "module: " << analysis.body["module"].dump();

    auto print_use = static_cast<uint32_t>(hello.find("print("));
    auto definition = call("gotoDef", {{"source", hello}, {"offset", print_use}});
    expect(!definition.body.is_null()) << "print has no definition";
    if (!definition.body.is_null()) {
      auto file = definition.body["file"].get<std::string>();
      expect(file.starts_with("stdlib/")) << "print defined in " << file;
    }

    auto main_decl = static_cast<uint32_t>(hello.find("fn main") + 3);
    auto refs = call("references", {{"source", hello}, {"offset", main_decl}});
    for (const auto& ref : refs.body) {
      expect(ref["file"].get<std::string>() == kDocumentPath) << ref.dump();
    }

    // A scratch buffer takes the synthetic module identity.
    auto scratch = call("analyze", {{"source", "fn main(): i32\n  return 0\n"}});
    expect(scratch.body["module"].get<std::string>() == "playground");
  };

  "examples_run_to_their_goldens"_test = [] {
    init_run_support();
    const bool update = std::getenv("DAO_UPDATE_GOLDENS") != nullptr;
    auto known_failures = load_known_failures();

    for (const auto& example : load_examples()) {
      auto reply = call("run", {{"source", example.source}});
      auto exit_code = reply.body["exit_code"].get<int>();
      auto stdout_text = reply.body["stdout"].get<std::string>();

      if (auto failure = known_failures.find(example.name); failure != known_failures.end()) {
        expect(exit_code == -1) << example.name << " now builds (exit " << exit_code
                                << "); remove it from known_failures.txt (" << failure->second
                                << ")";
        continue;
      }

      expect(exit_code == 0) << example.name << " exited " << exit_code << ": "
                             << reply.body["stderr"].get<std::string>()
                             << reply.body["diagnostics"].dump();

      auto golden_path = golden_dir() / (example.name.substr(0, example.name.size() - 4) + ".out");
      if (update) {
        std::ofstream(golden_path, std::ios::binary) << stdout_text;
        continue;
      }
      expect(std::filesystem::exists(golden_path))
          << golden_path.string() << " is missing; run with DAO_UPDATE_GOLDENS=1";
      if (std::filesystem::exists(golden_path)) {
        expect(dao::read_file(golden_path) == stdout_text)
            << example.name << " output differs from " << golden_path.filename().string();
      }
    }
  };
};

// Run the suites from main: boost.ut would otherwise run them from its
// runner's destructor after main returns, when LLVM's globals (used by
// the run route's object emission) are already being torn down.
auto main(int argc, const char** argv) -> int {
  return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv}) ? 1 : 0;
}
