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

#include "service_surface.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace boost::ut;
using namespace dao::playground;
using nlohmann::json;

namespace {

auto repo_root() -> std::filesystem::path {
  return DAO_SOURCE_DIR;
}
/// The path the tests give their one document; replies about it name it.
constexpr std::string_view kTestDocument = "main.dao";

/// A program-shaped request carrying one document, plus any extra fields.
auto document_request(const std::string& source, json extra = json::object()) -> json {
  json request = {{"files", json::array({{{"path", kTestDocument}, {"source", source}}})},
                  {"document", kTestDocument}};
  request.update(extra);
  return request;
}

auto golden_dir() -> std::filesystem::path {
  return repo_root() / "testdata" / "examples";
}
auto service_context() -> ServiceContext {
  return {.repo_root = repo_root(), .examples_dir = repo_root() / "examples"};
}

// ---------------------------------------------------------------------------
// Shape checking against the tooling surface
// ---------------------------------------------------------------------------

using Problems = std::vector<std::string>;

void check_value(const json& value,
                 std::string_view type,
                 const std::string& path,
                 Problems& problems);

void check_shape(const json& value,
                 const ShapeSpec& shape,
                 const std::string& path,
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

void check_value(const json& value,
                 std::string_view type,
                 const std::string& path,
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
  } else if (const auto* shape = find_service_shape(type)) {
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
  const auto* route = find_route(route_name);
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

/// `<name>.dao<TAB><expected diagnostic substring>` per line; blank lines
/// and `#` comments ignored.  The substring is what the compiler must
/// report for the failure to count as the known one.
auto load_known_failures() -> std::map<std::string, std::string> {
  std::map<std::string, std::string> failures;
  std::ifstream file(golden_dir() / "known_failures.txt");
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    auto split = line.find('\t');
    expect(split != std::string::npos) << "known_failures.txt: no expected diagnostic on: " << line;
    failures[line.substr(0, split)] = split == std::string::npos ? "" : line.substr(split + 1);
  }
  return failures;
}

/// True if any diagnostic message of a reply contains `needle`.
auto reports(const json& reply, const std::string& needle) -> bool {
  for (const auto& diag : reply["diagnostics"]) {
    if (diag["message"].get<std::string>().find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

/// The located diagnostics of a reply as (file, file-local offset)
/// pairs, in the order the reply lists them.  Entries with no position
/// carry an empty file and are skipped: only located ones have an order
/// to check.
auto located_positions(const json& reply) -> std::vector<std::pair<std::string, uint32_t>> {
  std::vector<std::pair<std::string, uint32_t>> positions;
  for (const auto& diag : reply["diagnostics"]) {
    auto file = diag["file"].get<std::string>();
    if (!file.empty()) {
      positions.emplace_back(std::move(file), diag["offset"].get<uint32_t>());
    }
  }
  return positions;
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
  for (const auto& field : find_service_shape(shape_name)->fields) {
    if (field.optional) {
      continue;
    }
    if (field.type == "SourceInput[]") {
      request[std::string(field.name)] = json::array({{{"path", kTestDocument}, {"source", ""}}});
    } else if (field.name == "document") {
      request[std::string(field.name)] = kTestDocument;
    } else if (field.type == "string") {
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
    for (const auto& route : kRoutes) {
      auto reply = dispatch(route.name, minimal_request(route.request), service_context());
      expect(reply.status != http_status::not_found) << route.name << " is not bound";
      if (route.request != "void") {
        auto rejected = dispatch(route.name, json::object(), service_context());
        expect(rejected.status == http_status::bad_request)
            << route.name << " accepted an empty request body";
      }
    }
    expect(dispatch("nonexistent", json::object(), service_context()).status ==
           http_status::not_found);
    expect(dispatch("hover", document_request("x", {{"offset", "0"}}), service_context()).status ==
           http_status::bad_request)
        << "a string offset must be rejected";
    auto unnamed = document_request("x", {{"document", "elsewhere.dao"}, {"offset", 0}});
    expect(dispatch("hover", unnamed, service_context()).status == http_status::bad_request)
        << "a document that is not one of the files must be rejected";
  };

  "diagnostics from different phases come back in file order"_test = [] {
    // Assembly and resolution are different phases, and the phases run
    // in dependency order, not file order.  `z.dao` cannot be parsed and
    // `a.dao` names something that does not exist: the parse error is
    // known first, the unknown name only once the resolver runs, and
    // appending each phase as it finishes would put `z.dao` ahead of
    // `a.dao`.  One stream, in program order (§8.4), regardless of which
    // phase said what.
    auto program = json{
        {"files",
         json::array(
             {{{"path", "z.dao"}, {"source", "module z\n\nfn also(: i32\n  return 2\n"}},
              {{"path", kTestDocument}, {"source", "module app\n\nfn main(): i32\n  return 0\n"}},
              {{"path", "a.dao"}, {"source", "module a\n\nfn f(): i32 -> missing\n"}}})},
        {"document", kTestDocument}};
    auto reported = call("analyze", program).body["diagnostics"];
    std::string said = reported.dump();
    std::vector<std::string> positioned;
    for (const auto& diag : reported) {
      auto file = diag["file"].get<std::string>();
      if (!file.empty()) {
        positioned.push_back(file);
      }
    }
    expect(positioned.size() >= 2) << "both phases must report: " << said;
    expect(std::ranges::is_sorted(positioned)) << "out of file order across phases: " << said;
    expect(positioned.front() == "a.dao")
        << "the resolver's error in a.dao must come first: " << said;
    expect(said.find("missing") != std::string::npos) << said;
  };

  "a graph error does not hide the parse errors around it"_test = [] {
    // Assembly, lex, and parse are one ordered stream (§8.4).  Returning
    // on the graph error would report the cycle and nothing else;
    // reporting the phases in turn would put the cycle before both parse
    // errors instead of between them; and collecting the files a second
    // time after the stream would report each parse error twice.
    const std::string broken_z = "module z\nimport a\n\nfn also(: i32\n  return 2\n";
    auto program_of = [&](const std::string& first, const std::string& document) {
      return json{{"files",
                   json::array({{{"path", "z.dao"}, {"source", broken_z}},
                                {{"path", kTestDocument}, {"source", document}},
                                {{"path", "a.dao"}, {"source", first}}})},
                  {"document", kTestDocument}};
    };
    // Both programs stop at assembly — the first on the cycle, the
    // second on the document's lex error — so each reply is exactly the
    // stream under test, with no later phase appending to it.
    auto cyclic = program_of("module a\nimport z\n\nfn broken(: i32\n  return 1\n",
                             "module app\n\nfn main(): i32\n  return 0\n");
    auto unlexable = program_of("module a\n\nfn broken(: i32\n  return 1\n",
                                "module app\n\nfn main(): i32\n  return @\n");

    // File ids follow the display path, not the request order, so this
    // is the order the assembly diagnostics must come back in.
    const std::vector<std::string> by_file_id = {"a.dao", "main.dao", "z.dao"};
    /// Assert the reply names each positioned diagnostic once, in file
    /// then offset order, and answer which files it named.
    auto stream_of = [&](std::string_view label, const json& diagnostics) {
      std::vector<std::pair<size_t, uint32_t>> ordered;
      std::set<std::string> once;
      std::set<std::string> files_named;
      for (const auto& diag : diagnostics) {
        auto file = diag["file"].get<std::string>();
        if (file.empty()) {
          continue; // no position to order by
        }
        auto offset = diag["offset"].get<uint32_t>();
        auto key = file + "@" + std::to_string(offset) + ": " + diag["message"].get<std::string>();
        expect(once.insert(key).second) << label << ": reported twice: " << key;
        auto rank = std::ranges::find(by_file_id, file);
        expect(rank != by_file_id.end()) << label << ": unexpected file " << file;
        files_named.insert(file);
        ordered.emplace_back(static_cast<size_t>(rank - by_file_id.begin()), offset);
      }
      expect(std::ranges::is_sorted(ordered))
          << label << ": out of file/offset order: " << diagnostics.dump();
      return files_named;
    };

    for (const auto* route : {"analyze", "run"}) {
      auto reported = call(route, cyclic).body["diagnostics"];
      std::string said = reported.dump();
      expect(said.find("import cycle: a -> z -> a") != std::string::npos) << route << ": " << said;
      expect(stream_of(route, reported) == std::set<std::string>{"a.dao", "z.dao"})
          << route << ": the parse errors around the cycle are missing: " << said;

      auto without_a_cycle = call(route, unlexable).body["diagnostics"];
      expect(stream_of(route, without_a_cycle) ==
             std::set<std::string>{"a.dao", "main.dao", "z.dao"})
          << route << ": " << without_a_cycle.dump();
    }
  };

  "a document without main is advised, not failed"_test = [] {
    // EntryPolicy::Advisory: analysis runs to completion and the reply
    // says why Run will not work, as a warning rather than an error.
    auto reply = call("analyze", document_request("module t\nfn f(): i32\n  return 1\n"));
    bool advised = false;
    for (const auto& diag : reply.body["diagnostics"]) {
      if (diag["message"].get<std::string>().find("no entry module") != std::string::npos) {
        advised = true;
        expect(diag["severity"].get<std::string>() == "warning") << diag.dump();
      }
    }
    expect(advised) << reply.body["diagnostics"].dump();
    expect(!reply.body["llvm_ir"].get<std::string>().empty())
        << "an advisory must not stop lowering: " << reply.body["diagnostics"].dump();
  };

  "completion does not offer another module's extension"_test = [] {
    // Tooling must not advertise a call the checker rejects: `secret`
    // is introduced by an `extend` in a module the document does not
    // (and cannot) import for that purpose (CONTRACT_MODULE_SYSTEM.md §5).
    const std::string main = "module app::main\nfn use_it(): i32\n  let v: i32 = 1\n  return v.";
    json request = {{"files",
                     json::array({{{"path", kTestDocument}, {"source", main}},
                                  {{"path", "ext.dao"},
                                   {"source",
                                    "module app::ext\nextend i32 as Secret:\n"
                                    "  fn secret(self): i32 -> 42\n"}}})},
                    {"document", kTestDocument},
                    {"offset", static_cast<uint32_t>(main.size())}};
    auto reply = call("completions", request);
    for (const auto& item : reply.body) {
      expect(item["label"].get<std::string>() != "secret")
          << "offered an extension of another module: " << reply.body.dump();
    }
  };

  "completion offers one method where a call would select one"_test = [] {
    // The document's Box has its own `pick`; the document also extends
    // Box with a `pick` of another concept.  A call selects the type's
    // own method, and completion offers that one, not both.
    const std::string main =
        "module app::main\nclass Box:\n  n: i32\n  fn pick(self): i32 -> self.n\nconcept Alt:\n  "
        "fn pick(self): string\nextend Box as Alt:\n  fn pick(self): string -> \"x\"\nfn use_it(): "
        "i32\n  let b: Box = Box(1)\n  return b.";
    auto reply = call("completions",
                      document_request(main, {{"offset", static_cast<uint32_t>(main.size())}}));
    size_t picks = 0;
    std::string type;
    for (const auto& item : reply.body) {
      if (item["label"].get<std::string>() == "pick") {
        ++picks;
        type = item["type"].get<std::string>();
      }
    }
    expect(picks == 1_u) << "offered " << picks << " pick(s): " << reply.body.dump();
    expect(type.find("i32") != std::string::npos) << "offered the shadowed extension: " << type;
  };

  "completion offers the document's own extension"_test = [] {
    const std::string main = "module app::main\nextend i32 as Secret:\n"
                             "  fn secret(self): i32 -> 42\n"
                             "fn use_it(): i32\n  let v: i32 = 1\n  return v.";
    auto reply = call("completions",
                      document_request(main, {{"offset", static_cast<uint32_t>(main.size())}}));
    bool offered = false;
    for (const auto& item : reply.body) {
      offered = offered || item["label"].get<std::string>() == "secret";
    }
    expect(offered) << "an extension of this module must be offered: " << reply.body.dump();
  };

  "duplicate file paths are rejected"_test = [] {
    // Two files with one path made `document` ambiguous: the synthetic
    // module header was measured from one copy and positions from the
    // other, so the document's length underflowed and any offset passed.
    const std::string with_header = "module t\nfn main(): i32\n  return 0\n";
    const std::string without = "fn f(): i32\n  return 0\n";
    json duplicated = {{"files",
                        json::array({{{"path", kTestDocument}, {"source", without}},
                                     {{"path", kTestDocument}, {"source", with_header}}})},
                       {"document", kTestDocument}};
    for (const auto& route : kRoutes) {
      if (route.request == "void" || route.request == "ExampleName") {
        continue;
      }
      json request = duplicated;
      request["offset"] = 999;
      auto reply = dispatch(route.name, request, service_context());
      expect(reply.status == http_status::bad_request)
          << route.name << " accepted duplicate paths: " << reply.status << " "
          << reply.body.dump();
    }
  };

  "document offsets are range checked"_test = [] {
    const std::string source = "module t\nfn main(): i32\n  return 0\n";
    const auto length = static_cast<int64_t>(source.size());
    const json rejected[] = {json(-1), json(3.5), json(length + 1), json("0")};
    for (const auto& offset : rejected) {
      for (const char* route : {"hover", "gotoDef", "references", "completions"}) {
        auto reply =
            dispatch(route, document_request(source, {{"offset", offset}}), service_context());
        expect(reply.status == http_status::bad_request)
            << route << " accepted offset " << offset.dump() << ": " << reply.body.dump();
      }
    }
    for (const char* route : {"hover", "gotoDef", "references", "completions"}) {
      auto at_end =
          dispatch(route, document_request(source, {{"offset", length}}), service_context());
      expect(at_end.status == http_status::ok) << route << " rejected the end of the document";
    }
  };

  "malformed nested request entries are rejected, not dereferenced"_test = [] {
    // Every element of `files` is validated against SourceInput before the
    // pipeline reads it; an empty entry used to abort the process.
    const json malformed[] = {
        {{"files", json::array({json::object()})}, {"document", "x"}, {"offset", 0}},
        {{"files", json::array({{{"path", 1}, {"source", ""}}})}, {"document", "x"}, {"offset", 0}},
        {{"files", json::array({{{"path", "x"}}})}, {"document", "x"}, {"offset", 0}},
        {{"files", "x"}, {"document", "x"}, {"offset", 0}},
        {{"files", json::array({"x"})}, {"document", "x"}, {"offset", 0}},
    };
    for (const auto& request : malformed) {
      auto reply = dispatch("hover", request, service_context());
      expect(reply.status == http_status::bad_request)
          << request.dump() << " -> " << reply.status << " " << reply.body.dump();
    }
  };

  "examples_analyze_with_every_token_classified"_test = [] {
    auto known_failures = load_known_failures();
    for (const auto& example : load_examples()) {
      auto reply = call("analyze", document_request(example.source));
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
      auto analysis = call("analyze", document_request(example.source));
      const auto& semantic_tokens = analysis.body["semanticTokens"];

      call("documentSymbols", document_request(example.source));

      auto uses = sample_offsets(semantic_tokens, "use.", kUsesPerExample);
      auto decls = sample_offsets(semantic_tokens, "decl.function", kDeclsPerExample);
      for (auto offset : uses) {
        json request = document_request(example.source, {{"offset", offset}});
        call("hover", request);
        call("gotoDef", request);
      }
      for (auto offset : decls) {
        call("references", document_request(example.source, {{"offset", offset}}));
      }
      // `main` is declared in every example and its references include
      // the declaration itself.  (Concept requirements and extension
      // methods do not answer references yet, so other declarations are
      // only shape-checked above.)
      auto main_refs =
          call("references",
               document_request(example.source, {{"offset", example.source.find("fn main") + 3}}));
      expect(!main_refs.body.empty()) << example.name << ": no references for main";

      // Scope completion at the end of the buffer, and member completion
      // just after the first `.`.  Members are guaranteed only for class
      // receivers (structs.dao's first `.` reads a field); builtin
      // receivers offer none yet.
      auto end = static_cast<uint32_t>(example.source.size());
      call("completions", document_request(example.source, {{"offset", end}}));
      auto dots = sample_offsets(semantic_tokens, "operator.member", 1);
      if (!dots.empty()) {
        auto members =
            call("completions", document_request(example.source, {{"offset", dots.front() + 1}}));
        if (example.name == "structs.dao") {
          expect(!members.body.empty()) << example.name << ": no members offered after `.`";
        }
      }
    }
  };

  "positions_carry_file_identity"_test = [] {
    // hello.dao: `print` is declared in the prelude, `main` in the buffer.
    auto hello = call("example", {{"name", "hello.dao"}}).body["source"].get<std::string>();
    auto analysis = call("analyze", document_request(hello));
    expect(analysis.body["file"].get<std::string>() == kTestDocument);
    expect(analysis.body["module"].get<std::string>() == "hello")
        << "module: " << analysis.body["module"].dump();

    auto print_use = static_cast<uint32_t>(hello.find("print("));
    auto definition = call("gotoDef", document_request(hello, {{"offset", print_use}}));
    expect(!definition.body.is_null()) << "print has no definition";
    if (!definition.body.is_null()) {
      auto file = definition.body["file"].get<std::string>();
      expect(file.starts_with("stdlib/")) << "print defined in " << file;
    }

    auto main_decl = static_cast<uint32_t>(hello.find("fn main") + 3);
    auto refs = call("references", document_request(hello, {{"offset", main_decl}}));
    for (const auto& ref : refs.body) {
      expect(ref["file"].get<std::string>() == kTestDocument) << ref.dump();
    }

    // A scratch buffer takes the synthetic module identity.
    auto scratch = call("analyze", document_request("fn main(): i32\n  return 0\n"));
    expect(scratch.body["module"].get<std::string>() == "playground");
  };

  "generated_typescript_is_current"_test = [] {
    auto generated = repo_root() / "tools" / "playground" / "frontend" / "src" / "generated" /
                     "tooling_surface.ts";
    expect(std::filesystem::exists(generated)) << generated.string() << " is missing";
    expect(dao::read_file(generated) == render_service_typescript())
        << generated.string() << " is stale: run `task gen-tooling-surface`";
  };

  "member_completion_works_while_typing"_test = [] {
    // The buffer a user has at the moment they type `.`: it does not
    // parse.  Members of the receiver must still be offered.
    std::string typing = "module t\nclass P:\n  x: i32\n  y: i32\nfn main(): i32\n"
                         "  let p: P = P(1, 2)\n  return p.";
    auto members =
        call("completions",
             document_request(typing, {{"offset", static_cast<uint32_t>(typing.size())}}));
    std::vector<std::string> labels;
    for (const auto& item : members.body) {
      labels.push_back(item["label"].get<std::string>());
    }
    expect(std::ranges::contains(labels, "x") && std::ranges::contains(labels, "y"))
        << "offered: " << members.body.dump();
  };

  "positions_in_a_two_file_program_name_their_files"_test = [] {
    // The document calls into another file of the program; the reply
    // says where the definition is, in that file's own coordinates.
    const std::string lib = "module lib\n\nfn helper(): i32\n  return 41\n";
    // `import lib` binds `lib` locally (CONTRACT_MODULE_SYSTEM): a module
    // reaches another module's functions through that binding.
    const std::string main =
        "module app\nimport lib\n\nfn main(): i32\n  return lib::helper() + 1\n";
    json request = {{"files",
                     json::array({{{"path", "lib.dao"}, {"source", lib}},
                                  {{"path", kTestDocument}, {"source", main}}})},
                    {"document", kTestDocument}};

    auto call_site = static_cast<uint32_t>(main.find("lib::helper()") + 5);
    json position = request;
    position["offset"] = call_site;
    auto definition = call("gotoDef", position);
    expect(!definition.body.is_null()) << "helper has no definition";
    if (!definition.body.is_null()) {
      expect(definition.body["file"].get<std::string>() == "lib.dao") << definition.body.dump();
      expect(definition.body["line"].get<uint32_t>() == 3) << definition.body.dump();
    }

    auto symbols = call("documentSymbols", request);
    expect(!symbols.body.empty()) << "no symbols for the document";
    for (const auto& sym : symbols.body) {
      expect(sym["file"].get<std::string>() == kTestDocument) << sym.dump();
    }

    // A diagnostic in the other file names that file, and an error there
    // stops lowering and execution just as one in the document would.
    json broken = request;
    broken["files"][0]["source"] = "module lib\n\nfn helper(): i32\n  return true\n";
    auto analysis = call("analyze", broken);
    bool named = false;
    for (const auto& diag : analysis.body["diagnostics"]) {
      named = named || diag["file"].get<std::string>() == "lib.dao";
    }
    expect(named) << "no diagnostic names lib.dao: " << analysis.body["diagnostics"].dump();
    expect(analysis.body["hir"].get<std::string>().empty() &&
           analysis.body["mir"].get<std::string>().empty() &&
           analysis.body["llvm_ir"].get<std::string>().empty())
        << "lowered a program with an error in lib.dao";
    auto run = call("run", broken);
    expect(run.body["exit_code"].get<int>() == -1)
        << "ran a program with an error in lib.dao: " << run.body.dump();

    // A lexer or parser error in the other file stops lowering and
    // execution just the same.
    for (const char* bad_lib : {"module lib\n\nfn helper(): i32\n  return @\n",
                                "module lib\n\nfn helper(: i32\n  return 41\n"}) {
      json unlexable = request;
      unlexable["files"][0]["source"] = bad_lib;
      auto reply = call("analyze", unlexable);
      bool names_lib = false;
      for (const auto& diag : reply.body["diagnostics"]) {
        names_lib = names_lib || diag["file"].get<std::string>() == "lib.dao";
      }
      expect(names_lib) << reply.body["diagnostics"].dump();
      expect(reply.body["hir"].get<std::string>().empty() &&
             reply.body["mir"].get<std::string>().empty() &&
             reply.body["llvm_ir"].get<std::string>().empty())
          << "lowered past a lex/parse error in lib.dao: " << reply.body["diagnostics"].dump();
      expect(call("run", unlexable).body["exit_code"].get<int>() == -1)
          << "ran past a lex/parse error in lib.dao";
    }
  };

  "assembly_diagnostics_come_back_in_program_order"_test = [] {
    // The document imports a module whose file does not lex, so that
    // file yields no module and the graph reports the import missing
    // while the file itself reports where it broke.  Both are root
    // causes and both must survive, in the program's canonical order
    // (Task 31 §8.4: file id, then offset) — the document sorts first,
    // so its import error leads the other file's.
    json request = {{"files",
                     json::array({{{"path", "a_main.dao"},
                                   {"source",
                                    "module app\nimport lib\n\nfn main(): i32\n"
                                    "  return lib::helper()\n"}},
                                  {{"path", "z_lib.dao"},
                                   {"source", "module lib\n\nfn helper(): i32\n  return @\n"}}})},
                    {"document", "a_main.dao"}};

    for (const char* route : {"analyze", "run"}) {
      auto reply = call(route, request);
      expect(reports(reply.body, "imported module 'lib' not found"))
          << route << " lost the import error: " << reply.body["diagnostics"].dump();
      auto positions = located_positions(reply.body);
      expect(positions.size() >= 2_ul)
          << route << " lost a root cause: " << reply.body["diagnostics"].dump();
      expect(std::ranges::is_sorted(positions))
          << route
          << " reported diagnostics out of file/offset order: " << reply.body["diagnostics"].dump();
      bool names_document = false;
      bool names_other = false;
      for (const auto& [file, offset] : positions) {
        names_document = names_document || file == "a_main.dao";
        names_other = names_other || file == "z_lib.dao";
      }
      expect(names_document && names_other)
          << route << " dropped a file's diagnostics: " << reply.body["diagnostics"].dump();
    }
  };

  "capability_surfaces_exist"_test = [] {
    // `daoc` subcommands: the driver's command table plus `build`,
    // which it dispatches before the table.
    auto driver = dao::read_file(repo_root() / "compiler" / "driver" / "main.cpp");
    static const std::regex command_pattern(R"re(Command\{\.name = "([a-z-]+)")re");
    std::set<std::string> commands{"build"};
    for (std::sregex_iterator it(driver.begin(), driver.end(), command_pattern), last; it != last;
         ++it) {
      commands.insert((*it)[1].str());
    }
    static const std::regex lsp_method(R"re(textDocument/[A-Za-z]+(/[A-Za-z]+)*)re");
    std::set<std::string> lsp_methods;
    for (const auto& cap : kCapabilities) {
      for (const auto& entry : cap.analysis) {
        auto header = repo_root() / "compiler" / entry.header;
        expect(std::filesystem::exists(header)) << cap.name << " names missing " << entry.header;
        // `LlvmBackend::lower` is declared as `lower(` inside the class.
        auto symbol = std::string(entry.symbol.substr(entry.symbol.rfind(':') + 1));
        expect(std::filesystem::exists(header) &&
               dao::read_file(header).find(symbol + "(") != std::string::npos)
            << cap.name << ": " << entry.symbol << " is not declared in " << entry.header;
      }
      expect(cap.playground.empty() || find_route(cap.playground) != nullptr)
          << cap.name << " names unknown route " << cap.playground;
      expect(cap.cli.empty() || commands.contains(std::string(cap.cli)))
          << cap.name << " names unknown daoc command " << cap.cli;
      if (!cap.lsp.empty()) {
        expect(std::regex_match(std::string(cap.lsp), lsp_method))
            << cap.name << " has a malformed LSP method " << cap.lsp;
        expect(lsp_methods.insert(std::string(cap.lsp)).second)
            << cap.name << " repeats LSP method " << cap.lsp;
      }
    }
  };

  "every_diagnostic_producer_is_in_the_matrix"_test = [] {
    // A compiler header that declares a diagnostics vector is a phase that
    // can reject a program; the Diagnostics row must name it.
    std::set<std::string> listed;
    for (const auto& cap : kCapabilities) {
      if (cap.name != "Diagnostics") {
        continue;
      }
      for (const auto& entry : cap.analysis) {
        listed.insert(std::string(entry.header));
      }
    }
    auto compiler = repo_root() / "compiler";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(compiler)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".h") {
        continue;
      }
      if (dao::read_file(entry.path()).find("std::vector<Diagnostic> diagnostics") ==
          std::string::npos) {
        continue;
      }
      auto header = std::filesystem::relative(entry.path(), compiler).generic_string();
      expect(listed.contains(header))
          << header << " produces diagnostics but is not in the Diagnostics row";
    }
  };

  "capability_matrix_is_current"_test = [] {
    auto matrix = repo_root() / "docs" / "tooling_capabilities.md";
    expect(std::filesystem::exists(matrix)) << matrix.string() << " is missing";
    expect(dao::read_file(matrix) == render_capability_matrix())
        << matrix.string() << " is stale: run `task gen-tooling-surface`";
  };

  "examples_run_to_their_goldens"_test = [] {
    init_run_support();
    const bool update = std::getenv("DAO_UPDATE_GOLDENS") != nullptr;
    auto known_failures = load_known_failures();

    for (const auto& example : load_examples()) {
      auto reply = call("run", document_request(example.source));
      auto exit_code = reply.body["exit_code"].get<int>();
      auto stdout_text = reply.body["stdout"].get<std::string>();

      if (auto failure = known_failures.find(example.name); failure != known_failures.end()) {
        expect(exit_code == -1) << example.name << " now builds (exit " << exit_code
                                << "); remove it from known_failures.txt (" << failure->second
                                << ")";
        // The failure must be the recorded one, not a fresh regression
        // hiding behind the same exit code.
        expect(reports(reply.body, failure->second))
            << example.name << " failed for a different reason than known_failures.txt records ('"
            << failure->second << "'): " << reply.body["diagnostics"].dump();
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
