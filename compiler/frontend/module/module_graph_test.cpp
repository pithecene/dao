// Module graph construction: registration, edges, order,
// cycles, entry selection, and root-file discovery.  Mirrors the
// bootstrap graph tests and adds the host-only path/declaration
// mismatch.

#include "frontend/module/module_graph.h"
#include "frontend/module/program.h"

#include <boost/ut.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace boost::ut;
using namespace dao;

namespace {

using NamedSource = std::pair<std::string, std::string>; // display path, text

auto program_of(std::vector<NamedSource> sources,
                std::optional<std::string> entry = {},
                EntryPolicy policy = EntryPolicy::Optional) -> Program {
  std::vector<SourceInput> inputs;
  for (auto& [display, text] : sources) {
    inputs.push_back({.display_path = display, .text = text, .is_prelude = false});
  }
  return build_program(std::move(inputs), std::move(entry), policy);
}

auto displays(const std::vector<ModuleInfo*>& modules) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto* module : modules) {
    out.push_back(module->display);
  }
  return out;
}

auto messages(const Program& program) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& diag : program.diagnostics) {
    out.push_back(diag.message);
  }
  return out;
}

auto joined(const std::vector<std::string>& items) -> std::string {
  std::string out;
  for (const auto& item : items) {
    out += item + " | ";
  }
  return out;
}

auto fixtures() -> std::filesystem::path {
  return std::filesystem::path(DAO_SOURCE_DIR) / "testdata" / "module";
}

const std::string kMain = "fn main(): i32\n  return 0\n";

} // namespace

