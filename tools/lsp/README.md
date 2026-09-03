# tools/lsp

Language Server Protocol surface for Dao.

Status: **not implemented**.  The analysis APIs it will wrap already
exist in `compiler/analysis/` and are served over HTTP by the
playground (`tools/playground/compiler_service/`).

Initial capability slice:
- diagnostics
- semantic tokens
- hover
- completion
- go-to-definition
- find references
- document symbols

Deferred until symbol identity and edit safety are mature:
- rename
- code actions
- semantic refactors
