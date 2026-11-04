# Bootstrap Shared Substrate

Single source of truth for the bootstrap frontend pipeline.

## What This Contains

The file `base.dao` provides the canonical definitions consumed by all bootstrap
subsystems:

- Token model (`TK` enum, `Token`, `LexResult`)
- Character / keyword classification
- Token kind names (`tk_name`)
- Lexer (`lex()`)
- AST (`Node` enum)
- Parser types (`PS`, `PR`, `FlushResult`, `flush_list`, `flush_pairs`)
- Full recursive-descent parser (`parse()`)

## How Assembly Works

Each bootstrap subsystem has its own `.dao` source containing
subsystem-specific code (tests, resolver logic, etc.).  The
`assemble.sh` script concatenates `base.dao` with each subsystem
source to produce `*.gen.dao` files that the compiler can build.

```sh
# From repository root:
bash bootstrap/assemble.sh
```

This produces one `<subsystem>.gen.dao` per subsystem (lexer, parser,
graph, resolver, typecheck, hir, mir, llvm).  Each is `base.dao` plus
the `impl.dao` (library only) of every upstream subsystem it depends
on, in dependency order, then the subsystem's own `impl.dao`, then its
own `tests.dao` runner last.  Production code lives in `impl.dao`, the test runner in
`tests.dao`.  The two are separate files and `assemble.sh` composes them
explicitly (no marker slicing).  `assemble.sh` is the exact statement
of that composition.

Git ignores the `*.gen.dao` files, because they are build artifacts.

A generated program imports `bootstrap::ascii`, a real module outside assembly, so every compile passes `bootstrap/ascii.dao` beside it:
`daoc build --source bootstrap/lexer/lexer.gen.dao --source bootstrap/ascii.dao`.

## Editing Rules

1. **Edit `base.dao` or subsystem `.dao` sources.**  Never edit the
   `.gen.dao` files directly.
2. **Run `bash bootstrap/assemble.sh`** after any edit.
3. **Verify every subsystem** still compiles and passes tests
   (`task bootstrap-test`).

## Why This Exists

Dao does not yet support multi-file compilation.  Before consolidation,
every bootstrap subsystem duplicated the entire token model, lexer, and
parser (~2000 lines each).  Three subsystems meant ~4000 lines of
triplicated code.

This shared base eliminates the duplication at the source level.
Assembly produces the single-file outputs the compiler requires.

When Dao gains a module system, proper imports can replace this concatenation scheme.