suite<"module_graph"> module_graph_suite = [] {
  "two_files_one_import"_test = [] {
    auto program = program_of({{"a.dao", "module a\nimport b\n" + kMain}, {"b.dao", "module b\n"}});
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(program.modules.size() == 2_u);
    auto* a = program.module_named("a");
    expect(a != nullptr && a->imports.size() == 1_u && a->imports[0]->display == "b");
    expect(displays(program.topo_order) == std::vector<std::string>{"b", "a"});
    expect(program.entry == a) << "entry is the unique module declaring main";
    expect(program.files[0]->module == a) << "files carry their module";
  };

  "three_module_chain_orders_imports_first"_test = [] {
    auto program = program_of({{"a.dao", "module a\nimport b\n" + kMain},
                               {"b.dao", "module b\nimport c\n"},
                               {"c.dao", "module c\n"}});
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(displays(program.topo_order) == std::vector<std::string>{"c", "b", "a"});
  };

  "independent_modules_order_lexically"_test = [] {
    auto program = program_of({{"z.dao", "module zeta\n"},
                               {"a.dao", "module alpha\n"},
                               {"m.dao", "module mid\n" + kMain}});
    expect(displays(program.topo_order) == std::vector<std::string>{"alpha", "mid", "zeta"});
  };

  "missing_module_is_diagnosed_and_the_rest_still_builds"_test = [] {
    auto program = program_of({{"a.dao", "module a\nimport nowhere::x\n" + kMain}});
    expect(program.diagnostics.size() == 1_u) << joined(messages(program));
    expect(messages(program)[0] == "imported module 'nowhere::x' not found in the program");
    expect(program.module_named("a")->imports.empty());
    expect(program.entry == program.module_named("a"));
  };

  "duplicate_module_is_diagnosed_where_the_second_file_declares_it"_test = [] {
    auto program = program_of({{"one.dao", "module dup\n"}, {"two.dao", "module dup\n"}});
    expect(program.modules.size() == 1_u);
    expect(program.diagnostics.size() == 1_u) << joined(messages(program));
    const auto& diag = program.diagnostics[0];
    expect(diag.message == "module 'dup' is already declared by one.dao");
    expect(program.source_map.file_for(diag.span.offset)->display_path == "two.dao")
        << "located in the file that repeats the declaration";
    expect(program.files[1]->module == nullptr) << "the repeated file has no module";
  };

  "self_import_is_diagnosed"_test = [] {
    auto program = program_of({{"a.dao", "module a\nimport a\n"}});
    expect(messages(program) == std::vector<std::string>{"module 'a' imports itself"});
    expect(program.module_named("a")->imports.empty());
    expect(displays(program.topo_order) == std::vector<std::string>{"a"});
  };

  "two_node_cycle_is_one_diagnostic_with_its_trace"_test = [] {
    auto program =
        program_of({{"a.dao", "module a\nimport b\n"}, {"b.dao", "module b\nimport a\n"}});
    expect(messages(program) == std::vector<std::string>{"import cycle: a -> b -> a"})
        << joined(messages(program));
    expect(program.source_map.file_for(program.diagnostics[0].span.offset)->display_path == "a.dao")
        << "reported at the import that starts the trace";
    expect(program.topo_order.empty()) << "cycle members are not ordered";
  };

  "cycle_trace_is_deterministic_across_input_orders"_test = [] {
    auto forward = program_of({{"a.dao", "module a\nimport b\n"},
                               {"b.dao", "module b\nimport c\n"},
                               {"c.dao", "module c\nimport a\n"}});
    auto reversed = program_of({{"c.dao", "module c\nimport a\n"},
                                {"b.dao", "module b\nimport c\n"},
                                {"a.dao", "module a\nimport b\n"}});
    expect(messages(forward) == messages(reversed)) << joined(messages(forward));
    expect(messages(forward)[0] == "import cycle: a -> b -> c -> a");
  };

  "input_order_does_not_change_file_ids_or_order"_test = [] {
    auto forward = program_of({{"a.dao", "module a\nimport b\n" + kMain}, {"b.dao", "module b\n"}});
    auto reversed =
        program_of({{"b.dao", "module b\n"}, {"a.dao", "module a\nimport b\n" + kMain}});
    expect(forward.files[0]->display_path == reversed.files[0]->display_path);
    expect(forward.files[0]->base_offset == reversed.files[0]->base_offset);
    expect(displays(forward.topo_order) == displays(reversed.topo_order));
    expect(forward.entry->display == reversed.entry->display);
  };

  "file_without_module_declaration_is_tolerated"_test = [] {
    auto program =
        program_of({{"a.dao", "module a\n" + kMain}, {"bare.dao", "fn f(): i32 -> 1\n"}});
    expect(program.modules.size() == 1_u);
    expect(program.files[1]->module == nullptr);
    expect(program.entry == program.module_named("a"));
  };

  "acyclic_dependent_is_excluded_from_the_trace"_test = [] {
    auto program = program_of({{"a.dao", "module a\nimport b\n"},
                               {"b.dao", "module b\nimport a\n"},
                               {"d.dao", "module d\nimport a\n"}});
    expect(messages(program) == std::vector<std::string>{"import cycle: a -> b -> a"})
        << joined(messages(program));
  };

  "two_cycles_joined_by_an_acyclic_bridge_are_both_reported"_test = [] {
    // `c` is on no cycle: it imports the b-cycle and the d-cycle imports
    // it.  Reporting the b-cycle first leaves `c` with nothing left to
    // import, so a trace walk that assumes every survivor still has an
    // edge steps off the graph before it reaches the d-cycle.
    const std::vector<NamedSource> sources = {{"b1.dao", "module b1\nimport b2\n"},
                                              {"b2.dao", "module b2\nimport b1\n"},
                                              {"c.dao", "module c\nimport b1\n"},
                                              {"d.dao", "module d\nimport c\nimport e\n"},
                                              {"e.dao", "module e\nimport d\n"}};
    const std::vector<std::string> expected = {"import cycle: b1 -> b2 -> b1",
                                               "import cycle: d -> e -> d"};
    auto forward = program_of(sources);
    expect(messages(forward) == expected) << joined(messages(forward));
    expect(forward.topo_order.empty()) << "no module of either cycle is ordered";

    // Every permutation of the same set says the same thing (§8.4).
    auto permuted = sources;
    std::ranges::sort(permuted);
    do {
      auto program = program_of(permuted);
      expect(messages(program) == expected) << joined(messages(program));
    } while (std::ranges::next_permutation(permuted).found);
  };

  "a bridge into a reported cycle is not itself reported"_test = [] {
    // The bridge and the acyclic dependent are the same rule seen from
    // two sides: a module is named by a trace only when it is on the
    // cycle that trace describes.
    auto program = program_of({{"a.dao", "module a\nimport b\n"},
                               {"b.dao", "module b\nimport a\n"},
                               {"bridge.dao", "module bridge\nimport b\n"},
                               {"top.dao", "module top\nimport bridge\n"}});
    expect(messages(program) == std::vector<std::string>{"import cycle: a -> b -> a"})
        << joined(messages(program));
  };

  "import_of_a_prelude_module_resolves"_test = [] {
    std::vector<SourceInput> inputs = {
        {.display_path = "stdlib/core/mini.dao", .text = "module core::mini\n", .is_prelude = true},
        {.display_path = "app.dao",
         .text = "module app\nimport core::mini\n" + kMain,
         .is_prelude = false},
    };
    auto program = build_program(std::move(inputs));
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(displays(program.topo_order) == std::vector<std::string>{"core::mini", "app"});
    expect(program.module_named("core::mini")->is_prelude);
  };
};

