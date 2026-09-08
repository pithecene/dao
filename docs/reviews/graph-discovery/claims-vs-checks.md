# graph-discovery — claims vs checks

Every record field, identity function, and verifier this branch adds or
changes, and the test that holds it to its claim.  A row that says
*unverified* says why.

Suites: `module_graph_test` (`module_graph` / `entry_selection` /
`root_file_discovery`), `driver_test` (`driver` / `driver_cli`),
`playground_service_test`, `source_map_test`.

## Record types

### `SourceInput` (`frontend/module/program.h`)

| Field | Claim | Verified by |
|---|---|---|
| `display_path` | Names the file in diagnostics and orders it; a normalized spelling, relative to the display root when it has one | `the same root under two spellings gives one program`; `positions_in_a_two_file_program_name_their_files` |
| `text` | The file's bytes, unmodified | every test that reads a diagnostic offset back out of a file |
| `is_prelude` | The file is in the prelude group (§7.6), which is a property of where it lives, not of how it was named | `a prelude root with main is the entry and is not duplicated`; `prelude_main_is_not_a_candidate` |

### `ModuleInfo` (`frontend/module/program.h`)

| Field | Claim | Verified by |
|---|---|---|
| `display` | `a::b::c` from the file's `module` declaration; the module's whole identity | `two_files_one_import`; `module identity is indexed, not scanned` |
| `file` | The one file declaring this module | `discovers_transitively_and_the_root_is_the_entry` |
| `is_prelude` | Mirrors the file's; keeps prelude modules out of entry selection | `prelude_main_is_not_a_candidate` |
| `declares_main` | A top-level `fn main` **with a body**; an `extern fn main` names an entry defined elsewhere and does not count | `an extern main declares no entry`; `several_mains_without_entry_is_an_error` |
| `imports` | Resolved edges in declaration order, duplicates removed, unresolved ones absent | `two_files_one_import`; `missing_module_is_diagnosed_and_the_rest_still_builds`; `self_import_is_diagnosed` |

### `Program` (`frontend/module/program.h`)

| Field | Claim | Verified by |
|---|---|---|
| `files` | Prelude group first, then lexical by display path — a function of the set, not of input order | `input_order_does_not_change_file_ids_or_order`; `the same root under two spellings gives one program` |
| `modules` | One per file that declares an identity no earlier file claimed | `duplicate_module_is_diagnosed_where_the_second_file_declares_it`; `file_without_module_declaration_is_tolerated` |
| `by_display` | Every module indexed exactly once; the only lookup path, so resolution is not a scan per edge | `module identity is indexed, not scanned` |
| `topo_order` | Imports before importers; Kahn with the ready set in lexical module order; cycle members and their dependents absent | `three_module_chain_orders_imports_first`; `independent_modules_order_lexically`; `two_node_cycle_is_one_diagnostic_with_its_trace` |
| `entry` | §7.7: the root's module, else `--entry`, else the unique non-prelude `fn main` | `explicit_entry_wins`; `entry_not_in_program_is_an_error`; `a root that is also a prelude file keeps its root role` |
| `source_map` | Program-wide offset space, one range per file | `source_map_test` (D0, unchanged by this branch) |
| `diagnostics` | Load-level and graph-level (§8.5) problems; the graph is still built as far as it goes | `missing_module_is_diagnosed_and_the_rest_still_builds` |

### `ProgramDiagnostics` (`frontend/module/program.h`, new)

| Field | Claim | Verified by |
|---|---|---|
| `unlocated` | Diagnostics with a zero-length span — entry selection, position budget — which have nowhere to point | `a document without main is advised, not failed`; `a stdlib file compiled as the root keeps its root role` |
| `located` | Graph, lex, and parse diagnostics as one stream ordered by file then offset (§8.4), never batched by phase | `a graph error does not hide the parse errors around it` |

### `GraphInputs` / `LocatedImport` (`frontend/module/module_graph.h`)

| Field | Claim | Verified by |
|---|---|---|
| `entry` | Explicit-set entry by module name | `explicit_entry_wins`; `--source takes an explicit set and --entry names its entry` |
| `entry_policy` | Required for a delivered file set, Advisory for an editor buffer, Optional for a fragment | `an explicit file set without main is an error`; `a document without main is advised, not failed`; `an in_memory_program_without_main_needs_no_entry` |
| `root_display` | The spelling the **program kept** for the root, which is not the command line's when the root was already loaded as a prelude file | `a root that is also a prelude file keeps its root role` |
| `located` | What discovery loaded for which import identity, so the §8.3 declaration check has a file to name | `path_declaration_mismatch_is_diagnosed`; `an earlier root's mismatch is not excused by a module loaded elsewhere` |
| `searched_roots` | The §8.2 roots in search order | `the roots are named in the order they are searched`; `an unfound import names every root, in the order they were searched` |

