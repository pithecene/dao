# Playground Architecture — Dao

This document is explanatory.

## Role

The Dao playground is both:
1. a learning surface for users
2. a development and testing surface for the compiler team

It is intended to evolve toward a lightweight web-based IDE.

## Immediate Goals

- example-driven language development
- rapid UAT for syntax and diagnostics
- semantic syntax highlighting
- easy inspection of representative `examples/` programs

## Initial Capabilities

- example loader from `/examples`
- editable source panes
- compiler-backed diagnostics as soon as the frontend analysis exists
- compiler-backed semantic tokens as soon as symbol resolution exists

## Mid-Term Capabilities

- type hover
- symbol navigation
- definition peek
- document symbol outline
- AST / HIR panes

## Long-Term Capabilities

- multi-file workspace view
- MIR inspection
- incremental compilation status
- future debugger / runtime visualizations where warranted

## Frontend Toolchain

- **Bundler**: Vite (dev server with HMR, production build to `dist/`)
- **Language**: TypeScript (strict mode)
- **Editor**: CodeMirror 6 (npm, not CDN)
- **Dev workflow**: `task playground-dev` runs Vite dev server (port 5173)
  proxying `/api/*` to C++ backend (port 8090)
- **Prod workflow**: `task playground` builds frontend with Vite then starts
  C++ server serving `dist/`

## Architectural Rule

The playground must reuse compiler analysis rather than maintaining its own
parser, semantic model, or token taxonomy.

## Sync Discipline

The playground drifts when the same fact is restated in several places:
the token taxonomy in the contract, the kinds the emitter produces, the
CSS mapping in the frontend; the routes the service registers and the
routes the frontend calls.  One table and three fences keep them equal.

**Two tables, one owner each.** `compiler/analysis/tooling_surface.h`
is the compiler's: every token kind with its visual group, the lexical
categories and diagnostic severities, and the payload shapes of the
analysis results — semantic truth, which the phase and tooling
contracts assign to `compiler/analysis`.
`tools/playground/compiler_service/service_surface.h` is the
playground's: its routes with methods and paths, its request envelopes,
and its own response shapes — transport, which those contracts leave to
the playground.  Both are C++ so nothing parses a manifest at runtime.

**Generated frontend types.** `service_surface_dump` renders both
tables as `tools/playground/frontend/src/generated/tooling_surface.ts`
and the capability matrix as `docs/tooling_capabilities.md`
(`task gen-tooling-surface`).  The frontend's typed client (`src/api.ts`)
takes route names, request bodies, and response types from it, and the
highlighter maps `TokenKind` to `dao-<group>` through the generated
`TOKEN_GROUP` record — an unknown kind or route is a `tsc` error, not a
silently unstyled token.

**Fences (all in `ctest`).**
- `tooling_surface_test` — the analysis table equals the taxonomy in
  `CONTRACT_LANGUAGE_TOOLING.md`; every kind the emitter produces over
  `examples/` and `spec/syntax_probes/` is in the table.
- `playground_service_test` — drives every route through `dispatch`
  (no HTTP) over every example: each route is bound and validates its
  request; each reply matches its declared shape; the checked-in
  TypeScript equals what `service_surface_dump` renders; every lexical
  token of every example gets a semantic token; hover, definition,
  references, symbols, and completions answer — including member
  completion on a buffer that does not parse, the state a user is in
  when they type `.`; every example runs and prints its golden
  `testdata/examples/<name>.out` (`task update-example-goldens`), except
  those listed in `testdata/examples/known_failures.txt` with the
  diagnostic the compiler must report for the failure to count.
- `npm run build` runs `tsc --noEmit` before bundling.

**Program-shaped requests, file identity on replies.** A request
carries the files of the program and names the one it is about
(`files`, `document`; offsets are local to the document).  This UI
sends one file; a workspace sends them all, and the service is the
same.  Every position a reply carries names its file (`file`, the
display path) with file-local offset and line — diagnostics, symbols,
definitions, references — and replies about the document report its
path and module at the top level; token entries inherit the file of
the reply.  Definitions and references therefore reach into the
prelude today and into other user files as sent; the single-document
UI names positions it cannot open (a notice for a prelude definition,
a count for references elsewhere) instead of the service hiding them.
That is what keeps the multi-file workspace (Task 31 D5) a UI change
rather than an API change.

**Capability matrix.** `docs/tooling_capabilities.md` is generated from
the service surface's capability table — compiler entry points,
playground route, `daoc` command, LSP method per capability — and
`playground_service_test` checks that each entry point is declared in
the header the table names, each route is in the route table, each
command is in the driver, and LSP methods are well-formed and unique.
READMEs point at it rather than restating it.

**The rule.** A change to a token kind, a payload field, a route, or a
diagnostic severity edits the table, regenerates the TypeScript, and
lands with the frontend change in the same PR.  A language feature lands
with an example that exercises it, which the service test then keeps
runnable and fully classified.

**Live loop.** `task playground-dev` runs `tools/playground/dev.sh`: the
compiler service restarts whenever its binary is rebuilt, Vite serves the
frontend with HMR, and the frontend retries requests while the service
restarts (a "compiler restarting…" badge shows meanwhile).
