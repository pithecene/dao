# tools/lsp

Language Server Protocol surface for Dao.

Status: **not implemented**.  The analysis APIs it will wrap already
exist in `compiler/analysis/` and are served over HTTP by the
playground (`tools/playground/compiler_service/`).

Initial capability slice: the rows of `docs/tooling_capabilities.md`
that name an LSP method.  The payload shapes come from
`compiler/analysis/tooling_surface.h`; the server's transport will be
its own table beside it, as the playground's is
(`tools/playground/compiler_service/service_surface.h`).

Deferred until symbol identity and edit safety are mature:
- rename
- code actions
- semantic refactors
