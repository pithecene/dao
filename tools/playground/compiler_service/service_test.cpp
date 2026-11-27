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

/// The editor's offset of a byte offset in `text`: UTF-16 code units,
/// by a count that shares nothing with the service's.
auto editor_offset(std::string_view text, size_t bytes) -> uint32_t {
  uint32_t units = 0;
  for (size_t i = 0; i < bytes; ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    units += (byte & 0xC0U) != 0x80U ? 1U : 0U; // every byte but a continuation
    units += byte >= 0xF0U ? 1U : 0U;           // the second half of a surrogate pair
  }
  return units;
}

/// The byte offset in `text` of an editor offset: the inverse of
/// editor_offset, for reading back the text a reply's range covers.
auto byte_offset(std::string_view text, uint32_t units) -> size_t {
  size_t bytes = 0;
  while (bytes < text.size() && editor_offset(text, bytes) < units) {
    ++bytes;
  }
  return bytes;
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
    if (entry["isMultifile"].get<bool>()) {
      continue;
    }
    auto name = entry["name"].get<std::string>();
    auto source = call("example", {{"name", name}});
    expect(source.body["files"].size() == 1_u) << name << " files: " << source.body.dump();
    expect(source.body["document"].get<std::string>() == name) << source.body.dump();
    examples.push_back({.name = name, .source = source.body["source"].get<std::string>()});
  }
  return examples;
}

/// One multi-file example served through `examples` (`isMultifile: true`)
/// and `example`: every `.dao` file of a directory under `examples/`
/// whose files declare a single `fn main` between them.
struct MultifileExample {
  std::string name; // the directory's name
  json files = json::array();
  std::string document; // the file declaring `fn main`
};

