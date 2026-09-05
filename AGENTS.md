# AGENTS.md — Dao Guardrails

This file defines non-negotiable working rules for Dao.

## Core Principles

- Clarity over cleverness
- Semantics over novelty theater
- Explicit structure over hidden magic
- Minimal syntax with strong visual taxonomy
- Spec-first changes before implementation-first changes

## Scope Discipline

Agents must not:
- invent subsystems beyond the declared repository layout
- redesign syntax casually or opportunistically
- add build tools, CI, or language backends without explicit request
- create new top-level directories without updating `CLAUDE.md` and
  `docs/ARCH_INDEX.md`
- blur normative docs and explanatory docs

## Syntax Discipline

The current frozen surface assumptions are:
- indentation-significant blocks
- `if` / `while` / `for` use `:`
- block-bodied functions use no token after the return type
- expression-bodied functions use `->`
- `extern fn` declares externally-provided functions with no body
- lambdas use `->`
- `mode <name> =>` enters an execution/safety mode
- `resource <kind> <name> =>` binds a scoped resource domain
- `|>` is a first-class pipeline operator
- `module a::b` declares a file's module identity
- `import a::b` binds the last segment `b` as a local module name
- `enum` declares fieldless closed variants (classification only)
- `enum class` declares closed structured variants with named fields
  (see `docs/contracts/ADR_ENUM_CLASS.md`)

Agents must not introduce alternate spellings for these without an
explicit syntax revision task.

## Dependency Discipline

- All dependencies are managed via **Conan 2.x** in manifest mode
  (`conanfile.txt` or `conanfile.py`).
- Agents must **never** install, suggest, or attempt to install system
  packages (e.g. via `zypper`, `apt`, `dnf`, `brew`, or any other
  system package manager). No exceptions.
- If a dependency is not available in Conan, escalate — do not
  work around it with system packages.

## Repository Discipline

- `docs/contracts/` contains binding contracts only
- `docs/` normal-case prose is explanatory only
- `spec/` holds grammar fragments, reference examples, and syntax probes
- `examples/` holds human-readable Dao snippets, not normative law
- `testdata/` holds fixtures for future parser/compiler tests
- `ai/skills/` holds Bonsai skills only

## Tooling Sync Discipline

`compiler/analysis/tooling_surface.h` is the single source of truth for
what the compiler exposes to editors: token kinds, payload shapes, and
service routes.  Agents must:
- edit the table, not a consumer, when adding a kind, field, or route
- regenerate `tools/playground/frontend/src/generated/tooling_surface.ts`
  (`task gen-tooling-surface`) in the same change
- land the frontend side of a new route or kind in the same PR
- add or update an example under `examples/` when a language feature
  lands; `playground_service_test` keeps every example runnable and
  fully classified

## Code Quality Discipline

Every diff must be evaluated in context of the larger project, not
in isolation.  Passing tests is necessary but not sufficient.

### Pre-commit self-review gate

Before committing, verify:

1. **Readability without history** — a stranger must understand the
   code without reading the PR description, task tracker, or commit
   log.  No task numbers in comments (`D1a`, `D3`, `D5` are opaque).
   Names describe what things are, not which ticket introduced them.
2. **DRY** — if the same pattern appears a third time, stop and
   extract a helper before writing the third copy.  Check whether
   the pattern already exists elsewhere in the codebase.
3. **Data structure fitness** — adding a field to a class is a
   signal to evaluate the design, not a solution.  If a struct has
   >8 fields, the design is wrong.  If positional constructors
   exceed 6 arguments, introduce named initialization, a builder,
   or sub-structs.
4. **Style consistency** — look at how neighboring code handles the
   same problem before inventing a new pattern.  Match the existing
   conventions for naming, error handling, and control flow.
5. **Workarounds require escalation** — if writing a comment that
   says "workaround" or "temporary," that is a flag to escalate to
   the user, not a license to ship it.  Document the root cause,
   the workaround, and the conditions under which it can be removed
   — all in one place, not scattered across files.
6. **No silent regressions** — every function that can fail must
   have an observable failure path (diagnostic, error return, or
   assert).  Returning a valid-looking default (0, nullptr, empty)
   on failure is a bug, not error handling.
7. **Algorithmic awareness** — O(n²) in a loop over symbols is not
   acceptable when O(n) or O(1) alternatives exist.  Linear scans
   are fine for small n with a documented size bound; they are not
   fine as the default lookup strategy.

### Naming rules

- Variables describe what they hold, not their type or scope depth
- Functions describe what they do, not which pass calls them
- Test fixtures describe the scenario, not their sequential number
- Comments explain why, not what — the code shows what

### Abstraction rules

- Every abstraction must earn its keep: if removing it would require
  copy-pasting code in ≥3 places, it belongs; otherwise it does not
- Do not introduce wrappers that add no semantic value
- Do not duplicate type definitions across subsystems — promote
  shared types to the appropriate substrate layer

### Review posture

Agents must review their own output as if they were the reviewer,
not the author.  The question is not "does this work" but "would I
approve this PR if someone else wrote it."

## Priority Order

1. Correctness of contracts
2. Structural coherence
3. Readability
4. Convenience
5. Extensibility
