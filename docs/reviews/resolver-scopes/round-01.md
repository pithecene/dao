# resolver-scopes — review round 1

- Head: `39137a477025d800f236425e6cbf637088bfa284`
- Base: `origin/andrew/feat/module/graph-discovery` (`6e497e3c5a9a6a4db993fda98e6097f636e74c87`)
- Reviewer: `gpt-5.6-sol` (effort `high`)
- Reviewer verdict: **REQUEST_CHANGES**
- Driver decision: **changes** (blocking 3, major 2, minor 0)

## Checks on this head

- PASS `task build` (exit 0)
- PASS `/tmp/dao-fmt-changed.sh` (exit 0)
- PASS `task test` (exit 0)

## Reviewer summary

Reviewed HEAD 39137a477025d800f236425e6cbf637088bfa284 against 6e497e3c5a9a6a4db993fda98e6097f636e74c87. I read the adoption handoff, AGENTS.md, the Task 31 resolver requirements and normative module contract, the complete diff, all changed resolver tests, and affected type-checker, semantic-token, completion, and diagnostic consumers. No claims-vs-checks record exists. The supplied build, formatting, and 18-test results pass, but the required resolver surface remains incorrect in five classes.

## Findings

### R1-F1 — blocking — module provenance is recorded but not enforced

- Location: `compiler/frontend/resolve/resolve.cpp` 781-847
- Claim: Non-prelude extend methods remain visible across module boundaries, contrary to CONTRACT_MODULE_SYSTEM.md §5 and the explicit D2 tests required by Task 31 §18.3.
- Evidence: resolve_extend creates method symbols with new_symbol, which assigns Symbol::module, but registers no module-scoped method set. TypeChecker::check collects declarations from every file (type_checker.cpp:25-45), type_conforms_to scans every ExtendDecl without checking module ownership (639-650), and build_method_table installs every extend method into one global table (2474-2490). The added resolver tests contain no multi-module extend case.
- Required test: Add an integrated resolver/type-check test proving an extend on i32 in a non-prelude module is unavailable in a sibling/importing module while the same extend in a prelude module is available there.

### R1-F2 — blocking — auxiliary lookup bypasses lexical shadowing

- Location: `compiler/frontend/resolve/resolve.cpp` 576-582
- Claim: A local module declaration or import binding does not reliably shadow an overloaded prelude name when used as a callee; the resolver can bind the call to the outer prelude overload instead.
- Evidence: resolve_callee calls try_resolve_overload before normal name lookup. Scope::has_overloads and Scope::find_overloads (scope.h:108-129) recurse to the parent whenever the current scope has no local overload set, without stopping when lookup_local(name) contains a non-overload binding. Therefore a module-local ordinary function named f, or an import binding named f, can be bypassed by a matching overload set in the prelude. The existing shadowing test uses a non-overloaded prelude function and cannot expose this branch.
- Required test: Add resolver tests with an overloaded prelude f and, separately, a module-local function f and import binding f; assert lexical lookup honors each inner binding and never selects the prelude overload.

### R1-F3 — major — qualified-path validator handles only expression ASTs

- Location: `compiler/frontend/resolve/resolve.cpp` 1243-1265
- Claim: The resolver does not reject a qualified type path deeper than b::T through an import binding; it silently records only b and T and ignores remaining segments.
- Evidence: The expression path checks qn.segments.size() and diagnoses excessive depth at lines 1020-1025. The NamedType branch accepts every path with more than one segment, reads only segments[0] and segments[1], and has no depth check. The only added deeper-path test uses a QualifiedName expression with four segments; no test exercises a three-segment NamedType. This misses CONTRACT_MODULE_SYSTEM.md §6's closed set of qualified forms.
- Required test: Add a resolver test using an imported type position such as fn f(x: lib::T::Extra): void and require the imports-bind-one-segment diagnostic; also directly assert successful lib::T binds T at its segment offset.

### R1-F4 — major — consumer overwrites authoritative per-segment classification

- Location: `compiler/analysis/semantic_tokens.cpp` 886-888, 933-940
- Claim: Semantic tokens misclassify the type segment of b::T::m and b::E::V as use.module, and classify an imported enum variant as use.type rather than use.variant.
- Evidence: The resolver now records the exported type at its own offset and the method/variant at the member offset (resolve.cpp:1042-1069). When semantic token processing sees the module head, however, it schedules every intermediate segment as use.module; the pending classification is consumed before consulting that segment's resolved symbol. For enum variants, the final segment resolves to the enum Type symbol and resolve_use_category maps it to use.type. No multi-module semantic-token test covers either form.
- Required test: Add program-based semantic-token tests for b::T::m and b::E::V asserting use.module, use.type, and respectively use.function/use.variant on the correct individual segments.

### R1-F5 — blocking — diagnostics merged without canonical ordering

- Location: `tools/playground/compiler_service/analyze.cpp` 180-189
- Claim: On assembly failure, the playground analyze and run endpoints no longer emit diagnostics in the required file-id/offset order.
- Evidence: The new analyze branch appends every file's lexer/parser diagnostics and only afterward appends program/graph diagnostics. run.cpp:71-80 repeats the same pattern. collect_diagnostics and collect_program_diagnostics append without sorting (pipeline.cpp:163-187), so an early-file missing-import diagnostic can follow a later-file parse diagnostic. Task 31 §8.4 requires diagnostics in file_id order and then offset order. The changed service test checks only that lib.dao is named, not ordering.
- Required test: Add analyze and run service regressions containing an early-file import whose later-file target fails to parse; assert the combined located diagnostics are sorted by program offset/file order while retaining both root-cause diagnostics.
