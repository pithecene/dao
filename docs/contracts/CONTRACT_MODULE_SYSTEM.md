# CONTRACT_MODULE_SYSTEM

Status: normative
Scope: module identity, imports, exports, `extend` scoping, the
prelude, the entry module, and determinism of multi-file programs
Authority: language semantics

## 1. Purpose

This contract freezes how a Dao program is assembled from source files
and how names cross file boundaries.  It applies to every Dao compiler
in this repository; the bootstrap compiler's parity with it is tracked
in `bootstrap/README.md`.

Syntax is frozen in `CONTRACT_SYNTAX_SURFACE.md` (module declarations,
namespace qualification).  This contract does not restate syntax.

## 2. Module identity

1. Every source file declares exactly one module; that declaration is
   the file's identity.
2. Two files declaring the same module identity are an error.  Modules
   are not merged across files.
3. File paths never define module identity.  A compiler may use a path
   convention to locate a candidate file for an import; the located
   file's declaration must equal the imported identity, and a mismatch
   is an error naming both.

## 3. Imports

1. `import a::b` binds the single local name `b` to module `a::b` in
   the top-level scope of the importing module.
2. An import exposes the imported module's name only.  None of its
   declarations enter the importing scope unqualified.
3. A binding introduced by an import must not collide with a top-level
   declaration or another import binding in the same module; a
   collision is an error.
4. An import binds exactly one segment.  A qualified reference through
   a binding is limited to the forms in §6.
5. The import graph of a program must be acyclic.  A cycle is an error
   whose diagnostic names the cycle.

## 4. Exports and same-module access

1. Every named top-level declaration of a module — function, class,
   enum, type alias, concept — is exported from that module.
2. `extend` blocks are unnamed and are not exported.
3. Inside a module, its own top-level declarations are visible
   unqualified, with forward references, regardless of how many files
   the compiler processes.

## 5. `extend` scoping

1. Methods introduced by an `extend` block participate in method-set
   lookup within the declaring module.
2. Importing a module does not import its `extend` methods.
3. The prelude (§7) is the only exception to rule 2.

## 6. Qualified forms

Given `import a::b` binding `b`:

| Form | Meaning |
|---|---|
| `b::f(...)` | exported function `f` of `a::b` |
| `b::T` (type position) | exported type `T` of `a::b` |
| `b::E::V` | variant `V` of exported enum `E` of `a::b` |
| `b::T::m(...)` | static method `m` of exported type `T` of `a::b` |

Unqualified `T::m` and `E::V` on same-module or prelude types are
unchanged.  Any deeper path through a binding is an error.

## 7. Prelude

1. The compiler designates a set of modules as the prelude.  At
   present the prelude is every module under `stdlib/core/` and
   `stdlib/io/`.  Changing the set is a contract change.
2. Prelude modules are part of every module's lexical environment.
   Their exported declarations are visible unqualified in every module,
   and their `extend` methods participate in method-set lookup in every
   module.
3. Prelude modules see one another unqualified, as one namespace.
4. A module's own top-level declaration shadows a prelude declaration
   of the same name; lookup is innermost-first.  No diagnostic is
   required.
5. Prelude modules have ordinary identities (`core::vector`,
   `io::file`) and may also be imported and referenced qualified;
   both paths resolve to the same declarations.

Scope order, outermost to innermost:

```
compiler builtins → prelude → module → function / block / lambda
```

## 8. Entry module and program entry

1. Every program that is built into an executable has exactly one
   entry module.
2. When the program is given as a root file, the entry module is the
   module declared by that file.
3. When the program is given as an explicit file set, the entry module
   is the one named by the driver's entry option; if none is named and
   exactly one module declares `fn main`, that module is the entry;
   otherwise it is an error.
4. Entry selection is a function of the file set and the driver
   options only, never of the order in which files are supplied.
5. The entry module's `fn main` is the program entry.  A `fn main` in
   any other module is an ordinary function.

## 9. Determinism

For a fixed file set and a fixed entry selection, every compiler
output — diagnostics, IR dumps, object code — is independent of the
order in which files are supplied and of filesystem traversal order.

## 10. Not covered

The following are not part of this contract and must not be inferred
from it:

- visibility modifiers (`language_vision.md` lists tiers as not frozen)
- selective, glob, or aliased imports; re-exports
- packages and workspaces
- cross-module `extend` visibility, coherence, or orphan rules
- separate compilation and caching
- the symbol naming or ABI of Dao-defined functions
  (`CONTRACT_C_ABI_INTEROP.md` §8 reserves an export contract)

## 11. Relationship to other contracts

- `CONTRACT_SYNTAX_SURFACE.md` — module and import syntax; `::` versus
  `.`; "import binds the last segment"
- `CONTRACT_COMPILER_PHASES.md` and `CONTRACT_REPOSITORY_LAYOUT.md` —
  `compiler/frontend/module/` owns source sets, module graphs, and the
  program-wide source map
- `CONTRACT_C_ABI_INTEROP.md` — `extern fn` names are exact; Dao
  symbol export is reserved
- `CONTRACT_RUNTIME_ABI.md` — runtime hooks are prelude-visible
  `extern fn` declarations under the `__dao_` prefix
