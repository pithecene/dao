# Bootstrap Shared Substrate

Shared parser, AST, module-graph, and cross-pass program definitions for
subsystems that still assemble through `bootstrap/assemble.sh`.

## Contents

The file `base.dao` (`module bootstrap::base`) imports `bootstrap::lexer`
and provides shared definitions for downstream subsystems:

- AST (`Node` enum)
- Parser types (`PS`, `PR`, `FlushResult`, `flush_list`, `flush_pairs`)
- Recursive-descent parser (`parse()`)
- Module graph construction, topological ordering, and cross-pass program structures

Token definitions (`TK`, `Token`, `LexResult`, `tk_name`) and lexical
analysis (`lex`) live in `bootstrap/lexer/impl.dao` (`module bootstrap::lexer`),
which imports `bootstrap::ascii` (`bootstrap/ascii.dao`).

## Assembly

Subsystems not yet extracted into standalone modules still compose with
`base.dao` via `bootstrap/assemble.sh`:

```sh
bash bootstrap/assemble.sh
```

This produces `<subsystem>.gen.dao` for `parser`, `graph`, `resolver`,
`typecheck`, `hir`, `mir`, `llvm`, `driver_tests`, and `driver`. The
`lexer` subsystem compiles directly from authored `bootstrap/lexer/tests.dao`,
`bootstrap/lexer/impl.dao`, and `bootstrap/ascii.dao` without assembly.

Every assembled program imports `bootstrap::lexer`, which in turn imports
`bootstrap::ascii`, so every compile passes both real module files:

```sh
daoc build --source bootstrap/parser/parser.gen.dao --source bootstrap/lexer/impl.dao --source bootstrap/ascii.dao
```

Git ignores `*.gen.dao` files because they are build artifacts.

## Editing Rules

1. **Edit authored `.dao` sources** (`base.dao`, `impl.dao`, `tests.dao`, `main.dao`), never `.gen.dao` files.
2. **Run `bash bootstrap/assemble.sh`** after editing any assembled input.
3. **Verify subsystems** via `task bootstrap-test` or `task bootstrap-crossing`.

## Rationale

The shared file `base.dao` eliminated triplicated frontend code before
multi-file compilation landed. Real module boundaries (`bootstrap::ascii`,
then `bootstrap::lexer`) are retiring `assemble.sh` one phase boundary at a
time.
