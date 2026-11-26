# CONTRACT_MODULE_SYSTEM

Status: normative
Scope: module identity, imports, exports, `extend` scoping, the
prelude, the entry module, and determinism of multi-file programs
Authority: language semantics

## 1. Purpose

This contract freezes how a compiler assembles a Dao program from source
files and how names cross file boundaries.  It states the language rules
and applies to every Dao compiler in this repository.  §12 records which
compiler conforms to which section and names the task that closes each
gap.  Until that task lands, the contract still governs and the gap
remains a documented non-conformance rather than a permission.

`CONTRACT_SYNTAX_SURFACE.md` freezes module declarations and namespace
qualification.  This contract does not restate syntax.

## 2. Module Identity

1. Every source file declares one module, and that declaration forms the
   file's identity.
2. Two files declaring the same module identity are an error.  The
   compiler never merges modules across files.
3. File paths never define module identity.  A compiler may use a path
   convention to locate a candidate file for an import.  The located
   file's declaration must equal the imported identity, and a mismatch
   is an error naming both.

## 3. Imports

1. `import a::b` binds the single local name `b` to module `a::b` in
   the top-level scope of the importing module.
2. An import exposes the imported module's name only.  None of its
   declarations enter the importing scope unqualified.
3. A binding introduced by an import must not collide with a top-level
   declaration or another import binding in the same module.  Such a
   collision is an error.
4. An import binds one segment.  §6 lists the only valid qualified
   references through an import binding.
5. The import graph of a program must be acyclic.  A cycle is an error
   whose diagnostic names the cycle.

## 4. Exports and Same-Module Access

1. A module exports every named top-level declaration it contains
   (function, class, enum, type alias, and concept).
2. `extend` blocks carry no name, and no module exports them.
3. Inside a module, its own top-level declarations are visible
   unqualified, with forward references, regardless of how many files
   the compiler processes.

## 5. `extend` Scoping

1. Methods and conformances introduced by an `extend` block participate
   in method-set lookup and generic bound satisfaction within the
   declaring module.
2. Importing a module does not import its `extend` methods or
   conformances, except for prelude `extend` blocks (§7.2) and canonical
   key conformances (rule 3).
3. Conformances to the prelude's `Equatable` and `Hashable` concepts
   (`core::equatable::Equatable` and `core::hashable::Hashable`,
   matched by resolved declaration rather than name spelling) are
   canonical properties of the conforming type:
   - Only a prelude module or a nominal type's declaring module may
     extend that type as prelude `Equatable` or prelude `Hashable`.
     Extending a builtin, predeclared, or imported nominal type as
     either concept outside its declaring module is an error.
   - A nominal type's same-module conformance to prelude `Equatable` or
     prelude `Hashable` travels with the type across modules.  Every
     module sees that conformance and its `eq` and `hash` methods, and
     prelude containers such as `HashMap<K, V>` always select it.
   - Every type conforming to prelude `Hashable` must also conform to
     prelude `Equatable`.  An explicit prelude `Equatable`
     implementation (`as Equatable:` or `extend ... as Equatable:`)
     suppresses field-wise derivation of prelude `Hashable` unless the
     module also implements prelude `Hashable` explicitly.
   - Any other concept, including a non-prelude concept named
     `Equatable` or `Hashable`, follows rules 1 and 2 and stays scoped
     to its declaring module.

## 6. Qualified Forms

Given `import a::b` binding `b`:

| Form | Meaning |
|---|---|
| `b::f(...)` | exported function `f` of `a::b` |
| `b::T` (type position) | exported type `T` of `a::b` |
| `b::E::V` | variant `V` of exported enum `E` of `a::b` |
| `b::T::m(...)` | static method `m` of exported type `T` of `a::b` |
| `b::C` (conformance position) | exported concept `C` of `a::b`, in `as`, `deny`, and `extend ... as` |

Unqualified `T::m` and `E::V` on same-module or prelude types keep their
meaning.  Any deeper path through a binding is an error.

A conformance position names one concept: `as b::C:`, `deny b::C`, and
`extend T as b::C:`.  An unqualified name does not reach through an
import binding (§3).  Without `b::C` in conformance positions, a module
could state a bound `b::C` that no external type could ever satisfy.

## 7. Prelude

1. The compiler designates a set of modules as the prelude.  At
   present the prelude comprises every module under `stdlib/core/` and
   `stdlib/io/`.  Changing the set requires a contract change.
2. Prelude modules belong to every module's lexical environment.
   Their exported declarations are visible unqualified in every module,
   and their `extend` methods participate in method-set lookup in every
   module.
3. Prelude modules see one another unqualified, as one namespace.
4. A module's own top-level declaration shadows a prelude declaration
   of the same name, and lookup proceeds innermost-first.  The compiler
   emits no diagnostic for this shadowing.
