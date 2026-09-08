# graph-discovery — review round 1

- Head: `6e497e3c5a9a6a4db993fda98e6097f636e74c87`
- Base: `origin/main` (`f9cb79733198e74c08c637d38bb598a4d1663e53`)
- Reviewer: `gpt-5.6-sol` (effort `high`)
- Reviewer verdict: **REQUEST_CHANGES**
- Driver decision: **changes** (blocking 4, major 2, minor 1)

## Checks on this head

- PASS `task build` (exit 0)
- PASS `/tmp/dao-fmt-changed.sh` (exit 0)
- PASS `task test` (exit 0)

## Reviewer summary

Reviewed the complete base-to-head diff, handoff, module-system contract, Task 31 specification, graph/program/driver/service implementations, and all relevant tests. Confirmed the requested HEAD and base identities. The supplied build and test checks pass, but targeted read-only driver execution exposed a root-role failure and abort. No claims-vs-checks record exists, and the new CLI option surface lacks process-level coverage. Blocking identity, cycle-safety, and determinism defects remain.

## Findings

### R1-F1 — blocking — cycle verifier assumes every residual node still has a residual edge

- Location: `compiler/frontend/module/module_graph.cpp` 273-295
- Claim: Cycle reporting can dereference null instead of diagnosing a graph containing two cycles connected through an acyclic dependency.
- Evidence: After reporting a cycle, lines 294-295 remove its members. strip_acyclic_dependents removes nodes with no remaining dependents, not nodes with no remaining imports. For cycle A importing bridge C, C importing cycle B, removing B retains C because A depends on it even though C then has no remaining import. first_import_within returns nullptr at line 282, and the next iteration dereferences *current. The cycle tests at module_graph_test.cpp:120-165 cover only a single cycle and an acyclic importer.
- Required test: Add a graph with two cycles connected through an acyclic dependency, arrange traversal to enter the second cycle first, and assert deterministic diagnostics for both cycles without an abort under input-order permutations.

### R1-F2 — blocking — canonical deduplication discards the requested file's root role

- Location: `compiler/frontend/module/program.cpp` 182-200,225-240
- Claim: When the requested root is also loaded as a prelude file, canonical deduplication retains only its prelude SourceInput, leaving no user file and preventing root entry selection.
- Evidence: add_prelude registers the canonical path first; add(root_file, pending.back()) then discards the non-prelude root while graph.root_display retains its different absolute display spelling. select_entry cannot find that spelling. I ran `daoc check stdlib/core/vector.dao`; it exited 0 and printed `ok` despite the required root entry having no main. `daoc tokens stdlib/core/vector.dao` exited 134 because user_files().front() asserted on an empty vector.
- Required test: Add loader and process-level tests with a root under options.stdlib_root/core, asserting its root/user role is preserved, it becomes Program.entry, missing main is diagnosed, and tokens does not index an empty user_files result.

### R1-F3 — blocking — path-identity verifier is bypassed by an unrelated matching module

- Location: `compiler/frontend/module/module_graph.cpp` 143-157
- Claim: A located file's declaration is checked against the requested import identity only when no module with that identity already exists, so an earlier-root mismatch is silently accepted when the desired identity is preloaded elsewhere.
- Evidence: resolve_edges first calls program.module_named(identity). The located candidate and declared_identity comparison occurs only inside `target == nullptr`. Thus if the root directory's `core/x.dao` declares `core::wrong` while the prelude already supplies `core::x`, the import binds the prelude module and no required path/declaration mismatch is emitted. Existing mismatch tests have no separately loaded target with the requested identity.
- Required test: Load a valid prelude module and an earlier search-root candidate at the same mapped path declaring another identity; assert the earlier candidate always produces the path/declaration mismatch even though the requested identity exists elsewhere.

### R1-F4 — blocking — canonical identity comparison has an input-order-dependent equality case

- Location: `compiler/driver/pipeline.h` 101-116
- Claim: ProgramRequest::primary_file is input-order dependent when two source spellings resolve to the same canonical file, allowing equivalent explicit sets to select different build output paths.
- Evidence: primary_file projects paths through canonical_or_self but returns the original path selected by min_element. Equal canonical projections cause min_element to return the first input. cmd_build derives and prints the executable path from that original spelling at main.cpp:201 and 227-268. The explicit loader itself deduplicates these aliases canonically, so reversing them leaves the program unchanged while potentially changing the output name, especially for symlink aliases with different stems.
- Required test: Build the same explicit set containing two aliases of one source in both source orders and assert identical selected output path and stdout; include symlink aliases with different names.

### R1-F5 — major — diagnostic aggregation is phase-batched and prematurely terminated

- Location: `tools/playground/compiler_service/analyze.cpp` 179-197
- Claim: A graph error makes analyze return before collecting lex/parse diagnostics, and collected program diagnostics are not globally ordered by file and offset.
- Evidence: Lines 180-182 return immediately on any program graph error; file lex and parse diagnostics are collected only afterward. collect_program_diagnostics in pipeline.cpp:163-173 preserves graph-construction order without sorting. The driver explicitly merges and sorts graph, lex, and parse diagnostics at compiler/driver/pipeline.cpp:117-135, but the service does not. The only added service test covers a missing-entry warning, not graph errors combined with source diagnostics or ordering.
- Required test: Add analyze and run requests containing recoverable parse diagnostics plus graph diagnostics across multiple files; assert all diagnostics are present and ordered by file id then offset.

### R1-F6 — major — new public CLI and ordered-root fields lack boundary tests

- Location: `compiler/driver/driver_test.cpp` 68-90
- Claim: No process-level regression test exercises `--source`, `--entry`, `--module-root`, `--stdlib-root`, mixed input rejection, or link inputs after explicit sources, despite the handoff claiming driver-level checks cover the CLI forms.
- Evidence: git diff shows no change to driver_test.cpp. Its sole test invokes only `daoc build <root>`. Repository test search found the flags only in production main.cpp; module_graph_test calls loaders directly. Its module-root test supplies one root and therefore does not verify ordered precedence, while the not-found test checks only that the message starts with `searched`, not which roots or their order.
- Required test: Add process-level driver tests for root discovery, ordered competing module roots, stdlib override, explicit sources and entry, mixed-mode rejection, single-file command rejection, and link passthrough after explicit sources; assert exact searched-root order.

### R1-F7 — minor — changed test code is not warning-clean

- Location: `compiler/frontend/module/module_graph_test.cpp` 367-368
- Claim: A designated initializer specifies ProgramOptions fields out of declaration order, relying on a compiler extension.
- Evidence: The supplied `task build` output reports that ISO C++ requires declaration order because module_roots is initialized before stdlib_root at line 368.
- Required test: Compile module_graph_test with reorder-init-list warnings treated as errors and keep the build warning-free.
