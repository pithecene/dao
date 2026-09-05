#include "frontend/module/module_graph.h"

#include <algorithm>
#include <map>
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
    return decl->is<FunctionDecl>() && decl->as<FunctionDecl>().name == "main";
  });
}

// ---------------------------------------------------------------------------
// Registration: one module per file with a declaration; a second file
// declaring the same identity is diagnosed where it declares it.
// ---------------------------------------------------------------------------

void register_modules(Program& program) {
  std::unordered_map<std::string, ModuleInfo*> by_display;
  for (const auto& file : program.files) {
    const auto* node = file->parse.file;
    if (node == nullptr || node->module_decl == nullptr) {
      continue; // a missing module declaration is already a parse error
    }
    auto display = module_display(node->module_decl->path.segments);
    if (auto existing = by_display.find(display); existing != by_display.end()) {
      program.diagnostics.push_back(Diagnostic::error(
          node->module_decl->span, "module '" + display + "' is already declared by " +
                                       existing->second->file->display_path));
      continue;
    }
    auto module = std::make_unique<ModuleInfo>(ModuleInfo{
        .module_id = static_cast<uint32_t>(program.modules.size()),
        .segments = node->module_decl->path.segments,
        .display = display,
        .file = file.get(),
        .is_prelude = file->is_prelude,
        .declares_main = declares_main(*node),
    });
    file->module = module.get();
    by_display.emplace(display, module.get());
    program.modules.push_back(std::move(module));
  }
}

// ---------------------------------------------------------------------------
// Edges: each import resolves to a module of the program or is diagnosed.
// ---------------------------------------------------------------------------

auto located_display(const GraphInputs& inputs, const std::string& identity)
    -> const std::string* {
  auto it = std::ranges::find(inputs.located, identity, &LocatedImport::identity);
  return it == inputs.located.end() ? nullptr : &it->display_path;
}

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
  for (auto& module : program.modules) {
    std::unordered_set<ModuleInfo*> seen;
    for (const auto* import : module->file->parse.file->imports) {
      auto identity = module_display(import->path.segments);
      if (identity == module->display) {
        program.diagnostics.push_back(
            Diagnostic::error(import->span, "module '" + identity + "' imports itself"));
        continue;
      }
      auto* target = program.module_named(identity);
      if (target == nullptr) {
        // Root-file mode may have loaded a file for this import that
        // turned out to declare something else (§8.3).
        if (const auto* display = located_display(inputs, identity)) {
          const auto* file = std::ranges::find_if(program.files, [&](const auto& f) {
                               return f->display_path == *display;
                             })->get();
          auto declared = file->module != nullptr ? file->module->display : "no module";
          program.diagnostics.push_back(Diagnostic::error(
              import->span, *display + " was found for import '" + identity +
                                "' but declares " +
                                (file->module != nullptr ? "module '" + declared + "'" : declared)));
        } else {
          program.diagnostics.push_back(
              Diagnostic::error(import->span, not_found_message(inputs, identity)));
        }
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

auto dependents_of(const Program& program) -> std::unordered_map<const ModuleInfo*, std::vector<ModuleInfo*>> {
  std::unordered_map<const ModuleInfo*, std::vector<ModuleInfo*>> dependents;
  for (const auto& module : program.modules) {
    for (auto* imported : module->imports) {
      dependents[imported].push_back(module.get());
    }
  }
  return dependents;
}

auto topological_order(Program& program) -> ModuleSet /* modules left unordered: on or behind a cycle */ {
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
// Repeatedly dropping modules nothing left depends on strips the
// acyclic dependents; walking first-import edges from the lexically
// smallest remaining module then closes a cycle, which is reported once
// and removed.  Deterministic, and the trace names only cycle members.
// ---------------------------------------------------------------------------

void strip_acyclic_dependents(ModuleSet& remaining) {
  for (bool changed = true; changed;) {
    changed = false;
    for (auto it = remaining.begin(); it != remaining.end();) {
      auto* candidate = *it;
      bool depended_on = std::ranges::any_of(remaining, [&](const ModuleInfo* other) {
        return std::ranges::contains(other->imports, candidate);
      });
      if (depended_on) {
        ++it;
      } else {
        it = remaining.erase(it);
        changed = true;
      }
    }
  }
}

auto first_import_within(const ModuleInfo& module, const ModuleSet& remaining) -> ModuleInfo* {
  auto it = std::ranges::find_if(module.imports,
                                 [&](ModuleInfo* imported) { return remaining.contains(imported); });
  return it == module.imports.end() ? nullptr : *it;
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
  for (strip_acyclic_dependents(remaining); !remaining.empty();
       strip_acyclic_dependents(remaining)) {
    std::vector<ModuleInfo*> path;
    std::map<const ModuleInfo*, size_t> position;
    auto* current = *remaining.begin();
    while (!position.contains(current)) {
      position[current] = path.size();
      path.push_back(current);
      current = first_import_within(*current, remaining);
    }
    std::vector<ModuleInfo*> cycle(path.begin() + static_cast<std::ptrdiff_t>(position[current]),
                                   path.end());

    std::string trace;
    for (const auto* member : cycle) {
      trace += member->display + " -> ";
    }
    trace += current->display;
    program.diagnostics.push_back(
        Diagnostic::error(import_span(*cycle.front(), *cycle[1 % cycle.size()]),
                          "import cycle: " + trace));
    for (auto* member : cycle) {
      remaining.erase(member);
    }
  }
}

// ---------------------------------------------------------------------------
// Entry (§7.7)
// ---------------------------------------------------------------------------

void select_entry(Program& program, const GraphInputs& inputs) {
  if (!inputs.root_display.empty()) {
    auto root = std::ranges::find_if(program.files, [&](const auto& file) {
      return file->display_path == inputs.root_display;
    });
    program.entry = root == program.files.end() ? nullptr : (*root)->module;
    return;
  }
  if (inputs.entry) {
    program.entry = program.module_named(*inputs.entry);
    if (program.entry == nullptr) {
      program.diagnostics.push_back(
          Diagnostic::error(Span{}, "entry module '" + *inputs.entry + "' is not in the program"));
    }
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
  } else if (candidates.size() > 1) {
    std::string names;
    for (const auto* candidate : candidates) {
      names += (names.empty() ? "" : ", ") + candidate->display;
    }
    program.diagnostics.push_back(Diagnostic::error(
        Span{}, "several modules declare `fn main` (" + names + "); select one with --entry"));
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
