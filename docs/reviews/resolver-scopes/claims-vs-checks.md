# resolver-scopes — claims vs checks

Every field this branch adds to a record the compiler carries between
passes, and every identity function it introduces, with the test that
holds it to its claim.  A row with no test says why.

Scope: the delta between `origin/andrew/feat/module/graph-discovery` and
this branch (Task 31 D2).

## `Symbol` (`compiler/frontend/resolve/symbol.h`)

| Field | Claim | Verified by |
|---|---|---|
| `module` | The module whose declaration produced the symbol; null for compiler builtins and for resolution without a `Program`. | `resolve_test.cpp` `resolve_modules/modules_own_their_scopes_and_symbols` (a local's owner), `.../qualified_function_resolves_through_the_import` (an export's owner), `.../a declaration shadows an overloaded prelude name` (owner distinguishes the shadowing declaration from the prelude's). Builtins carry null: they are made by `ctx_.make_symbol` in `populate_builtins`, outside `new_symbol`, which is the only stamping site. |
| `kind == SymbolKind::Module` | An import binding; `decl` is the bound `ModuleInfo`, null while the graph reported the module missing. | `resolve_test.cpp` `.../qualified_function_resolves_through_the_import` asserts the head symbol's kind and `decl_as_module()->display`; `.../an import binding shadows an overloaded prelude name` asserts the binding wins lookup as a callee. A null `decl` is exercised by `.../import_binding_collides_with_an_import` (`other::math` binds with no resolved target and does not crash). |

## `ModuleInfo` (`compiler/frontend/module/program.h`)

| Field | Claim | Verified by |
|---|---|---|
| `scope` | The module's lexical scope: its own declarations (except a prelude module's, which go to the shared prelude scope) plus its import bindings. | `resolve_test.cpp` `.../modules_own_their_scopes_and_symbols` (one module's name is absent from another's scope); `import_binding_locality` suite (a prelude module's binding is private to it). |
| `exports` | What a qualified path reaches: this module's own declarations.  The same scope as `scope` except for a prelude module. | `prelude_qualified_exports` and `per_module_overload_exports` suites in `resolve_test.cpp`; `.../import_bindings_are_not_reexported`; `.../a type path binds the exported type at its own segment`. |

## `Scope` (`compiler/frontend/resolve/scope.h`)

| Field / function | Claim | Verified by |
|---|---|---|
| `ScopeKind::{Builtins, Prelude, Module}` | The scope shape is builtins → prelude group → one scope per module (CONTRACT_MODULE_SYSTEM.md §7). | `resolve_test.cpp` `.../builtins_cannot_be_redeclared_anywhere`, `.../prelude_names_are_visible_unqualified_and_shadowable`, `.../modules_own_their_scopes_and_symbols`. |
| `find_overloads` / `has_overloads` | Innermost-first over the whole chain: a nearer binding of the name — ordinary or overloaded — shadows an outer overload set. | `resolve_test.cpp` `.../a declaration shadows an overloaded prelude name`, `.../a shadowing declaration hides every arity of the outer set`, `.../an import binding shadows an overloaded prelude name`. |
| `Scope::range` on the builtins and prelude scopes | Spans the whole program so offset-based scope lookup descends through them to the module scope containing a position. | `service_test.cpp` `.../navigation_and_completion_answer_over_the_examples`, which asks `completions` at an offset in each example's own module of a program whose prelude group is the real stdlib; `query_completions` reaches it through `scope_at_offset`. |

## `TypeChecker::MethodKey` (`compiler/frontend/typecheck/type_checker.h`)

| Field | Claim | Verified by |
|---|---|---|
| `owner` | The module whose `extend` block introduced the method, and the only module the method is in the method set of; null means every module sees it (a type's own methods, a prelude `extend`, or a program with no module identities). | `typecheck_test.cpp` `module_extend_scoping` suite: a sibling module does not see a non-prelude `extend`, the declaring module does, and a prelude `extend` is visible everywhere. |

## Identity functions

| Function | Claim | Verified by |
|---|---|---|
| `is_prelude_intrinsic` (`resolve.h`) | Names the generic intrinsic family, mangled specializations included; only a prelude module may declare one. | `resolve_test.cpp` `.../the intrinsic family is the prelude's to declare` (entry module and imported module alike). The mangled form (`size_of$i32`) is not covered by a resolver test: no source can declare it — the `$` is not a legal identifier character — and the backend's use of the predicate is Task 31 D4's. |
| `reject_deep_path` (`resolve.cpp`) | The closed set of qualified forms bottoms out at a type's member in expression position and at the exported type in type position; anything deeper is an error in either position. | `resolve_test.cpp` `.../deeper_path_through_a_binding_is_an_error` (expression) and `.../a deeper path in type position is an error` (type). |
| `collect_assembly_diagnostics` (`tools/playground/compiler_service/pipeline.h`) | A failed assembly reports every file's lex/parse diagnostics merged with the graph's, located entries in program order (Task 31 §8.4). | `service_test.cpp` `.../assembly_diagnostics_come_back_in_program_order`, over `analyze` and `run`. |
