// The analysis surface must agree with the contract that freezes it and
// with what the emitter actually produces.  (The playground's service_test
// checks the generated TypeScript and the routes, which are transport.)

#include "analysis/semantic_tokens.h"
#include "analysis/tooling_surface.h"
#include "frontend/resolve/resolve.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>

#include <algorithm>
#include <filesystem>
#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace boost::ut;
using namespace dao;
using namespace dao::tooling;

namespace {

auto repo_root() -> std::filesystem::path {
  return DAO_SOURCE_DIR;
}

auto table_kinds() -> std::set<std::string> {
  std::set<std::string> kinds;
  for (const auto& spec : kTokenKinds) {
    kinds.emplace(spec.kind);
  }
  return kinds;
}

/// Token kinds named in backticks within the contract's taxonomy
/// sections (everything between the taxonomy heading and the next
/// top-level heading).
auto contract_kinds() -> std::set<std::string> {
  auto text = read_file(repo_root() / "docs" / "contracts" / "CONTRACT_LANGUAGE_TOOLING.md");
  auto begin = text.find("## Frozen Initial Semantic Token Taxonomy");
  auto end = text.find("\n## ", begin + 1);
  expect(begin != std::string::npos && end != std::string::npos)
      << "taxonomy section not found in CONTRACT_LANGUAGE_TOOLING.md";
  auto section = text.substr(begin, end - begin);

  // Kinds are dotted names; `punctuation` is the one single-word kind.
  static const std::regex kind_pattern(R"(`([a-z]+(?:\.[a-z]+)+|punctuation)`)");
  std::set<std::string> kinds;
  for (std::sregex_iterator it(section.begin(), section.end(), kind_pattern), last; it != last;
       ++it) {
    kinds.insert((*it)[1].str());
  }
  return kinds;
}

auto dao_files_in(const std::filesystem::path& dir) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".dao") {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  return files;
}

auto difference(const std::set<std::string>& lhs, const std::set<std::string>& rhs) -> std::string {
  std::string out;
  for (const auto& item : lhs) {
    if (!rhs.contains(item)) {
      out += item + " ";
    }
  }
  return out;
}

} // namespace

suite<"tooling_surface"> tooling_surface_suite = [] {
  "table_lists_exactly_the_contract_taxonomy"_test = [] {
    auto contract = contract_kinds();
    auto table = table_kinds();
    expect(contract == table) << "in contract but not table: " << difference(contract, table)
                              << "| in table but not contract: " << difference(table, contract);
  };

  "every_group_and_field_type_is_declared"_test = [] {
    for (const auto& spec : kTokenKinds) {
      expect(std::ranges::contains(kTokenGroups, spec.group))
          << spec.kind << " uses unknown group " << spec.group;
    }
    for (const auto& shape : kShapes) {
      for (const auto& field : shape.fields) {
        auto type = field.type;
        if (type.ends_with("[]")) {
          type.remove_suffix(2);
        }
        bool known = type == "string" || type == "number" || type == "boolean" ||
                     type == "TokenKind" || type == "LexicalCategory" ||
                     type == "DiagnosticSeverity" || find_shape(type) != nullptr;
        expect(known) << shape.name << "." << field.name << " has unknown type " << field.type;
      }
    }
  };

  "emitter_kinds_over_the_corpus_are_in_the_table"_test = [] {
    auto prelude = stdlib_prelude_sources(repo_root());
    auto table = table_kinds();
    std::set<std::string> unlisted;

    for (const auto& dir : {repo_root() / "examples", repo_root() / "spec" / "syntax_probes"}) {
      for (const auto& path : dao_files_in(dir)) {
        auto program = make_test_program(read_file(path), prelude);
        const auto& user = user_file(program);
        if (user.file() == nullptr) {
          continue; // probes that do not parse are not classification input
        }
        auto resolved = resolve(program);
        for (const auto& token : classify_tokens(user.lex.tokens, user.file(), &resolved)) {
          if (!table.contains(std::string(token.kind))) {
            unlisted.emplace(token.kind);
          }
        }
      }
    }
    expect(unlisted.empty()) << "emitted but not in the table: " << difference(unlisted, {});
  };
};

// Run the suites from main rather than from boost.ut's runner destructor
// after main returns, when this file's globals are already gone.
auto main(int argc, const char** argv) -> int {
  return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv}) ? 1 : 0;
}
