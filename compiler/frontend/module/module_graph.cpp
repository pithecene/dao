#include "frontend/module/module_graph.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace dao {

auto module_display(const std::vector<std::string_view>& segments) -> std::string {
  std::string display;
  for (auto segment : segments) {
    if (!display.empty()) {
      display += "::";
    }
    display += segment;
  }
  return display;
}

namespace {

struct ByDisplay {
  auto operator()(const ModuleInfo* lhs, const ModuleInfo* rhs) const -> bool {
    return lhs->display < rhs->display;
  }
};

using ModuleSet = std::set<ModuleInfo*, ByDisplay>;

auto declares_main(const FileNode& file) -> bool {
  return std::ranges::any_of(file.declarations, [](const Decl* decl) -> bool {
    if (!decl->is<FunctionDecl>()) {
      return false;
    }
    // `extern fn main` names an entry defined somewhere else and has no
    // body of its own, so it cannot be the program's (§8).  Counting it
    // would let a program with no entry at all reach the linker.
    const auto& fn = decl->as<FunctionDecl>();
    return fn.name == "main" && !fn.is_extern;
  });
}

// ---------------------------------------------------------------------------
// Registration: one module per file with a declaration; a second file
// declaring the same identity is diagnosed where it declares it.
// ---------------------------------------------------------------------------

void register_modules(Program& program) {
  for (const auto& file : program.files) {
    const auto* node = file->parse.file;
    if (node == nullptr || node->module_decl == nullptr) {
      continue; // a missing module declaration is already a parse error
    }
    auto display = module_display(node->module_decl->path.segments);
    if (auto existing = program.by_display.find(display); existing != program.by_display.end()) {
      program.diagnostics.push_back(Diagnostic::error(node->module_decl->span,
                                                      "module '" + display +
                                                          "' is already declared by " +
                                                          existing->second->file->display_path));
      continue;
    }
    auto module = std::make_unique<ModuleInfo>(ModuleInfo{
        .display = display,
        .file = file.get(),
        .is_prelude = file->is_prelude,
        .declares_main = declares_main(*node),
    });
    file->module = module.get();
    program.by_display.emplace(module->display, module.get());
    program.modules.push_back(std::move(module));
  }
}

// ---------------------------------------------------------------------------
// Edges: each import resolves to a module of the program or is diagnosed.
// ---------------------------------------------------------------------------

/// The module identity a file declares, taken from the file itself.
/// A file that lost registration to a duplicate has no ModuleInfo, so
/// asking the graph would answer "no module" for a file that plainly
/// declares one (§2.3 wants both identities named).
auto declared_identity(const SourceFile* file) -> std::optional<std::string> {
  if (file == nullptr || file->parse.file == nullptr || file->parse.file->module_decl == nullptr) {
    return std::nullopt;
  }
  return module_display(file->parse.file->module_decl->path.segments);
}

/// The files discovery loaded, by the import identity it went looking
/// for and by display path.  Built once per graph: an unresolved import
/// asks both, and rescanning the whole input set for each one is
/// quadratic in programs where several imports fail together.
struct LocatedIndex {
  std::unordered_map<std::string_view, const std::string*> display_by_identity;
  std::unordered_map<std::string_view, const SourceFile*> file_by_display;

  LocatedIndex(const GraphInputs& inputs, const Program& program) {
    for (const auto& located : inputs.located) {
      display_by_identity.try_emplace(located.identity, &located.display_path);
    }
    for (const auto& file : program.files) {
      file_by_display.try_emplace(file->display_path, file.get());
    }
  }

  [[nodiscard]] auto display_for(const std::string& identity) const -> const std::string* {
    auto it = display_by_identity.find(identity);
    return it == display_by_identity.end() ? nullptr : it->second;
  }

