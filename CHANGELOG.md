# Changelog

Behaviour changes that a Dao program can observe, newest first.
Contracts remain the normative source; this file records what changed
and when, not what is guaranteed.

## Unreleased

### Fixed

- **A variant is reached with `::`.**  `Enum.Variant` — a field access
  on a type name — was accepted as variant access in expressions,
  constructor callees, and match patterns, against the enum-class ADR,
  and let a payload variant be constructed positionally past the ADR's
  named-construction rule.  The type checker now rejects it with
  `variant access uses '::': Enum::Variant`; a payload variant is
  constructed by field name, `Enum::Variant(field = value)`.  Every
  source in the repository is on the qualified spelling.

- **A loop's temporaries take one stack slot per site, not one per
  iteration.**  The backend allocated the slot for a string operand, a
  string argument, a struct passed or returned through the C ABI, or an
  enum value where the value was used, and a slot allocated inside a
  loop lives until the function returns; a long loop overflowed the
  stack (the bootstrap lexer, at 8 MiB, on its largest program).  Every
  such slot is now allocated in the function's entry block and reused.

### Changed

- **A user module may shadow a prelude name.**  A top-level declaration
  in a non-prelude module now silently shadows a prelude declaration of
  the same name, and lookup proceeds innermost-first, as it already did
  for locals.  Previously the prelude and user code shared one scope, so
  the same collision was reported as a duplicate top-level declaration.
  Shadowing stops at the prelude: compiler builtins and predeclared
  names (`i32`, `string`, `void`, `Generator`, `null_ptr`, `ptr_cast`)
  cannot be redeclared by any module.  See `CONTRACT_MODULE_SYSTEM.md`
  §7.6.

### Added

- **Multi-file programs.**  A program is a set of files, discovered from
  a root file's imports or given explicitly, compiled together into one
  offset space.  `daoc <command> <root.dao>` discovers; `--source` names
  files explicitly; `--entry` selects the entry module.  File order,
  diagnostic order, and the default output name are functions of the
  file set rather than of the order of the paths given
  (`CONTRACT_MODULE_SYSTEM.md` §9; the file order itself is Task 31
  §8.4).