suite<"entry_selection"> entry_selection_suite = [] {
  "explicit_entry_wins"_test = [] {
    auto program =
        program_of({{"a.dao", "module a\n" + kMain}, {"b.dao", "module b\n" + kMain}}, "b");
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(program.entry == program.module_named("b"));
  };

  "an extern main declares no entry"_test = [] {
    // `extern fn main` names an entry defined elsewhere and lowers to a
    // declaration with no body, so a program holding only that one has
    // no entry to build (§8).
    auto program = program_of(
        {{"a.dao", "module a\nextern fn main(): i32\n"}}, std::nullopt, EntryPolicy::Required);
    expect(program.entry == nullptr);
    expect(joined(messages(program)).find("no module declares 'fn main'") != std::string::npos)
        << joined(messages(program));
  };

  "several_mains_without_entry_is_an_error"_test = [] {
    auto program = program_of({{"a.dao", "module a\n" + kMain}, {"b.dao", "module b\n" + kMain}});
    expect(program.entry == nullptr);
    expect(messages(program) ==
           std::vector<std::string>{
               "ambiguous entry module: 'fn main' declared in a, b (use --entry)"})
        << joined(messages(program));
  };

  "an in_memory_program_without_main_needs_no_entry"_test = [] {
    // build_program assembles a fragment (the playground's buffer, a
    // single-file test), not a delivered file set: an entry is not
    // required of it (CONTRACT_MODULE_SYSTEM.md §8.3 governs file sets).
    auto program = program_of({{"lib.dao", "module lib\nfn f(): i32 -> 1\n"}});
    expect(program.entry == nullptr);
    expect(program.diagnostics.empty()) << joined(messages(program));
  };

  "module identity is indexed, not scanned"_test = [] {
    // Import resolution asks once per edge; a scan per edge would be
    // quadratic in the graph.  The index answers every module and
    // nothing else, so a lookup cannot degrade into a walk.
    std::vector<NamedSource> sources;
    for (int i = 0; i < 24; ++i) {
      auto name = "m" + std::to_string(i);
      auto text = "module " + name + "\n" +
                  (i > 0 ? "import m" + std::to_string(i - 1) + "\n" : "") + "fn f(): i32 -> " +
                  std::to_string(i) + "\n";
      sources.emplace_back(name + ".dao", text);
    }
    auto program = program_of(std::move(sources));
    expect(program.by_display.size() == program.modules.size())
        << "every module is indexed exactly once";
    for (const auto& module : program.modules) {
      expect(program.module_named(module->display) == module.get()) << module->display;
    }
    expect(program.module_named("absent") == nullptr);
  };

  "a named entry module without main is an error"_test = [] {
    // The entry module's `fn main` is the program entry (§8.5): selecting
    // a module by name does not excuse it from declaring one, even when
    // another module in the program does.
    auto program = build_program(
        {{.display_path = "lib.dao", .text = "module lib\nfn f(): i32 -> 1\n", .is_prelude = false},
         {.display_path = "app.dao", .text = "module app\n" + kMain, .is_prelude = false}},
        "lib",
        EntryPolicy::Required);
    expect(messages(program) ==
           std::vector<std::string>{"entry module 'lib' (--entry) declares no 'fn main'"})
        << joined(messages(program));
  };

  "the same root under two spellings gives one program"_test = [] {
    // Files are ordered and named by the file each path names, not by
    // how the invocation reached it (§8.4), so a `..` detour and a
    // relative spelling must give the same layout as the plain path —
    // discovered imports included, since those keep their own display
    // paths.
    auto root = fixtures() / "smoke" / "main.dao";
    auto layout = [](const Program& program) {
      std::vector<std::string> out;
      for (const auto& file : program.files) {
        out.push_back(file->display_path + "@" + std::to_string(file->base_offset));
      }
      return out;
    };
    auto plain = layout(load_program_from_root(root, {.stdlib_root = {}}));
    expect(plain.size() > 1_ul) << "fixture no longer discovers imports";

    auto detour = fixtures() / "smoke" / "app" / ".." / "main.dao";
    expect(layout(load_program_from_root(detour, {.stdlib_root = {}})) == plain)
        << joined(layout(load_program_from_root(detour, {.stdlib_root = {}})));

    std::error_code ec;
    auto relative = std::filesystem::relative(root, ec);
    if (!ec && !relative.empty()) {
      expect(layout(load_program_from_root(relative, {.stdlib_root = {}})) == plain)
          << joined(layout(load_program_from_root(relative, {.stdlib_root = {}})));
    }
  };

  "a root file without main is an error"_test = [] {
    // Root-file mode takes the root's module as the entry (§8.2); the
    // same requirement applies to it.
    auto program =
        load_program_from_root(fixtures() / "smoke" / "app" / "util.dao", {.stdlib_root = {}});
    bool named = false;
    for (const auto& diag : program.diagnostics) {
      named = named || diag.message.find("declares no 'fn main'") != std::string::npos;
    }
    expect(named) << joined(messages(program));
  };

  "an explicit file set without main is an error"_test = [] {
    // A library compiled on its own would otherwise reach the linker
    // and fail there with an undefined `main`.
    auto program =
        load_program_from_files({fixtures() / "smoke" / "app" / "util.dao"}, {.stdlib_root = {}});
    expect(program.entry == nullptr);
    expect(messages(program) ==
           std::vector<std::string>{"no entry module: no module declares 'fn main' (use --entry)"})
        << joined(messages(program));
  };

  "entry_not_in_program_is_an_error"_test = [] {
    auto program = program_of({{"a.dao", "module a\n" + kMain}}, "zzz");
    expect(program.entry == nullptr);
    expect(messages(program) ==
           std::vector<std::string>{"entry module 'zzz' is not in the program"});
  };

  "prelude_main_is_not_a_candidate"_test = [] {
    std::vector<SourceInput> inputs = {
        {.display_path = "stdlib/core/p.dao",
         .text = "module core::p\n" + kMain,
         .is_prelude = true},
        {.display_path = "app.dao", .text = "module app\n" + kMain, .is_prelude = false},
    };
    auto program = build_program(std::move(inputs));
    expect(program.entry == program.module_named("app"));
  };
};