  [[nodiscard]] auto file_at(const std::string& display) const -> const SourceFile* {
    auto it = file_by_display.find(display);
    return it == file_by_display.end() ? nullptr : it->second;
  }
};

auto not_found_message(const GraphInputs& inputs, const std::string& identity) -> std::string {
  std::string message = "imported module '" + identity + "' not found";
  if (inputs.searched_roots.empty()) {
    return message + " in the program";
  }
  message += "; searched";
  for (const auto& root : inputs.searched_roots) {
    message += " " + root;
  }
  return message;
}

void resolve_edges(Program& program, const GraphInputs& inputs) {
  const LocatedIndex located(inputs, program);
  for (auto& module : program.modules) {
    std::unordered_set<ModuleInfo*> seen;
    for (const auto* import : module->file->parse.file->imports) {
      auto identity = module_display(import->path.segments);
      if (identity == module->display) {
        program.diagnostics.push_back(
            Diagnostic::error(import->span, "module '" + identity + "' imports itself"));
        continue;
      }
      // Root-file mode located a file for this import by the §8.3
      // mapping rule, and that file's own declaration must agree.  Asked
      // BEFORE the graph: a module of the requested identity loaded from
      // somewhere else — a prelude file, another root — does not excuse
      // the file the search actually found, and binding it would let the
      // import name a module the mapping rule never chose.
      if (const auto* display = located.display_for(identity)) {
        auto declared = declared_identity(located.file_at(*display));
        if (declared != identity) {
          program.diagnostics.push_back(Diagnostic::error(
              import->span,
              *display + " was found for import '" + identity + "' but declares " +
                  (declared ? "module '" + *declared + "'" : "no module")));
          continue;
        }
      }
      auto* target = program.module_named(identity);
      if (target == nullptr) {
        program.diagnostics.push_back(
            Diagnostic::error(import->span, not_found_message(inputs, identity)));
        continue;
      }
      if (seen.insert(target).second) {
        module->imports.push_back(target);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Order: Kahn's algorithm with the ready set in lexical order of module
// display, so the order is a function of the graph alone (§8.4).
// ---------------------------------------------------------------------------

auto dependents_of(const Program& program)
    -> std::unordered_map<const ModuleInfo*, std::vector<ModuleInfo*>> {
  std::unordered_map<const ModuleInfo*, std::vector<ModuleInfo*>> dependents;
  for (const auto& module : program.modules) {
    for (auto* imported : module->imports) {
      dependents[imported].push_back(module.get());
    }
  }
  return dependents;
}

auto topological_order(Program& program) -> ModuleSet
/* modules left unordered: on or behind a cycle */ {
  auto dependents = dependents_of(program);
  std::unordered_map<const ModuleInfo*, size_t> pending_imports;
  ModuleSet ready;
  for (const auto& module : program.modules) {
    pending_imports[module.get()] = module->imports.size();
    if (module->imports.empty()) {
      ready.insert(module.get());
    }
  }
  while (!ready.empty()) {
    auto* next = *ready.begin();
    ready.erase(ready.begin());
    program.topo_order.push_back(next);
    for (auto* dependent : dependents[next]) {
      if (--pending_imports[dependent] == 0) {
        ready.insert(dependent);
      }
    }
  }
  ModuleSet unordered;
  for (const auto& module : program.modules) {
    if (pending_imports[module.get()] != 0) {
      unordered.insert(module.get());
    }
  }
  return unordered;
}

// ---------------------------------------------------------------------------
// Cycles: modules left unordered are on a cycle or merely import one.
// Reducing that set to its cycle kernel drops the modules that only
// lead to a cycle; walking first-import edges from the lexically
// smallest survivor then closes a cycle, which is reported once and
// removed.  Deterministic, and the trace names only cycle members.
// ---------------------------------------------------------------------------

/// Drop every module of `remaining` that is not itself on a cycle: one
/// nothing left imports (an acyclic dependent, which §8.5 keeps out of
/// the trace) and one that imports nothing left (a bridge into a cycle
/// already reported and erased) are both peripheral, and dropping one
/// can make its neighbours peripheral in turn.
///
/// Every survivor therefore has a remaining import to walk.  Only
/// dependents were counted at first, which holds for the residue Kahn's
/// algorithm leaves — every module in it has an unordered import — but
/// not after report_cycles erases a cycle's members, where a bridge
/// module can be left with no remaining import at all and the walk
/// below would step off the end of the graph.
void strip_to_cycle_kernel(ModuleSet& remaining) {
  // Both directions: dropping a module leaves the modules it imports
  // with one dependent fewer and the modules importing it with one
  // import fewer, and either count reaching zero drops that module too.
  // Counting once and decrementing costs one pass over the edges rather
  // than a rescan of the set per candidate.
  std::unordered_map<const ModuleInfo*, std::vector<ModuleInfo*>> importers;
  std::unordered_map<const ModuleInfo*, size_t> dependent_count;
  std::unordered_map<const ModuleInfo*, size_t> import_count;
  for (auto* module : remaining) {
    dependent_count.try_emplace(module, 0);
    import_count.try_emplace(module, 0);
  }
  for (auto* module : remaining) {
    for (auto* imported : module->imports) {
      if (!remaining.contains(imported)) {
        continue;
      }
      ++dependent_count[imported];
      ++import_count[module];
      importers[imported].push_back(module);
    }
  }

  // Seeded in display order so the trace a cycle report walks is the
  // same on every run; the set that survives is order-independent.
  std::vector<ModuleInfo*> peripheral;
  for (auto* module : remaining) {
    if (dependent_count[module] == 0 || import_count[module] == 0) {
      peripheral.push_back(module);
    }
  }
  while (!peripheral.empty()) {
    auto* dropped = peripheral.back();
    peripheral.pop_back();
    if (remaining.erase(dropped) == 0) {
      continue; // both counts reached zero, or a neighbour queued it twice
    }
    for (auto* imported : dropped->imports) {
      if (remaining.contains(imported) && --dependent_count[imported] == 0) {
        peripheral.push_back(imported);
      }
    }
    for (auto* importer : importers[dropped]) {
      if (remaining.contains(importer) && --import_count[importer] == 0) {
        peripheral.push_back(importer);
      }
    }
  }
}

auto first_import_within(const ModuleInfo& module, const ModuleSet& remaining) -> ModuleInfo* {
  auto it = std::ranges::find_if(
      module.imports, [&](ModuleInfo* imported) { return remaining.contains(imported); });
  return it == module.imports.end() ? nullptr : *it;
}

/// Walk first-import edges from `start` until the walk revisits a
/// module; that module and everything after its first visit are the
/// cycle.  Empty only if the walk runs out of edges, which the cycle
/// kernel rules out.
auto cycle_from(ModuleInfo* start, const ModuleSet& remaining) -> std::vector<ModuleInfo*> {
  std::vector<ModuleInfo*> path;
  std::map<const ModuleInfo*, size_t> position;
  for (auto* current = start; current != nullptr;
       current = first_import_within(*current, remaining)) {
    if (auto seen = position.find(current); seen != position.end()) {
      return {path.begin() + static_cast<std::ptrdiff_t>(seen->second), path.end()};
    }
    position.emplace(current, path.size());
    path.push_back(current);
  }
  return {};
}

auto import_span(const ModuleInfo& module, const ModuleInfo& target) -> Span {
  for (const auto* import : module.file->parse.file->imports) {
    if (module_display(import->path.segments) == target.display) {
      return import->span;
    }
  }
  return module.file->parse.file->module_decl->span;
}

void report_cycles(Program& program, ModuleSet remaining) {
  for (strip_to_cycle_kernel(remaining); !remaining.empty(); strip_to_cycle_kernel(remaining)) {
    auto* start = *remaining.begin();
    auto cycle = cycle_from(start, remaining);
    if (cycle.empty()) {
      // The kernel leaves only modules with a remaining import, so the
      // walk always closes.  Erasing the module it started from keeps a
      // later change to the kernel from spinning here forever.
      remaining.erase(start);
      continue;
    }

    std::string trace;
    for (const auto* member : cycle) {
      trace += member->display + " -> ";
    }
    trace += cycle.front()->display;
    program.diagnostics.push_back(Diagnostic::error(
        import_span(*cycle.front(), *cycle[1 % cycle.size()]), "import cycle: " + trace));
    for (auto* member : cycle) {
      remaining.erase(member);
    }
  }
}

// ---------------------------------------------------------------------------
// Entry (§7.7)
// ---------------------------------------------------------------------------

/// The entry module's `fn main` is the program entry
/// (CONTRACT_MODULE_SYSTEM.md §8.5), so a module selected as the entry —
/// by being the root file or by name — must declare one.  Selecting it
/// and saying nothing sends a program with no entry point to the linker,
/// which is the failure the missing-entry diagnostic exists to prevent.
void require_main(Program& program,
                  const GraphInputs& inputs,
                  ModuleInfo* entry,
                  const std::string& how) {
  if (entry == nullptr || entry->declares_main) {
    return;
  }
  const std::string message =
      "entry module '" + entry->display + "' (" + how + ") declares no 'fn main'";
  if (inputs.entry_policy == EntryPolicy::Required) {
    program.diagnostics.push_back(Diagnostic::error(Span{}, message));
  } else if (inputs.entry_policy == EntryPolicy::Advisory) {
    program.diagnostics.push_back(Diagnostic::warning(Span{}, message));
  }
}

void select_entry(Program& program, const GraphInputs& inputs) {
  if (!inputs.root_display.empty()) {
    auto root = std::ranges::find_if(
        program.files, [&](const auto& file) { return file->display_path == inputs.root_display; });
    program.entry = root == program.files.end() ? nullptr : (*root)->module;
    require_main(program, inputs, program.entry, "the root file");
    return;
  }
  if (inputs.entry) {
    program.entry = program.module_named(*inputs.entry);
    if (program.entry == nullptr) {
      program.diagnostics.push_back(
          Diagnostic::error(Span{}, "entry module '" + *inputs.entry + "' is not in the program"));
    }
    require_main(program, inputs, program.entry, "--entry");
    return;
  }
  std::vector<ModuleInfo*> candidates;
  for (const auto& module : program.modules) {
    if (!module->is_prelude && module->declares_main) {
      candidates.push_back(module.get());
    }
  }
  if (candidates.size() == 1) {
    program.entry = candidates.front();
  } else if (candidates.empty()) {
    // A program built into an executable has exactly one entry module
    // (CONTRACT_MODULE_SYSTEM.md §8.1, §8.3); saying so here is what
    // keeps a library file from reaching the linker as `undefined
    // main`.  How loudly depends on what the caller is compiling.
    const std::string message = "no entry module: no module declares 'fn main' (use --entry)";
    if (inputs.entry_policy == EntryPolicy::Required) {
      program.diagnostics.push_back(Diagnostic::error(Span{}, message));
    } else if (inputs.entry_policy == EntryPolicy::Advisory) {
      program.diagnostics.push_back(Diagnostic::warning(Span{}, message));
    }
  } else {
    std::string names;
    for (const auto* candidate : candidates) {
      names += (names.empty() ? "" : ", ") + candidate->display;
    }
    program.diagnostics.push_back(Diagnostic::error(
        Span{}, "ambiguous entry module: 'fn main' declared in " + names + " (use --entry)"));
  }
}

} // namespace

void build_module_graph(Program& program, const GraphInputs& inputs) {
  register_modules(program);
  resolve_edges(program, inputs);
  report_cycles(program, topological_order(program));
  select_entry(program, inputs);
}

} // namespace dao