5. Prelude modules have ordinary identities (`core::vector`,
   `io::file`).  A module may also import them and reference them
   qualified, and both paths resolve to the same declarations.
6. No module, prelude included, may shadow or redeclare compiler
   builtins and predeclared names (the outermost scope).  A top-level
   declaration bearing such a name is an error.  Rule 4 applies to
   prelude declarations only.
7. Only prelude modules may declare names carrying the `__dao_`
   prefix, which `CONTRACT_RUNTIME_ABI.md` reserves for runtime hooks.
   Such a declaration in any other module is an error.
8. Only prelude modules may declare the intrinsic family `size_of`,
   `align_of`, and `copy_out`.  The backend replaces the first two with
   inline IR, and the monomorphizer replaces `copy_out` with a per-type
   domain copy (`CONTRACT_EXECUTION_CONTEXTS.md`, law 7).  Declaring any
   of these three names outside the prelude is an error.  A class's own
   `copy_out` method is distinct and permitted, and rule 4 does not
   apply to these intrinsics.  No module declares the operations of
   `Ptr<T>` (`ADR_RAW_POINTER_SURFACE.md`), which are compiler-standard
   methods whose lowering primitives carry no source name.

Scope order, outermost to innermost:

```text
compiler builtins → prelude → module → function / block / lambda
```

Lookup proceeds innermost-first, and the compiler must check
declarations in the module or prelude scope against the builtins scope
(rule 6) because scope nesting alone does not enforce it.

## 8. Entry Module and Program Entry

1. Every executable program has one entry module.
2. When the user supplies a root file, that file's declared module is
   the entry module.
3. When the user supplies an explicit file set, the driver's entry
   option names the entry module.  With no entry option, a single module
   declaring `fn main` becomes the entry, and any other count is an
   error.
4. Entry selection depends only on the file set and driver options,
   never on the order in which the caller supplies files.
5. The entry module's `fn main` is the program entry.  A `fn main` in
   any other module is an ordinary function.

## 9. Determinism

For a fixed file set and entry selection, every compiler output
(diagnostics, IR dumps, and object code) is independent of file argument
order and filesystem traversal order.

## 10. Not Covered

This contract does not cover the following items, and readers must not
infer rules for them here:

- visibility modifiers (`language_vision.md` lists tiers as not frozen)
- selective, glob, or aliased imports, or re-exports
- packages and workspaces
- general cross-module `extend` visibility, coherence, or orphan rules
  beyond prelude `Equatable` and `Hashable` (§5)
- separate compilation and caching
- the symbol naming or ABI of Dao-defined functions
  (`CONTRACT_C_ABI_INTEROP.md` §8 reserves an export contract)

## 11. Relationship to Other Contracts

- `CONTRACT_SYNTAX_SURFACE.md`: module and import syntax, `::` versus
  `.`, and single-segment import binding
- `CONTRACT_COMPILER_PHASES.md` and `CONTRACT_REPOSITORY_LAYOUT.md`:
  `compiler/frontend/module/` owns source sets, module graphs, and the
  program-wide source map
- `CONTRACT_C_ABI_INTEROP.md`: `extern fn` names are exact, and the
  contract reserves Dao symbol export
- `CONTRACT_RUNTIME_ABI.md`: runtime hooks are prelude-visible
  `extern fn` declarations under the `__dao_` prefix

## 12. Conformance Status

| Section | Host compiler | Bootstrap compiler |
|---|---|---|
| §2 identity | conforms (Task 31) | conforms (Task 25, explicit file lists only, no path convention) |
| §3 imports | conforms (Task 31) | conforms (Tasks 25 and 26) |
| §4 exports, same-module access | conforms (Task 31) | conforms (Task 26) |
| §5 `extend` scoping | conforms (Task 31, including canonical prelude `Equatable` and `Hashable`) | conforms (Task 26 §6.5, including canonical prelude `Equatable` and `Hashable`) |
| §6 qualified forms | conforms (Task 31) | `b::f`, `b::T`, and `b::E::V` conform (Task 27). `extend ... as b::C` and `<T: b::C>` conform. Class-body `as b::C`, `deny b::C`, and `b::T::m` await Task 33. |
| §7 prelude | conforms (Task 31) | partial (Tasks 38 and 41): scope order, shadowing, identity, reserved names, prelude `extend` visibility, and derived conformance conform. Prelude files load partially, and probe columns omit full `hir`, `mir`, and `llvm` prelude runs. |
| §8 entry module | conforms (Task 31) | not implemented (Task 33): no driver or entry concept, and MIR flattens all functions |
| §9 determinism | conforms (Task 31, `multifile_build_test` compares two file orders' IR) | graph construction conforms (Task 25 §8), and program-level output determinism awaits Task 33 |

"Task N" in a cell means that compiler does not yet implement the
section and Task N delivers it.  `bootstrap/README.md` mirrors the
bootstrap column, and any bootstrap change that moves a row must update
both files together.