### `ProgramOptions` / `ProgramRequest` (loader and driver)

| Field | Claim | Verified by |
|---|---|---|
| `stdlib_root` | Prelude source and last search root; empty loads no prelude | `--stdlib-root replaces the prelude the driver would load` |
| `module_roots` | Searched after the root's directory, in command-line order | `module_roots_are_searched_after_the_root_directory`; `module roots are searched in command-line order` |
| `entry` | Explicit-set entry module | `--source takes an explicit set and --entry names its entry` |
| `root` / `sources` | Two different programs; supplying both is rejected rather than silently resolved | `a root file given with --source is rejected` |

## Identity functions

| Function | Claim | Verified by |
|---|---|---|
| `module_display(segments)` | A module's identity is the `::`-joined declaration path and nothing else | `two_files_one_import`; `import_of_a_prelude_module_resolves` |
| `canonical_or_self(path)` | Two spellings of one file — `.`/`..`, relative, symlink — reduce to one string | `the same root under two spellings gives one program`; `an explicit set names its output by the set, not by the order` |
| `display_path_for(path, root)` | The display path is derived from the normalized path, never from the spelling the search happened to use | `the same root under two spellings gives one program` |
| `declared_identity(file)` | Taken from the file itself, so a file that lost registration to a duplicate is still named by what it declares | `a duplicate identity is still named by the mismatch it causes` |
| `Discovery::add` / `display_of` | Files are keyed by canonical path; the display kept is the first load's, and callers must ask rather than remember their own | `a root that is also a prelude file keeps its root role`; `a prelude root with main is the entry and is not duplicated` |
| `ProgramRequest::primary_file()` | Build outputs are named by the file the set names, not by the spelling that sorted first — equal canonical paths must not leave the choice to input order | `an explicit set names its output by the set, not by the order` |
| `assemble()` ordering key | `(is_prelude, display_path)`, a pure function of the set | `input_order_does_not_change_file_ids_or_order` |

## Verifiers

| Verifier | Claim | Verified by |
|---|---|---|
| `register_modules` duplicate check | A second file declaring a live identity is diagnosed at its own declaration, naming the first file | `duplicate_module_is_diagnosed_where_the_second_file_declares_it` |
| `resolve_edges` self-import | `import` of one's own identity is diagnosed and adds no edge | `self_import_is_diagnosed` |
| `resolve_edges` §8.3 declaration check | The file the mapping rule **found** must declare the imported identity; a module of that identity loaded elsewhere does not excuse it | `an earlier root's mismatch is not excused by a module loaded elsewhere`; `path_declaration_mismatch_is_diagnosed` |
| `resolve_edges` not-found | Names the roots searched in root-file mode, "in the program" otherwise | `not_found_names_the_roots_searched`; `missing_module_is_diagnosed_and_the_rest_still_builds` |
| `strip_to_cycle_kernel` | Leaves exactly the modules on a cycle: acyclic dependents and bridges into an already-reported cycle are both dropped, so every survivor still has an edge to walk | `two_cycles_joined_by_an_acyclic_bridge_are_both_reported`; `a bridge into a reported cycle is not itself reported`; `acyclic_dependent_is_excluded_from_the_trace` |
| `cycle_from` trace | One diagnostic per cycle, naming only its members, identical under every input permutation | `cycle_trace_is_deterministic_across_input_orders`; `two_cycles_joined_by_an_acyclic_bridge_are_both_reported` (all 120 permutations) |
| `select_entry` / `require_main` | A module selected as the entry by any rule must declare `fn main`; how loudly depends on `entry_policy` | `a named entry module without main is an error`; `a root file without main is an error`; `an explicit file set without main is an error` |
| `assembly_diagnostics` | One §8.4-ordered stream for the driver and the service alike; neither may return before it is complete, nor collect a file twice | `a graph error does not hide the parse errors around it` |
| `reported_file` (driver dumps) | The dumps report on the entry module's file — the root in root-file mode — and say so rather than indexing an empty user-file set | `a root file's imports are discovered from its own directory`; `a stdlib file compiled as the root keeps its root role` |

## Deliberately unverified

| Item | Why |
|---|---|
| `Program::source_map` field semantics | D0's, unchanged by this branch; `source_map_test` owns them |
| `report_cycles`' empty-walk branch | Unreachable while `strip_to_cycle_kernel` holds its invariant. It exists so a later change to the kernel degrades to a dropped module rather than a null dereference or a hang; a test would have to break the kernel to reach it |
| `EntryPolicy::Optional` silence | Asserted only as "no diagnostic" (`an in_memory_program_without_main_needs_no_entry`); there is no positive observation to make |
| Cross-module *resolution* of the discovered set | D2. Until then every file declares into one shared scope, so these fixtures call across files unqualified; the graph is what this branch claims, not the name binding over it |
