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
(`module bootstrap::resolver`), type checking lives in
`bootstrap/typecheck/impl.dao` (`module bootstrap::typecheck`), and HIR
lowering lives in `bootstrap/hir/impl.dao` (`module bootstrap::hir`).

## Assembly

The modules `bootstrap::base`, `bootstrap::resolver`,
`bootstrap::typecheck`, and `bootstrap::hir` compile directly from authored sources without `bootstrap/assemble.sh`. Subsystems not yet extracted
into standalone modules still compose upstream fragments via
`bootstrap/assemble.sh`:

```sh
bash bootstrap/assemble.sh
```

This produces `<subsystem>.gen.dao` for `mir`, `llvm`,
`driver_tests`, and `driver`. The `ascii`, `lexer`, `parser`, `graph`, `base`,
`resolver`, `typecheck`, and `hir` modules compile directly from authored sources
without assembly:

```sh
daoc build --source bootstrap/resolver/tests.dao --source bootstrap/resolver/impl.dao --source bootstrap/shared/base.dao --source bootstrap/graph/impl.dao --source bootstrap/parser/impl.dao --source bootstrap/lexer/impl.dao --source bootstrap/ascii.dao
```

Git ignores `*.gen.dao` files because they are build artifacts.

## Editing Rules

1. **Edit authored `.dao` sources** (`base.dao`, `impl.dao`, `tests.dao`, `main.dao`), never `.gen.dao` files.
2. **Run `bash bootstrap/assemble.sh`** after editing any assembled input (`mir`, `llvm`, `driver`).
3. **Verify subsystems** via `task bootstrap-test` or `task bootstrap-crossing`.

## Rationale

The shared file `base.dao` eliminated triplicated frontend code before
multi-file compilation landed. Real module boundaries (`bootstrap::ascii`,
`bootstrap::lexer`, `bootstrap::parser`, `bootstrap::graph`, `bootstrap::base`,
`bootstrap::resolver`, `bootstrap::typecheck`, and `bootstrap::hir`) are retiring `assemble.sh` one phase boundary at a
time.
