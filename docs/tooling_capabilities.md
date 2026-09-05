# Tooling Capabilities — Dao

Generated from `compiler/analysis/tooling_surface.h` by `tooling_surface_dump`;
do not edit by hand (`task gen-tooling-surface`).  `tooling_surface_test` fails
while this file and the table disagree.

One row per capability the compiler exposes to tooling and the surfaces that
serve it.  "—" means no surface of that kind serves it.  The LSP column names
the method each capability maps to; `tools/lsp` is not implemented, so none is
served over LSP yet.

| Capability | Compiler entry point | Playground route | `daoc` command | LSP method |
|---|---|---|---|---|
| Diagnostics | `Diagnostic` | `analyze` | `check` | `textDocument/publishDiagnostics` |
| Semantic tokens | `classify_tokens` | `analyze` | `tokens` | `textDocument/semanticTokens/full` |
| Hover | `query_hover` | `hover` | — | `textDocument/hover` |
| Go to definition | `query_definition` | `gotoDef` | — | `textDocument/definition` |
| References | `query_references` | `references` | — | `textDocument/references` |
| Document symbols | `query_document_symbols` | `documentSymbols` | — | `textDocument/documentSymbol` |
| Completion | `query_completions` | `completions` | — | `textDocument/completion` |
| AST dump | `print_ast` | `analyze` | `ast` | — |
| HIR dump | `print_hir` | `analyze` | `hir` | — |
| MIR dump | `print_mir` | `analyze` | `mir` | — |
| LLVM IR dump | `LlvmBackend::print_ir` | `analyze` | `llvm-ir` | — |
| Native build and run | `LlvmBackend::emit_object` | `run` | `build` | — |
