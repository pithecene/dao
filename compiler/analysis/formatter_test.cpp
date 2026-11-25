#include "analysis/formatter.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "support/test_utils.h"

#include <boost/ut.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace boost::ut;
using namespace dao;

namespace {

auto repo_root() -> std::filesystem::path {
  return DAO_SOURCE_DIR;
}

auto format_checked(std::string_view input,
                    const FormatOptions& options = {},
                    std::string_view name = "test.dao") -> std::string {
  SourceBuffer buf{std::string(name), std::string(input)};
  auto res = format_source(buf, options);
  expect(res.ok()) << name << ": format failed: "
                   << (res.errors.empty() ? "unknown" : res.errors.front().message);
  if (res.ok()) {
    auto problem = verify_format_invariants(buf, res.output, options);
    expect(problem.empty()) << name << ": invariant failed: " << problem << "\n--- formatted ---\n"
                            << res.output;
  }
  return res.output;
}

auto load_known_failures() -> std::set<std::string> {
  std::set<std::string> known;
  std::ifstream file(repo_root() / "testdata" / "examples" / "known_failures.txt");
  for (std::string line; std::getline(file, line);) {
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    auto tab = line.find('\t');
    known.insert(tab == std::string::npos ? line : line.substr(0, tab));
  }
  return known;
}

auto collect_dao_files(const std::filesystem::path& root) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> files;
  if (!std::filesystem::exists(root)) {
    return files;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".dao") {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);
  return files;
}

} // namespace