suite<"root_file_discovery"> root_file_discovery_suite = [] {
  "discovers_transitively_and_the_root_is_the_entry"_test = [] {
    auto program =
        load_program_from_root(fixtures() / "smoke" / "main.dao",
                               {.stdlib_root = std::filesystem::path(DAO_SOURCE_DIR) / "stdlib"});
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(program.entry != nullptr && program.entry->display == "smoke");
    auto order = displays(program.topo_order);
    auto index = [&](const std::string& name) {
      return std::ranges::find(order, name) - order.begin();
    };
    expect(index("app::util") < index("app::math") && index("app::math") < index("smoke"));
    expect(index("core::vector") < index("smoke"))
        << "prelude import resolved to the loaded prelude";
    expect(program.module_named("app::util")->file->display_path.ends_with("app/util.dao"));
  };

  "path_declaration_mismatch_is_diagnosed"_test = [] {
    auto program = load_program_from_root(fixtures() / "mismatch" / "main.dao", {});
    expect(program.diagnostics.size() == 1_u) << joined(messages(program));
    expect(messages(program)[0].ends_with(
        "lib/util.dao was found for import 'lib::util' but declares module 'lib::other'"))
        << messages(program)[0];
    expect(program.module_named("mismatch")->imports.empty());
  };

  "a duplicate identity is still named by the mismatch it causes"_test = [] {
    // `dup/second.dao` declares an identity `dup/first.dao` already
    // registered, so it owns no module of the graph.  The mismatch for
    // the import that found it must still say what it declares (§2.3),
    // not "no module", which is what asking the graph would answer.
    auto program = load_program_from_root(fixtures() / "duplicate" / "main.dao", {});
    auto said = joined(messages(program));
    expect(said.find("declares module 'dup::first'") != std::string::npos) << said;
    expect(said.find("declares no module") == std::string::npos) << said;
    expect(said.find("is already declared by") != std::string::npos) << said;
  };

  "an earlier root's mismatch is not excused by a module loaded elsewhere"_test = [] {
    // The root directory maps `core::x` to a file declaring `core::wrong`
    // while the prelude supplies a real `core::x`.  §8.3 makes the FIRST
    // located file the one that must agree: binding the prelude module
    // instead would let the import name a module the mapping rule never
    // chose, and the disagreeing file would go unmentioned.
    auto fixture = fixtures() / "shadowed_prelude";
    auto program =
        load_program_from_root(fixture / "main.dao", {.stdlib_root = fixture / "stdlib"});
    expect(program.diagnostics.size() == 1_u) << joined(messages(program));
    expect(messages(program)[0].ends_with(
        "core/x.dao was found for import 'core::x' but declares module 'core::wrong'"))
        << messages(program)[0];
    expect(program.module_named("app")->imports.empty()) << "the import binds nothing";
    expect(program.module_named("core::x") != nullptr)
        << "the prelude module is still in the program; it is just not what the search found";
  };

  "a prelude file's imports are discovered too"_test = [] {
    // The prelude group is loaded, not discovered, so its own imports
    // were never followed: what a prelude file imports is part of the
    // program even when no user file mentions it (§8.2).
    auto fixture = fixtures() / "prelude_imports";
    auto program = load_program_from_root(
        fixture / "main.dao", {.stdlib_root = fixture / "stdlib", .module_roots = {fixture}});
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(program.module_named("ext::thing") != nullptr)
        << "the module a prelude file imports was not discovered";
  };

  "module_roots_are_searched_after_the_root_directory"_test = [] {
    auto program = load_program_from_root(fixtures() / "roots" / "main" / "main.dao",
                                          {.module_roots = {fixtures() / "roots" / "extra"}});
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(displays(program.topo_order) == std::vector<std::string>{"ext::thing", "rooted"});
  };

  "not_found_names_the_roots_searched"_test = [] {
    auto program = load_program_from_root(fixtures() / "roots" / "main" / "main.dao", {});
    expect(program.diagnostics.size() == 1_u) << joined(messages(program));
    expect(messages(program)[0].starts_with("imported module 'ext::thing' not found; searched "));
  };

  "the roots are named in the order they are searched"_test = [] {
    // §8.2 fixes the order — the root's directory, each --module-root in
    // command-line order, then the stdlib root — and the diagnostic is
    // what tells the user which one was expected to have the file.
    auto roots = fixtures() / "roots";
    auto program = load_program_from_root(
        roots / "main" / "main.dao",
        {.stdlib_root = roots / "nowhere", .module_roots = {roots / "first", roots / "second"}});
    auto expected = "; searched " + (roots / "main").generic_string() + " " +
                    (roots / "first").generic_string() + " " + (roots / "second").generic_string() +
                    " " + (roots / "nowhere").generic_string();
    expect(program.diagnostics.size() == 1_u) << joined(messages(program));
    expect(messages(program)[0].ends_with(expected)) << messages(program)[0];
  };

  "a root that is also a prelude file keeps its root role"_test = [] {
    // `daoc check stdlib/core/lib.dao`.  The program holds one copy of
    // the file, in the prelude group — whether a stdlib file belongs to
    // the prelude cannot depend on whether the command line named it
    // (§7.6).  What the root contributes is its role: its module is the
    // entry (§7.7), and an entry declaring no `fn main` is an error.
    auto stdlib = fixtures() / "prelude_root" / "stdlib";
    auto program = load_program_from_root(stdlib / "core" / "lib.dao", {.stdlib_root = stdlib});
    expect(program.entry != nullptr && program.entry->display == "core::lib")
        << "the root file selects no entry at all";
    expect(messages(program) ==
           std::vector<std::string>{"entry module 'core::lib' (the root file) declares no 'fn "
                                    "main'"})
        << joined(messages(program));
  };

  "a prelude root with main is the entry and is not duplicated"_test = [] {
    auto stdlib = fixtures() / "prelude_root" / "stdlib";
    auto program = load_program_from_root(stdlib / "core" / "app.dao", {.stdlib_root = stdlib});
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(program.entry != nullptr && program.entry->display == "core::app");
    expect(program.files.size() == 2_ul) << "the root is loaded once, not once per role";
    expect(program.user_files().empty())
        << "a stdlib file stays in the prelude group when it is also the root";
  };
};

auto main(int argc, const char** argv) -> int {
  return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv}) ? 1 : 0;
}
