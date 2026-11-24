# Bootstrap Shared Substrate

Shared module-graph, type-universe, and cross-pass program definitions for
subsystems that still assemble through `bootstrap/assemble.sh`.

## Contents

The file `base.dao` (`module bootstrap::base`) imports `bootstrap::lexer`
and `bootstrap::parser` and provides shared definitions for downstream
subsystems:

- Module graph construction, cycle detection, and topological ordering (`SourceInput`, `ProgramGraph`, `build_program`)
- Cross-pass type universe (`TypeKind`, `DaoType`)
- Cross-pass program lookup and index tables (`ProgramIndex`, `ProgramLookup`, `TypeCheckData`, `HirData`)

AST definitions (`Node`, `ParseOutput`, `read_list`) and recursive-descent
parsing (`parse`) live in `bootstrap/parser/impl.dao`
(`module bootstrap::parser`), which imports `bootstrap::lexer`
(`bootstrap/lexer/impl.dao`), which in turn imports `bootstrap::ascii`
(`bootstrap/ascii.dao`).

## Assembly

Subsystems not yet extracted into standalone modules still compose with
`base.dao` via `bootstrap/assemble.sh`:

```sh
bash bootstrap/assemble.sh
```

This produces `<subsystem>.gen.dao` for `graph`, `resolver`, `typecheck`,
`hir`, `mir`, `llvm`, `driver_tests`, and `driver`. Both `lexer` and
`parser` compile directly from authored sources without assembly.

Every assembled program imports `bootstrap::parser` and `bootstrap::lexer`,
which imports `bootstrap::ascii`, so every compile passes all three real
module files:

```sh
daoc build --source bootstrap/graph/graph.gen.dao --source bootstrap/parser/impl.dao --source bootstrap/lexer/impl.dao --source bootstrap/ascii.dao
```

Git ignores `*.gen.dao` files because they are build artifacts.

## Editing Rules

1. **Edit authored `.dao` sources** (`base.dao`, `impl.dao`, `tests.dao`, `main.dao`), never `.gen.dao` files.
2. **Run `bash bootstrap/assemble.sh`** after editing any assembled input.
3. **Verify subsystems** via `task bootstrap-test` or `task bootstrap-crossing`.

## Rationale

The shared file `base.dao` eliminated triplicated frontend code before
multi-file compilation landed. Real module boundaries (`bootstrap::ascii`,
`bootstrap::lexer`, and `bootstrap::parser`) are retiring `assemble.sh` one
phase boundary at a time.
