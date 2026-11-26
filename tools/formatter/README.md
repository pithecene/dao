# Dao Formatter

Canonical Dao source formatting is owned by the compiler analysis layer so
every developer surface formats buffers identically in memory without a
shadow parser (`CONTRACT_LANGUAGE_TOOLING.md`):

- **Engine**: `compiler/analysis/formatter.h` and `compiler/analysis/formatter.cpp`
  (`dao::format_source`, `dao::verify_format_invariants`), tested by
  `compiler/analysis/formatter_test.cpp` across unit cases and the `stdlib/`
  and `examples/` corpus.
- **CLI**: `daoc fmt [--write | --check] <file.dao>...` in `compiler/driver/main.cpp`.
- **Playground**: `POST /api/format` (`tools/playground/compiler_service/format.cpp`).
- **LSP**: maps to `textDocument/formatting` once `tools/lsp/` lands.

## Design Invariants

1. **Single compiler parser**: formatting runs over `SourceBuffer`, the
   lexer's `Token` stream, and the parsed `FileNode` AST.
2. **Trivia preservation**: `//` standalone and trailing line comments and
   intentional single blank lines are recovered from `SourceBuffer` byte
   spans and emitted in source order.
3. **Fail-safe**: inputs with lexical or syntax errors are never rewritten;
   post-format verification checks AST equivalence and comment preservation
   before returning formatted text.

## Path to a Self-Hosted Formatter in Dao

`tools/formatter/` remains reserved for a standalone Dao-authored formatter
tool once the self-hosted tier replaces host tooling. Replacing the C++
formatter with a Dao implementation requires:

- reusing `bootstrap::lexer` and `bootstrap::parser` (with byte-span trivia
  indexing over `SourceBuffer` text) as imported library modules rather than
  assembled single-file targets, and
- standard-library filesystem write/rename and exit-status primitives in
  `stdlib/io/` so `--write` and `--check` can update files atomically.
