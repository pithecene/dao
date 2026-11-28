# Bootstrap Shared Substrate

Cross-pass semantic type-universe and program-lookup tables compiled directly
as `module bootstrap::base` (`bootstrap/shared/base.dao`).

## Contents

The file `base.dao` (`module bootstrap::base`) provides shared definitions
imported by `bootstrap::resolver` and downstream compiler passes:

- Cross-pass type universe (`TypeKind`, `DaoType`)
- Cross-pass program lookup and index tables (`ProgramIndex`, `ProgramLookup`, `TypeCheckData`, `HirData`)

Module-graph construction, topological ordering, and cycle detection live in
`bootstrap/graph/impl.dao` (`module bootstrap::graph`). AST definitions and
recursive-descent parsing live in `bootstrap/parser/impl.dao`
(`module bootstrap::parser`), which imports `bootstrap::lexer`
(`bootstrap/lexer/impl.dao`), which imports `bootstrap::ascii`
(`bootstrap/ascii.dao`). Name resolution lives in `bootstrap/resolver/impl.dao`
(`module bootstrap::resolver`), and type checking lives in
`bootstrap/typecheck/impl.dao` (`module bootstrap::typecheck`).  HIR
lowering lives in `bootstrap/hir/impl.dao` (`module bootstrap::hir`), and MIR
lowering lives in `bootstrap/mir/impl.dao` (`module bootstrap::mir`).  The
LLVM backend lives in `bootstrap/llvm/impl.dao` (`module bootstrap::llvm`),
and the program pipeline lives in `bootstrap/driver/impl.dao`
(`module bootstrap::driver`).

## Building

Every bootstrap module compiles directly from its authored source.  The
host builds a program from its entry source and each module that it
imports:

```sh
daoc build --source bootstrap/resolver/tests.dao --source bootstrap/resolver/impl.dao --source bootstrap/shared/base.dao --source bootstrap/graph/impl.dao --source bootstrap/parser/impl.dao --source bootstrap/lexer/impl.dao --source bootstrap/ascii.dao
```

## Editing Rules

1. **Edit authored `.dao` sources** (`base.dao`, `impl.dao`, `tests.dao`, `main.dao`).
2. **Verify subsystems** via `task bootstrap-test` or `task bootstrap-crossing`.

## Rationale

The shared file `base.dao` eliminated triplicated frontend code before
multi-file compilation landed.  Real module boundaries then replaced the
script that concatenated each program from fragments, one phase boundary
at a time.
