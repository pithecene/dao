# Task 38 — A Variant Is Reached With `::`

Status: implementation spec
Phase: syntax correction, sequenced on stack #292 (after Task 37)
Scope: the host and bootstrap compilers spell enum variant access one
way — `Enum::Variant` — and the corpus follows

## 1. Objective

Retire the dot spelling of variant access.  `Enum::Variant` is the
form `docs/contracts/ADR_ENUM_CLASS.md` grants everywhere it shows a
variant — construction, a fieldless variant, a match pattern.
`Enum.Variant` is field access on a value in every other place the
language uses `.`; a variant is a member of the enum's namespace, so it
is reached the way a module's or a type's members are reached, with
`::`.

After this task the host parser still reads `Enum.Variant` as a field
access — that is what the syntax is — and the type checker rejects it
with a diagnostic that names the `::` spelling; the bootstrap pipeline
types and lowers `Enum::Variant` constructors and patterns; and every
source in the repository uses `::`.

## 2. Why this task exists now

The host never rejected the dot form.  Its parser reads
`Option.Some` as a `FieldExpr` on a type name, and the type checker,
HIR builder, and semantic-token classifier each carry an explicit
"`FieldExpr` or `QualifiedName`" branch for variants
(`type_checker.cpp` check_field, check_call, check_match;
`hir_builder.cpp` pattern and constructor lowering;
`semantic_tokens.cpp` `visit_pattern` and `FieldObjects`).  With both
forms accepted, the dot form spread: the bootstrap sources spell their
own enums that way at roughly 1,200 sites (`TK.X`, `Node.X`,
`HirNode.X`, `MirNode.X`, `Option.Some`), the examples at 191, the
stdlib at 3, and two contracts quote it in passing
(`CONTRACT_LANGUAGE_TOOLING.md` "use.variant" examples,
`CONTRACT_NUMERIC_SEMANTICS.md` `checked_*` results) while the ADR —
the law for `enum class` — uses `::` throughout.

The bootstrap compiler only half-supports the dot form: its type
checker's `check_field_expr` has no enum arm, so `Option.Some(v)` is
typed as nothing, silently.  The `::` form reaches
`check_qualname_expr` and `check_call_expr`'s enum arm and is typed as
the enum.  Migrating the corpus therefore also closes a bootstrap gap.

## 3. Contract changes (land first)

- `CONTRACT_SYNTAX_SURFACE.md`, under `enum` / `enum class`: a law that
  a variant is reached through its enum with `::`
  (`Enum::Variant`, `Enum::Variant(args)`, `Enum::Variant(field = v)`),
  in expressions and in patterns; `.` after a type name is a field
  access and is diagnosed.
- `CONTRACT_LANGUAGE_TOOLING.md` "use.variant": the examples read
  `Color::Red`, `Option::Some`.
- `CONTRACT_NUMERIC_SEMANTICS.md` `checked_*`: `Option::None`,
  `Option::Some(result)`.

## 4. Host compiler

- Type checker: `check_field` on an object that resolves to an enum
  type emits `variant access uses '::': Enum::Variant` at the field
  span and yields no type; the pattern extraction in `check_match` and
  the constructor path in `check_call` keep only their `QualifiedName`
  branches.  The "has a payload; use constructor syntax" message spells
  its example with `::`.
- HIR builder: the `FieldExpr` branches of pattern and constructor
  lowering go; `QualifiedName` remains.
- Semantic tokens: `visit_pattern`'s `FieldExpr` case and the
  `FieldObjects` variant classification go; `use.variant` is painted
  from qualified paths only.
- Tests: `typecheck_test` gains the diagnostic (expression, constructor
  callee, match pattern); every host test source that spelled a variant
  with `.` moves to `::` (`llvm_backend_test`, `semantic_tokens_test`);
  the AST goldens under `testdata/ast/` regenerate (a `FieldAccess` on
  a type name becomes a `QualifiedName`).

## 5. Bootstrap compiler

The `::` path exists: `check_qualname_expr` types a two-segment name
whose head is a type as that type, `check_call_expr`'s `TEnum` arm
types a construction, `check_match_stmt` checks a pattern as an
expression, `lower_call_expr` lowers a `QualNameE` callee to
`HirQualName`, and MIR defers enums as before.  This task adds:

- variant validation for a two-segment `Enum::Variant` on a local enum
  (the three-segment `mod::Enum::Variant` path already checks
  `variant_set`), with the diagnostic the three-segment path emits;
- tests in the type checker and HIR suites for a `::` constructor and a
  `::` pattern.

The `FieldE` path stays what it is — field access on a value — and
types nothing for an enum object, as today; after the corpus moves, no
bootstrap source exercises it.

## 6. Corpus migration

Mechanical, in one commit, over every tracked `.dao` file and the test
strings inside the C++ tests: `\b([A-Z][A-Za-z0-9_]*)\.([A-Z][A-Za-z0-9_]*)\b`
→ `\1::\2`.  The match is a capitalized identifier, a dot, and a
capitalized identifier: a variant of an enum whose name is a type name.
Fields and methods are lowercase by convention throughout the
repository, so no field access matches; the rewrite is then checked by
the host building every program (`task test`, `task bootstrap-test`)
and by the rejection diagnostic, which would name any site the regex
missed.  Docs that show source (`TASK_18_ENUM_PAYLOADS.md`, the plan,
the two contracts) are edited by hand; `docs/bootstrap_closure.md` is
regenerated.

## 7. Delivery

Three PRs on stack #292, above Task 37:

1. This spec, the three contract edits, the plan entry.
2. Bootstrap: two-segment variant validation and the `::` tests (green
   with the corpus still on `.`; both spellings type-check in the
   bootstrap until the next PR).
3. Host rejection, host tests, the corpus migration, goldens, docs,
   the audit rerun.

PR 3 after PR 2 keeps every PR green: the bootstrap's own sources are
inputs to the host, so the host's rejection and the corpus migration
land together, and the bootstrap must already type the `::` form its
migrated sources use.

## 8. Tests

- Host: `typecheck_test` — `Color.Red` in an expression, `Token.Int(1)`
  as a constructor, `Option.Some(v):` as a pattern each diagnose
  `variant access uses '::'`; the `::` spellings still type.
- Host: existing suites unchanged in count; goldens regenerated.
- Bootstrap: type checker — `Color::Red` types as `Color`;
  `Color::Blue` on an enum without `Blue` diagnoses; a `::` constructor
  with the wrong arity diagnoses as today.  HIR — a `::` pattern lowers
  the same nodes as before.
- Corpus: `task test`, `task bootstrap-test`, the closure audit rerun —
  with the corpus on `::` the resolve column's `unknown name` histogram
  is expected to move, since the dot form's enum head was a `FieldE`
  object the bootstrap never typed.

## 9. Non-goals

- Named-field construction (`Enum::Variant(field = v)`) beyond what the
  host parses today.
- Nested or or-patterns.
- Any change to `.` as field access on values.

## 10. One-sentence summary

A variant is reached through its enum with `::`; the host rejects the
dot spelling, the bootstrap types the qualified one, and every source
in the repository says the same thing.
