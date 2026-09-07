# Changelog

Behaviour changes that a Dao program can observe, newest first.
Contracts remain the normative source; this file records what changed
and when, not what is guaranteed.

## Unreleased

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
  file set rather than of the order or spelling of the paths given
  (`CONTRACT_MODULE_SYSTEM.md` §8.4).