auto load_multifile_examples() -> std::vector<MultifileExample> {
  std::vector<MultifileExample> examples;
  auto list = call("examples", json::object());
  for (const auto& entry : list.body["examples"]) {
    if (!entry["isMultifile"].get<bool>()) {
      continue;
    }
    auto name = entry["name"].get<std::string>();
    auto loaded = call("example", {{"name", name}});
    expect(!loaded.body["files"].empty()) << name << " returned no files";
    examples.push_back({.name = name,
                        .files = loaded.body["files"],
                        .document = loaded.body["document"].get<std::string>()});
  }
  expect(!examples.empty()) << "expected at least one multi-file example (modules/)";
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
  if (shape_name == "PreludeRequest") {
    return {{"path", "stdlib/core/option.dao"}};
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

/// Every lexical token of the analysed document has a semantic token at
/// its offset -- the single-file corpus's bar, applied to every document
/// of a multi-file example too.
void expect_every_token_classified(const std::string& name, const json& body) {
  std::set<uint32_t> classified;
  for (const auto& token : body["semanticTokens"]) {
    classified.insert(token["offset"].get<uint32_t>());
  }
  expect(!body["tokens"].empty()) << name << ": the document produced no tokens";
  for (const auto& token : body["tokens"]) {
    auto offset = token["offset"].get<uint32_t>();
    expect(classified.contains(offset))
        << name << ": no semantic token for " << token["text"].get<std::string>() << " ("
        << token["kind"].get<std::string>() << ") at line " << token["line"].get<uint32_t>();
  }
}

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

  "filtered llvm keeps every function the document defines"_test = [] {
    // The backend names a module's functions `<module>::<name>` and
    // leaves only the entry module's `main` bare, so a filter built from
    // MIR symbol names kept `main` alone and emitted a call to a
    // definition it had dropped.
    const std::string source = "module app\n"
                               "fn helper(): i32\n  return 41\n"
                               "fn main(): i32\n  return helper() + 1\n";
    auto reply = call("analyze", document_request(source));
    auto ir = reply.body["llvm_ir"].get<std::string>();
    expect(!ir.empty()) << reply.body["diagnostics"].dump();

    // Names introduced by `define` / `declare`, and names called.
    auto names_after = [&ir](std::string_view keyword) -> std::set<std::string> {
      std::set<std::string> found;
      const std::regex pattern(std::string(keyword) + R"re(\s[^@\n]*@"?([A-Za-z0-9_:.$]+)"?\()re");
      for (std::sregex_iterator it(ir.begin(), ir.end(), pattern), last; it != last; ++it) {
        found.insert((*it)[1].str());
      }
      return found;
    };
    auto defined = names_after("define");
    auto declared = names_after("declare");
    auto called = names_after("call");

    expect(defined.contains("app::helper"))
        << "the document's own helper must be defined in the filtered IR:\n"
        << ir;
    expect(defined.contains("main")) << "the entry keeps its bare name:\n" << ir;
    for (const auto& callee : called) {
      if (callee.starts_with("llvm.")) {
        continue; // intrinsics need no declaration of ours
      }
      expect(defined.contains(callee) || declared.contains(callee))
          << "call to " << callee << " which the filter dropped:\n"
          << ir;
    }
  };

  "filtered llvm keeps a definition the document calls in another file"_test = [] {
    // The IR views hide the prelude, not the rest of the program: a
    // document calling into a sibling file must not be shown a call with
    // no definition.
    const std::string lib = "module lib\n\nfn helper(): i32\n  return 41\n";
    const std::string main =
        "module app\nimport lib\n\nfn main(): i32\n  return lib::helper() + 1\n";
    json request = {{"files",
                     json::array({{{"path", "lib.dao"}, {"source", lib}},
                                  {{"path", kTestDocument}, {"source", main}}})},
                    {"document", kTestDocument}};
    auto reply = call("analyze", request);
    auto ir = reply.body["llvm_ir"].get<std::string>();
    expect(!ir.empty()) << reply.body["diagnostics"].dump();
    expect(ir.find("lib::helper") != std::string::npos)
        << "the callee's definition must survive the filter:\n"
        << ir;
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

  "a program that stops at assembly still classifies the document"_test = [] {
    // The editor colours a file from this reply alone, so a reply with no
    // tokens leaves the file plain.  Each program here stops at assembly,
    // and the document must still come back classified.
    const std::string uses_helper =
        "module app\nimport helper\n\nfn main(): i32\n  return helper::one()\n";
    const std::string helper = "module helper\n\nfn one(): i32 -> 1\n";
    /// The text each kind covers in the document, read back through the
    /// reply's own offsets: the editor places colour by those, and drops a
    /// span that falls outside the document.
    auto classified = [&](const std::string& document, const std::string& other) {
      auto reply = call("analyze",
                        json{{"files",
                              json::array({{{"path", kTestDocument}, {"source", document}},
                                           {{"path", "helper.dao"}, {"source", other}}})},
                             {"document", kTestDocument}});
      expect(std::ranges::any_of(reply.body["diagnostics"],
                                 [](const json& diag) { return diag["severity"] == "error"; }))
          << "the program must stop at assembly: " << reply.body["diagnostics"].dump();
      expect(!reply.body["tokens"].empty()) << "no lexical tokens: " << reply.body.dump();
      // What the service sends: a reply that does not serialize never
      // reaches the editor.
      expect(nothrow([&] { (void)reply.body.dump(); })) << "the reply does not serialize";
      std::multimap<std::string, std::string> text_by_kind;
      for (const auto& token : reply.body["semanticTokens"]) {
        auto offset = token["offset"].get<uint32_t>();
        auto length = token["length"].get<uint32_t>();
        expect(offset + length <= editor_offset(document, document.size()))
            << "a span outside the document";
        auto first = byte_offset(document, offset);
        auto past = byte_offset(document, offset + length);
        text_by_kind.emplace(token["kind"].get<std::string>(),
                             document.substr(first, past - first));
      }
      return text_by_kind;
    };
    auto covers = [](const std::multimap<std::string, std::string>& text_by_kind,
                     const std::string& kind, const std::string& text) {
      auto [first, last] = text_by_kind.equal_range(kind);
      return std::ranges::any_of(first, last,
                                 [&](const auto& entry) { return entry.second == text; });
    };

    // The document's own lexer error: no syntax tree, so the lexical
    // kinds, on both sides of the error.
    auto unlexable =
        classified("module app\n\nfn main(): i32\n  print(\"oops)\n  return 0\n", helper);
    expect(covers(unlexable, "keyword.fn", "fn")) << "nothing before the lexer error is classified";
    expect(covers(unlexable, "keyword.return", "return"))
        << "nothing after the lexer error is classified";

    // A character outside ASCII where none belongs, as a pasted curly
    // quote is.
    auto curly_quote =
        classified("module app\n\nfn main(): i32\n  let x = \xE2\x80\x9C\n  return 0\n", helper);
    expect(covers(curly_quote, "keyword.return", "return"))
        << "nothing after the curly quote is classified";

    // A lexer error in a module the document imports, and a module
    // declared twice: the document parsed, so its declarations classify
    // from its syntax tree.
    auto beside_unlexable = classified(uses_helper, "module helper\n\nfn one(): i32 -> @\n");
    expect(covers(beside_unlexable, "decl.function", "main"))
        << "an imported module's lexer error hid the document's declarations";
    auto declared_twice = classified(uses_helper, "module app\n\nfn one(): i32 -> 1\n");
    expect(covers(declared_twice, "decl.function", "main"))
        << "a module declared twice hid the document's declarations";
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
      if (route.request == "void" || route.request == "ExampleName" ||
          route.request == "PreludeRequest") {
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

  "positions cross the boundary in the editor's unit"_test = [] {
    // The editor counts UTF-16 code units and the compiler counts bytes.
    // "é" is two bytes and one unit, and the emoji four bytes and two
    // units, so past the comment the two counts differ by three.
    const std::string source = "module app\n\n// caf\xC3\xA9 \xF0\x9F\x98\x80\n"
                               "fn one(): i32 -> 1\n\n"
                               "fn main(): i32\n  print(\"h\xC3\xA9\")\n  return one()\n";
    auto units_of = [&](size_t bytes) -> uint32_t { return editor_offset(source, bytes); };
    auto units_before = [&](std::string_view text) -> uint32_t {
      auto bytes = source.find(text);
      expect(fatal(bytes != std::string::npos)) << text;
      return units_of(bytes);
    };
    expect(units_of(source.size()) + 4U == source.size())
        << "the fixture must hold wide characters";

    // Replies: a token's offset and length.
    auto analyzed = call("analyze", document_request(source)).body;
    auto token_at = [&](uint32_t offset) -> json {
      for (const auto& token : analyzed["semanticTokens"]) {
        if (token["offset"].get<uint32_t>() == offset) {
          return token;
        }
      }
      return json::object();
    };
    auto declared = token_at(units_before("main"));
    expect(declared.value("kind", "") == "decl.function") << analyzed["semanticTokens"].dump();
    expect(declared.value("length", 0U) == 4U);
    expect(token_at(units_before("\"h")).value("length", 0U) == 4U)
        << "a string literal of five bytes is four units";

    // Requests: an offset in the editor's unit finds the token there, and
    // the reply names the declaration in the same unit.
    const auto declaration = units_before("one(): i32");
    const auto use = units_before("one()\n");
    auto definition = call("gotoDef", document_request(source, {{"offset", use}})).body;
    expect(definition.value("offset", 0U) == declaration) << definition.dump();
    expect(definition.value("length", 0U) == 3U) << definition.dump();
    std::set<uint32_t> referenced;
    for (const auto& ref : call("references", document_request(source, {{"offset", use}})).body) {
      referenced.insert(ref["offset"].get<uint32_t>());
      expect(ref["length"] == 3U) << ref.dump();
    }
    expect(referenced == std::set<uint32_t>{declaration, use});
    bool outlined = false;
    for (const auto& symbol : call("documentSymbols", document_request(source)).body) {
      if (symbol["name"] == "main") {
        outlined = true;
        // The symbol spans its declaration, which holds the wide string
        // and runs to the end of the source.
        const auto start = units_before("fn main");
        expect(symbol["offset"] == start) << symbol.dump();
        expect(symbol["length"] == units_of(source.size()) - start) << symbol.dump();
      }
    }
    expect(outlined) << "the outline must list main";

    // The document's length is counted in the same unit: its end is a
    // position, and its byte length, which is past the end, is not one.
    // Between the emoji's two units is the emoji's start: the request's
    // offset becomes the program offset of the emoji's first byte, not of
    // a byte inside it.
    const auto emoji = units_before("\xF0\x9F");
    auto inputs = parse_program_request(document_request(source));
    expect(fatal(inputs.has_value()));
    auto prog = build_playground_program(service_context().repo_root, std::move(*inputs));
    expect(fatal(prog.user != nullptr));
    const auto emoji_byte = prog.user->base_offset + source.find("\xF0\x9F");
    expect(prog.to_program_offset(emoji) == emoji_byte);
    expect(prog.to_program_offset(emoji + 1) == emoji_byte);
    expect(prog.to_program_offset(emoji + 2) == emoji_byte + 4);
    expect(call("hover", document_request(source, {{"offset", emoji + 1}})).body.is_null());
    const auto end = units_of(source.size());
    expect(dispatch("hover", document_request(source, {{"offset", end}}), service_context())
               .status == http_status::ok);
    expect(dispatch("hover", document_request(source, {{"offset", source.size()}}),
                    service_context())
               .status == http_status::bad_request);
  };

  "a position inside a character is the character's start"_test = [] {
    // A curly quote is three bytes and one unit.  A range over part of it
    // covers all of it, wherever the range starts.
    const Utf16Index quote("\xE2\x80\x9Cx");
    for (uint32_t byte = 0; byte < 3; ++byte) {
      const dao::Span marked = quote.to_utf16(dao::Span{.offset = byte, .length = 1});
      expect(marked.offset == 0U && marked.length == 1U) << byte;
      expect(quote.to_utf16(byte) == 0U) << byte;
    }
    expect(quote.to_utf16(3U) == 1U);
    expect(quote.to_utf16(dao::Span{.offset = 1, .length = 0}).length == 0U);
    expect(quote.to_utf16(dao::Span{.offset = 3, .length = 9}).length == 1U)
        << "held inside the text";

    // An emoji is four bytes and a surrogate pair.  The offset between
    // the pair's halves is the emoji's start.
    const Utf16Index emoji("a\xF0\x9F\x98\x80z");
    expect(emoji.to_bytes(1U) == 1U);
    expect(emoji.to_bytes(2U) == 1U);
    expect(emoji.to_bytes(3U) == 5U);
    expect(emoji.to_utf16(5U) == 3U);

    // A lead byte with no continuation is one unit and takes nothing
    // after it with it.
    const Utf16Index malformed("\xE2"
                               "ab");
    expect(malformed.to_utf16(1U) == 1U);
    expect(malformed.to_utf16(3U) == 3U);
  };

  "a character outside ASCII in rejected syntax is marked whole"_test = [] {
    // A curly quote outside a string is three bytes and one unit.  The
    // editor receives the reply as text, so the reply is read back from
    // its serialized form: one error, over the quote and nothing else.
    const std::string source =
        "module app\n\nfn main(): i32\n  let x = \xE2\x80\x9C\n  return 0\n";
    const auto quote = editor_offset(source, source.find("\xE2"));
    auto sent = json::parse(call("analyze", document_request(source)).body.dump());
    size_t marked = 0;
    for (const auto& diag : sent["diagnostics"]) {
      if (diag["severity"] == "error" && diag["offset"] == quote) {
        ++marked;
        expect(diag["length"] == 1U) << diag.dump();
        expect(diag["col"] == 11U) << diag.dump();
      }
    }
    expect(marked == 1U) << sent["diagnostics"].dump();
  };

  "a column counts the editor's unit along its line"_test = [] {
    // Wide characters on earlier lines do not move a column; one on the
    // same line does.
    const std::string source =
        "module app\n\n// caf\xC3\xA9\nfn main(): i32\n"
        "  print(\"h\xC3\xA9\" + missing)\n  return 0\n";
    const auto at = source.find("missing");
    const auto line_start = source.rfind('\n', at) + 1;
    const auto column = 1U + editor_offset(source, at) - editor_offset(source, line_start);
    expect(column + 1U == at - line_start + 1U) << "the fixture must differ by one on this line";
    bool found = false;
    for (const auto& diag : call("analyze", document_request(source)).body["diagnostics"]) {
      if (diag["offset"] == editor_offset(source, at)) {
        found = true;
        expect(diag["col"] == column) << diag.dump();
      }
    }
    expect(found) << "no diagnostic at the unknown name";
  };

  "positions in another file use that file's own count"_test = [] {
    // The two files differ in how far bytes and units have drifted: by one
    // in the document, by four in the library.
    const std::string lib =
        "module lib\n\n// \xF0\x9F\x98\x80 \xF0\x9F\x98\x80\nfn helper(): i32\n  return 41\n";
    const std::string main =
        "module app\nimport lib\n\n// caf\xC3\xA9\nfn main(): i32\n  return lib::helper() + 1\n";
    json request = {{"files",
                     json::array({{{"path", "lib.dao"}, {"source", lib}},
                                  {{"path", kTestDocument}, {"source", main}}})},
                    {"document", kTestDocument}};
    const auto use = editor_offset(main, main.find("helper()"));
    const auto declaration = editor_offset(lib, lib.find("helper()"));
    request["offset"] = use;
    auto definition = call("gotoDef", request).body;
    expect(definition.value("file", "") == "lib.dao") << definition.dump();
    expect(definition.value("offset", 0U) == declaration) << definition.dump();
    std::set<std::pair<std::string, uint32_t>> referenced;
    for (const auto& ref : call("references", request).body) {
      referenced.emplace(ref["file"].get<std::string>(), ref["offset"].get<uint32_t>());
    }
    expect(referenced == std::set<std::pair<std::string, uint32_t>>{
                             {"lib.dao", declaration}, {std::string(kTestDocument), use}});
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
      expect_every_token_classified(example.name, body);

      if (!known_failures.contains(example.name)) {
        expect(body["diagnostics"].empty())
            << example.name << " has diagnostics: " << body["diagnostics"].dump();
        expect(!body["llvm_ir"].get<std::string>().empty())
            << example.name << " produced no LLVM IR";
      }
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
               document_request(example.source,
                                {{"offset",
                                  editor_offset(example.source,
                                                example.source.find("fn main") + 3)}}));
      expect(!main_refs.body.empty()) << example.name << ": no references for main";

      // Scope completion at the end of the buffer, and member completion
      // just after the first `.`.  Members are guaranteed only for class
      // receivers (structs.dao's first `.` reads a field); builtin
      // receivers offer none yet.
      auto end = editor_offset(example.source, example.source.size());
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

  "format_route_formats_modules_scratch_buffers_and_preserves_broken_code"_test = [] {
    auto mod_reply =
        call("format", document_request("module app\n// comment\nfn main():i32->1+2\n"));
    expect(mod_reply.body["changed"].get<bool>());
    expect(mod_reply.body["diagnostics"].empty());
    expect(mod_reply.body["formatted"].get<std::string>() ==
           "module app\n\n// comment\nfn main(): i32 -> 1 + 2\n");

    // Second pass is unchanged.
    auto second = call("format", document_request(mod_reply.body["formatted"].get<std::string>()));
    expect(!second.body["changed"].get<bool>());

    // Scratch buffer without `module` keeps its moduleless form.
    auto scratch_reply =
        call("format", document_request("// scratch\nfn main():i32\n    return 0"));
    expect(scratch_reply.body["changed"].get<bool>());
    expect(scratch_reply.body["formatted"].get<std::string>() ==
           "// scratch\nfn main(): i32\n  return 0\n");

    // Broken input returns original source unchanged + located diagnostics.
    const std::string broken = "fn broken(: i32\n  return 0\n";
    auto bad_reply = call("format", document_request(broken));
    expect(!bad_reply.body["changed"].get<bool>());
    expect(bad_reply.body["formatted"].get<std::string>() == broken);
    expect(!bad_reply.body["diagnostics"].empty());
    expect(bad_reply.body["diagnostics"][0]["line"].get<uint32_t>() == 1);
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

  "a request file under stdlib is refused"_test = [] {
    // A request file whose path collides with a prelude file's would be
    // ranked as the prelude's when diagnostics are ordered.  It is not
    // a user file the playground can take.
    json files = json::array({
        {{"path", "a.dao"}, {"source", "module a\nfn broken(): i32\n  return @\n"}},
        {{"path", "stdlib/core/builtins.dao"},
         {"source", "module x\nfn also(): i32\n  return @\n"}},
    });
    for (const auto& route : {"analyze", "run"}) {
      // `call` insists on a 200; this reply is meant to be a 400.
      auto reply =
          dispatch(route, json{{"files", files}, {"document", "a.dao"}}, service_context());
      expect(reply.status == http_status::bad_request)
          << route << " accepted a request file under the prelude roots: " << reply.body.dump();
    }
    // Only the prelude roots are reserved: `stdlib/concepts/` is not the
    // prelude (§7.1) and is an ordinary request file.
    json outside = json::array({
        {{"path", "stdlib/concepts/mine.dao"},
         {"source", "module concepts::mine\nfn f(): i32 -> 1\n"}},
        {{"path", kTestDocument}, {"source", "module app\nfn main(): i32 -> 0\n"}},
    });
    auto ok = dispatch(
        "analyze", json{{"files", outside}, {"document", kTestDocument}}, service_context());
    expect(ok.status == http_status::ok)
        << "a non-prelude stdlib path was refused: " << ok.body.dump();
  };

  "diagnostics_come_back_in_program_order"_test = [] {
    // Phases are collected one after another, so without a final sort a
    // resolve error in the first file follows a parse error in the
    // second.  Program order is by file, then by offset within it
    // (CONTRACT_MODULE_SYSTEM.md §8.4).
    json files = json::array({
        {{"path", "a.dao"}, {"source", "module a\nfn uses_missing(): i32 -> nowhere()\n"}},
        {{"path", "b.dao"}, {"source", "module b\nfn broken(): i32\n  return @\n"}},
    });
    for (const auto& route : {"analyze", "run"}) {
      auto reply = call(route, json{{"files", files}, {"document", "a.dao"}});
      std::vector<std::pair<std::string, uint32_t>> seen;
      for (const auto& diag : reply.body["diagnostics"]) {
        seen.emplace_back(diag.value("file", std::string{}), diag.value("offset", 0U));
      }
      expect(seen.size() > 1_ul) << route << ": expected diagnostics from both files";
      expect(std::ranges::is_sorted(seen))
          << route << " diagnostics are out of program order: " << reply.body["diagnostics"].dump();
    }
  };

  "multifile_examples_analyze_and_run_as_one_program"_test = [] {
    // A directory of examples is one program: every file analyzes with
    // its own diagnostics, and the set builds and runs together.
    init_run_support();
    const bool update = std::getenv("DAO_UPDATE_GOLDENS") != nullptr;

    for (const auto& example : load_multifile_examples()) {
      for (const auto& file : example.files) {
        json request = {{"files", example.files}, {"document", file["path"]}};
        auto analyzed = call("analyze", request);
        expect(analyzed.status == http_status::ok) << example.name << ": analyze failed";
        for (const auto& diag : analyzed.body["diagnostics"]) {
          expect(diag["severity"] != "error")
              << example.name << "/" << file["path"] << ": " << diag["message"];
        }
        expect_every_token_classified(example.name + "/" + file["path"].get<std::string>(),
                                      analyzed.body);
      }

      auto reply = call("run", json{{"files", example.files}, {"document", example.document}});
      auto exit_code = reply.body["exit_code"].get<int>();
      expect(exit_code == 0) << example.name << " exited " << exit_code << ": "
                             << reply.body["stderr"].get<std::string>()
                             << reply.body["diagnostics"].dump();

      auto stdout_text = reply.body["stdout"].get<std::string>();
      auto golden_path = golden_dir() / (example.name + ".out");
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

  "version_reports_compiler_and_prelude_inventory"_test = [] {
    auto reply = call("version", json::object());
    expect(!reply.body["commit"].get<std::string>().empty());
    expect(!reply.body["llvmVersion"].get<std::string>().empty());
    expect(!reply.body["targetTriple"].get<std::string>().empty());
    const auto& prelude = reply.body["preludeFiles"];
    expect(!prelude.empty()) << "preludeFiles must list stdlib prelude modules";
    bool has_option = false;
    bool has_vector = false;
    for (const auto& entry : prelude) {
      auto path = entry.get<std::string>();
      expect(path.starts_with("stdlib/core/") || path.starts_with("stdlib/io/"))
          << "unexpected prelude path: " << path;
      has_option = has_option || path == "stdlib/core/option.dao";
      has_vector = has_vector || path == "stdlib/core/vector.dao";
    }
    expect(has_option && has_vector) << prelude.dump();
  };

  "prelude_serves_classified_readonly_stdlib_files"_test = [] {
    auto hello = call("example", {{"name", "hello.dao"}}).body["source"].get<std::string>();
    auto print_use = static_cast<uint32_t>(hello.find("print("));
    auto def = call("gotoDef", document_request(hello, {{"offset", print_use}}));
    expect(!def.body.is_null());
    auto target_path = def.body["file"].get<std::string>();

    auto prelude = call("prelude", {{"path", target_path}});
    expect(prelude.body["file"].get<std::string>() == target_path);
    expect(!prelude.body["module"].get<std::string>().empty());
    expect(!prelude.body["source"].get<std::string>().empty());
    expect(!prelude.body["semanticTokens"].empty());
    expect(!prelude.body["symbols"].empty());

    expect(dispatch("prelude", {{"path", "../CMakeLists.txt"}}, service_context()).status ==
           http_status::bad_request);
    expect(dispatch("prelude", {{"path", "examples/hello.dao"}}, service_context()).status ==
           http_status::bad_request);
    expect(dispatch("prelude", {{"path", "stdlib/core/does_not_exist.dao"}}, service_context())
               .status == http_status::not_found);
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
