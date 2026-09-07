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

auto program_of(std::vector<NamedSource> sources, std::optional<std::string> entry = {})
    -> Program {
  std::vector<SourceInput> inputs;
  for (auto& [display, text] : sources) {
    inputs.push_back({.display_path = display, .text = text, .is_prelude = false});
  }
  return build_program(std::move(inputs), std::move(entry));
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
    auto program = program_of({{"z.dao", "module zeta\n"}, {"a.dao", "module alpha\n"},
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
    auto program = program_of({{"a.dao", "module a\nimport b\n"}, {"b.dao", "module b\nimport a\n"}});
    expect(messages(program) == std::vector<std::string>{"import cycle: a -> b -> a"})
        << joined(messages(program));
    expect(program.source_map.file_for(program.diagnostics[0].span.offset)->display_path ==
           "a.dao")
        << "reported at the import that starts the trace";
    expect(program.topo_order.empty()) << "cycle members are not ordered";
  };

  "cycle_trace_is_deterministic_across_input_orders"_test = [] {
    auto forward = program_of({{"a.dao", "module a\nimport b\n"}, {"b.dao", "module b\nimport c\n"},
                               {"c.dao", "module c\nimport a\n"}});
    auto reversed = program_of({{"c.dao", "module c\nimport a\n"}, {"b.dao", "module b\nimport c\n"},
                                {"a.dao", "module a\nimport b\n"}});
    expect(messages(forward) == messages(reversed)) << joined(messages(forward));
    expect(messages(forward)[0] == "import cycle: a -> b -> c -> a");
  };

  "input_order_does_not_change_file_ids_or_order"_test = [] {
    auto forward = program_of({{"a.dao", "module a\nimport b\n" + kMain}, {"b.dao", "module b\n"}});
    auto reversed = program_of({{"b.dao", "module b\n"}, {"a.dao", "module a\nimport b\n" + kMain}});
    expect(forward.files[0]->display_path == reversed.files[0]->display_path);
    expect(forward.files[0]->base_offset == reversed.files[0]->base_offset);
    expect(displays(forward.topo_order) == displays(reversed.topo_order));
    expect(forward.entry->display == reversed.entry->display);
  };

  "file_without_module_declaration_is_tolerated"_test = [] {
    auto program = program_of({{"a.dao", "module a\n" + kMain}, {"bare.dao", "fn f(): i32 -> 1\n"}});
    expect(program.modules.size() == 1_u);
    expect(program.files[1]->module == nullptr);
    expect(program.entry == program.module_named("a"));
  };

  "acyclic_dependent_is_excluded_from_the_trace"_test = [] {
    auto program = program_of({{"a.dao", "module a\nimport b\n"}, {"b.dao", "module b\nimport a\n"},
                               {"d.dao", "module d\nimport a\n"}});
    expect(messages(program) == std::vector<std::string>{"import cycle: a -> b -> a"})
        << joined(messages(program));
  };

  "import_of_a_prelude_module_resolves"_test = [] {
    std::vector<SourceInput> inputs = {
        {.display_path = "stdlib/core/mini.dao", .text = "module core::mini\n", .is_prelude = true},
        {.display_path = "app.dao", .text = "module app\nimport core::mini\n" + kMain, .is_prelude = false},
    };
    auto program = build_program(std::move(inputs));
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(displays(program.topo_order) == std::vector<std::string>{"core::mini", "app"});
    expect(program.module_named("core::mini")->is_prelude);
  };
};

suite<"entry_selection"> entry_selection_suite = [] {
  "explicit_entry_wins"_test = [] {
    auto program = program_of({{"a.dao", "module a\n" + kMain}, {"b.dao", "module b\n" + kMain}}, "b");
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(program.entry == program.module_named("b"));
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
    expect(messages(program) == std::vector<std::string>{"entry module 'zzz' is not in the program"});
  };

  "prelude_main_is_not_a_candidate"_test = [] {
    std::vector<SourceInput> inputs = {
        {.display_path = "stdlib/core/p.dao", .text = "module core::p\n" + kMain, .is_prelude = true},
        {.display_path = "app.dao", .text = "module app\n" + kMain, .is_prelude = false},
    };
    auto program = build_program(std::move(inputs));
    expect(program.entry == program.module_named("app"));
  };
};

suite<"root_file_discovery"> root_file_discovery_suite = [] {
  "discovers_transitively_and_the_root_is_the_entry"_test = [] {
    auto program = load_program_from_root(
        fixtures() / "smoke" / "main.dao",
        {.stdlib_root = std::filesystem::path(DAO_SOURCE_DIR) / "stdlib"});
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(program.entry != nullptr && program.entry->display == "smoke");
    auto order = displays(program.topo_order);
    auto index = [&](const std::string& name) {
      return std::ranges::find(order, name) - order.begin();
    };
    expect(index("app::util") < index("app::math") && index("app::math") < index("smoke"));
    expect(index("core::vector") < index("smoke")) << "prelude import resolved to the loaded prelude";
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

  "module_roots_are_searched_after_the_root_directory"_test = [] {
    auto program = load_program_from_root(
        fixtures() / "roots" / "main" / "main.dao",
        {.module_roots = {fixtures() / "roots" / "extra"}});
    expect(program.diagnostics.empty()) << joined(messages(program));
    expect(displays(program.topo_order) == std::vector<std::string>{"ext::thing", "rooted"});
  };

  "not_found_names_the_roots_searched"_test = [] {
    auto program = load_program_from_root(fixtures() / "roots" / "main" / "main.dao", {});
    expect(program.diagnostics.size() == 1_u) << joined(messages(program));
    expect(messages(program)[0].starts_with("imported module 'ext::thing' not found; searched "));
  };
};

auto main(int argc, const char** argv) -> int {
  return boost::ut::cfg<>.run({.report_errors = true, .argc = argc, .argv = argv}) ? 1 : 0;
}