suite<"formatter"> formatter_suite = [] {
  "canonicalizes_module_imports_and_spacing"_test = [] {
    const std::string_view src = "module   app :: math  \n"
                                 "import   core :: fmt\n"
                                 "import net::http\n"
                                 "\n\n\n"
                                 "fn add<T : Show + Clone>( a : i32 , b : i32 ) : i32 -> a+b*2\n"
                                 "fn main():i32\n"
                                 "    let x:i32=add<i32>( 1 , 2 )   \n"
                                 "    \n"
                                 "    \n"
                                 "    return   -x\n";

    const std::string expected = "module app::math\n"
                                 "\n"
                                 "import core::fmt\n"
                                 "import net::http\n"
                                 "\n"
                                 "fn add<T: Show + Clone>(a: i32, b: i32): i32 -> a + b * 2\n"
                                 "\n"
                                 "fn main(): i32\n"
                                 "  let x: i32 = add<i32>(1, 2)\n"
                                 "\n"
                                 "  return -x\n";

    expect(format_checked(src) == expected);
  };

  "preserves_comments_in_every_position"_test = [] {
    const std::string_view src = "// File header line 1\n"
                                 "// File header line 2\n"
                                 "\n"
                                 "// Module comment\n"
                                 "module demo::comments // module trailing\n"
                                 "// Post-module comment\n"
                                 "\n"
                                 "// Before first import\n"
                                 "import a::b // import trailing\n"
                                 "// Between imports\n"
                                 "import c::d\n"
                                 "\n"
                                 "// Doc for class\n"
                                 "class Box<T>: // class header trailing\n"
                                 "  // Field comment\n"
                                 "  value: T // field trailing\n"
                                 "  // Method comment\n"
                                 "  fn get(self): T // method sig trailing\n"
                                 "    // Inside method\n"
                                 "    return self.value // return trailing\n"
                                 "    // End of method block\n"
                                 "  // End of class block\n"
                                 "\n"
                                 "// Enum class doc\n"
                                 "enum class Token:\n"
                                 "  // First variant\n"
                                 "  Int(\n"
                                 "    // payload field\n"
                                 "    value: i64 // field trailing\n"
                                 "  )\n"
                                 "  Plus // variant trailing\n"
                                 "  // End of enum\n"
                                 "\n"
                                 "fn check(x: i32, tok: Token): i32\n"
                                 "  if x > 0: // if trailing\n"
                                 "    // inside then\n"
                                 "    let y = x + 1\n"
                                 "    // end of then\n"
                                 "  // before else if\n"
                                 "  else if x == 0:\n"
                                 "    return 0\n"
                                 "  // before else\n"
                                 "  else: // else trailing\n"
                                 "    match tok: // match trailing\n"
                                 "      // before first arm\n"
                                 "      Token::Int(value): // arm trailing\n"
                                 "        return 1\n"
                                 "        // end of arm\n"
                                 "      Token::Plus:\n"
                                 "        return 2\n"
                                 "      // end of match\n"
                                 "  return 3\n"
                                 "\n"
                                 "// Trailing file comment\n";

    auto formatted = format_checked(src);
    expect(formatted.contains("// File header line 1\n// File header line 2\n\n// Module "
                              "comment\nmodule demo::comments // module trailing\n"))
        << formatted;
    expect(formatted.contains("    // end of then\n  // before else if\n  else if x == 0:\n"))
        << formatted;
    expect(formatted.contains("  // before else\n  else: // else trailing\n")) << formatted;
    expect(formatted.contains("// Trailing file comment\n")) << formatted;
  };

  "preserves_else_block_when_comment_sits_between_else_and_if"_test = [] {
    const std::string_view src = "module demo\n"
                                 "\n"
                                 "fn f(x: i32): i32\n"
                                 "  if x > 0:\n"
                                 "    return 1\n"
                                 "  else:\n"
                                 "    // Explain nested check\n"
                                 "    if x < 0:\n"
                                 "      return -1\n"
                                 "  return 0\n";

    auto formatted = format_checked(src);
    expect(formatted.contains("  else:\n    // Explain nested check\n    if x < 0:\n"))
        << formatted;
  };

  "formats_pointers_modes_resources_lambdas_and_match_patterns"_test = [] {
    const std::string_view src = "module demo\n"
                                 "enum class Expr:\n"
                                 "  Lit(value:i32)\n"
                                 "  Add(lhs:i32,rhs:i32)\n"
                                 "fn work(e:Expr):i32\n"
                                 "  let p:Ptr<i32> =Ptr<i32>::new()\n"
                                 "  let f = |x,y|->x+y\n"
                                 "  resource memory pool =>\n"
                                 "    mode unsafe =>\n"
                                 "      if !p.is_null():\n"
                                 "        p.set(f(1,2))\n"
                                 "  match e:\n"
                                 "    Expr::Lit(value):\n"
                                 "      return value\n"
                                 "    Expr::Add(lhs,..):\n"
                                 "      return lhs\n"
                                 "    Expr::Add as whole:\n"
                                 "      return 0\n";

    const std::string expected = "module demo\n"
                                 "\n"
                                 "enum class Expr:\n"
                                 "  Lit(value: i32)\n"
                                 "  Add(lhs: i32, rhs: i32)\n"
                                 "\n"
                                 "fn work(e: Expr): i32\n"
                                 "  let p: Ptr<i32> = Ptr<i32>::new()\n"
                                 "  let f = |x, y| -> x + y\n"
                                 "  resource memory pool =>\n"
                                 "    mode unsafe =>\n"
                                 "      if !p.is_null():\n"
                                 "        p.set(f(1, 2))\n"
                                 "  match e:\n"
                                 "    Expr::Lit(value):\n"
                                 "      return value\n"
                                 "    Expr::Add(lhs, ..):\n"
                                 "      return lhs\n"
                                 "    Expr::Add as whole:\n"
                                 "      return 0\n";

    expect(format_checked(src) == expected);
  };

  "wraps_long_signatures_variants_calls_and_pipelines_at_100_columns"_test = [] {
    const std::string_view src =
        "module demo::wrapping\n"
        "\n"
        "enum class Command:\n"
        "  Configure(endpoint_url: string, timeout_millis: i64, max_retries: i32, "
        "enable_compression: bool, certificate_bundle_path: string)\n"
        "\n"
        "fn compute_everything(first_parameter: i64, second_parameter: i64, third_parameter: i64, "
        "fourth_parameter: i64): i64\n"
        "  let cmd = Command::Configure(endpoint_url = \"https://example.invalid/service\", "
        "timeout_millis = 5000, max_retries = 3, enable_compression = true, "
        "certificate_bundle_path = \"/etc/ssl/cert.pem\")\n"
        "  let piped = first_parameter |> transform_alpha_stage |> transform_beta_stage |> "
        "transform_gamma_stage |> transform_delta_stage\n"
        "  return piped\n";

    auto formatted = format_checked(src);
    expect(formatted.find("  Configure(\n    endpoint_url: string,\n") != std::string::npos)
        << formatted;
    expect(formatted.find("fn compute_everything(\n  first_parameter: i64,\n") != std::string::npos)
        << formatted;
    expect(formatted.find("  let cmd = Command::Configure(\n    endpoint_url = ") !=
           std::string::npos)
        << formatted;
    expect(formatted.find("  let piped = first_parameter\n    |> transform_alpha_stage\n    |> "
                          "transform_beta_stage\n") != std::string::npos)
        << formatted;
  };

  "preserves_precedence_and_lambda_grouping_parentheses"_test = [] {
    const std::string_view src = "module demo::prec\n"
                                 "\n"
                                 "fn calc(a: i32, b: i32, c: i32, d: i32): i32\n"
                                 "  let p = (a + b) * (c - d)\n"
                                 "  let r = a - (b - c)\n"
                                 "  let g = ((a < b) > (c == d))\n"
                                 "  let l = (|x| -> x + 1)(a)\n"
                                 "  let m = (a |> double) + 1\n"
                                 "  return p + r + l + m\n";

    auto formatted = format_checked(src);
    expect(formatted.find("(a + b) * (c - d)") != std::string::npos) << formatted;
    expect(formatted.find("a - (b - c)") != std::string::npos) << formatted;
    expect(formatted.find("(a < b) > (c == d)") != std::string::npos) << formatted;
    expect(formatted.find("(|x| -> x + 1)(a)") != std::string::npos) << formatted;
    expect(formatted.find("(a |> double) + 1") != std::string::npos) << formatted;
  };

  "formats_concepts_conformances_denials_lists_try_and_pipe_comments"_test = [] {
    const std::string_view src = "module demo::full\n"
                                 "\n"
                                 "derived concept Printable:\n"
                                 "  fn to_string(self): string\n"
                                 "\n"
                                 "class Secret:\n"
                                 "  id: i32\n"
                                 "  deny Printable\n"
                                 "  as Show:\n"
                                 "    fn show(self): string -> \"secret\"\n"
                                 "\n"
                                 "extend i32 as Printable:\n"
                                 "  fn to_string(self): string -> \"int\"\n"
                                 "\n"
                                 "fn gen(xs: Vector<i32>): Generator<i32>\n"
                                 "  for item in xs.iter():\n"
                                 "    if item < 0:\n"
                                 "      break\n"
                                 "    yield item?\n"
                                 "  let items = [\n"
                                 "    // first item\n"
                                 "    1,\n"
                                 "    2\n"
                                 "  ]\n"
                                 "  let piped = xs\n"
                                 "    // keep positive\n"
                                 "    |> filter is_positive\n"
                                 "    |> map |v| -> v + 1\n"
                                 "  return\n";

    auto formatted = format_checked(src);
    expect(formatted.find("derived concept Printable:\n  fn to_string(self): string\n") !=
           std::string::npos)
        << formatted;
    expect(formatted.find(
               "  deny Printable\n\n  as Show:\n    fn show(self): string -> \"secret\"\n") !=
           std::string::npos)
        << formatted;
    expect(formatted.find("    yield item?\n") != std::string::npos) << formatted;
    expect(formatted.find("  let items = [\n    // first item\n    1,\n    2\n  ]\n") !=
           std::string::npos)
        << formatted;
    expect(formatted.find("  let piped = xs\n    // keep positive\n    |> filter is_positive\n    "
                          "|> map |v| -> v + 1\n") != std::string::npos)
        << formatted;
  };

  "refuses_to_format_broken_syntax_or_lex_errors"_test = [] {
    for (std::string_view bad : {
             std::string_view{"module bad\nfn broken(: i32\n  return 1\n"},
             std::string_view{"module bad\nfn main(): i32\n  return @\n"},
             std::string_view{"module bad\nfn main(): i32\n\treturn 0\n"},
             std::string_view{"fn missing_module(): i32 -> 1\n"},
         }) {
      SourceBuffer buf("bad.dao", std::string(bad));
      auto res = format_source(buf);
      expect(!res.ok()) << "expected refusal on broken input:\n" << bad;
      expect(res.output.empty()) << "broken input must produce empty output";
      expect(!res.errors.empty());
    }
  };

  "corpus_invariants_hold_over_every_stdlib_and_example_file"_test = [] {
    auto known_failures = load_known_failures();
    auto stdlib_files = collect_dao_files(repo_root() / "stdlib");
    auto example_files = collect_dao_files(repo_root() / "examples");

    expect(!stdlib_files.empty()) << "no stdlib .dao files found";
    expect(!example_files.empty()) << "no example .dao files found";

    size_t verified_count = 0;
    for (const auto* group : {&stdlib_files, &example_files}) {
      for (const auto& path : *group) {
        auto rel = std::filesystem::relative(path, repo_root()).generic_string();
        auto filename = path.filename().string();
        std::string contents = read_file(path);
        SourceBuffer buf(rel, contents);

        // Skip only files listed in known_failures.txt that genuinely do not parse.
        if (known_failures.contains(filename)) {
          auto lex_res = lex(buf);
          auto parse_res = parse(lex_res.tokens);
          if (!lex_res.diagnostics.empty() || !parse_res.diagnostics.empty() ||
              parse_res.file == nullptr) {
            continue;
          }
        }

        auto res = format_source(buf);
        expect(res.ok()) << rel << ": format_source failed: "
                         << (res.errors.empty() ? "unknown" : res.errors.front().message);
        if (!res.ok()) {
          continue;
        }

        auto problem = verify_format_invariants(buf, res.output);
        expect(problem.empty()) << rel << ": " << problem;
        ++verified_count;
      }
    }
    expect(verified_count >= stdlib_files.size() + example_files.size() - known_failures.size());
  };

  "corpus_invariants_hold_over_parseable_bootstrap_and_probe_files"_test = [] {
    auto bootstrap_files = collect_dao_files(repo_root() / "bootstrap");
    auto probe_files = collect_dao_files(repo_root() / "spec" / "syntax_probes");
    size_t verified_count = 0;

    for (const auto* group : {&bootstrap_files, &probe_files}) {
      for (const auto& path : *group) {
        auto rel = std::filesystem::relative(path, repo_root()).generic_string();
        std::string contents = read_file(path);
        SourceBuffer buf(rel, contents);
        auto lex_res = lex(buf);
        auto parse_res = parse(lex_res.tokens);
        if (!lex_res.diagnostics.empty() || !parse_res.diagnostics.empty() ||
            parse_res.file == nullptr) {
          // Subsystem fragments that lack `module` before `assemble.sh` or
          // negative syntax probes are legitimately unparseable standalone.
          continue;
        }
        auto res = format_source(buf);
        expect(res.ok()) << rel << ": format_source failed: "
                         << (res.errors.empty() ? "unknown" : res.errors.front().message);
        if (res.ok()) {
          auto problem = verify_format_invariants(buf, res.output);
          expect(problem.empty()) << rel << ": " << problem;
          ++verified_count;
        }
      }
    }
    expect(verified_count > 0_u);
  };
};

auto main(int argc, const char** argv) -> int {
  return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv}) ? 1 : 0;
}
