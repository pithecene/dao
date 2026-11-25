# Bootstrap Shared Substrate

Shared type-universe and cross-pass program lookup tables for subsystems
that still assemble through `bootstrap/assemble.sh`.

## Contents

The file `base.dao` (`module bootstrap::base`) imports `bootstrap::lexer`,
`bootstrap::parser`, and `bootstrap::graph` and provides shared definitions
for downstream subsystems:

- Cross-pass type universe (`TypeKind`, `DaoType`)
- Cross-pass program lookup and index tables (`ProgramIndex`, `ProgramLookup`, `TypeCheckData`, `HirData`)

Module-graph construction, topological ordering, and cycle detection live in
`bootstrap/graph/impl.dao` (`module bootstrap::graph`). AST definitions and
recursive-descent parsing live in `bootstrap/parser/impl.dao`
(`module bootstrap::parser`), which imports `bootstrap::lexer`
(`bootstrap/lexer/impl.dao`), which imports `bootstrap::ascii`
(`bootstrap/ascii.dao`).

## Assembly

Subsystems not yet extracted into standalone modules still compose with
`base.dao` via `bootstrap/assemble.sh`:

```sh
bash bootstrap/assemble.sh
```

This produces `<subsystem>.gen.dao` for `resolver`, `typecheck`, `hir`, `mir`,
`llvm`, `driver_tests`, and `driver`. The `lexer`, `parser`, and `graph`
subsystems compile directly from authored sources without assembly.

Every assembled program imports `bootstrap::graph`, `bootstrap::parser`, and
`bootstrap::lexer`, so every compile passes all four real module files:

```sh
daoc build --source bootstrap/resolver/resolver.gen.dao --source bootstrap/graph/impl.dao --source bootstrap/parser/impl.dao --source bootstrap/lexer/impl.dao --source bootstrap/ascii.dao
```

Git ignores `*.gen.dao` files because they are build artifacts.

## Editing Rules

1. **Edit authored `.dao` sources** (`base.dao`, `impl.dao`, `tests.dao`, `main.dao`), never `.gen.dao` files.
2. **Run `bash bootstrap/assemble.sh`** after editing any assembled input.
3. **Verify subsystems** via `task bootstrap-test` or `task bootstrap-crossing`.

## Rationale

The shared file `base.dao` eliminated triplicated frontend code before
multi-file compilation landed. Real module boundaries (`bootstrap::ascii`,
`bootstrap::lexer`, `bootstrap::parser`, and `bootstrap::graph`) are retiring
`assemble.sh` one phase boundary at a time.
