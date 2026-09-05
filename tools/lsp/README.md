# tools/lsp

Language Server Protocol surface for Dao.

Status: **not implemented**.  The analysis APIs it will wrap already
exist in `compiler/analysis/` and are served over HTTP by the
playground (`tools/playground/compiler_service/`).

Initial capability slice: the rows of `docs/tooling_capabilities.md`
that name an LSP method (generated from
`compiler/analysis/tooling_surface.h`, which is also where the server's
routes and payload shapes will come from).

Deferred until symbol identity and edit safety are mature:
- rename
- code actions
- semantic refactors
