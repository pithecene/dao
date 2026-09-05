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

**One table.** `compiler/analysis/tooling_surface.h` lists every token
kind with its visual group, the lexical categories and diagnostic
severities, every JSON payload shape, and every service route with its
request and response shapes.  It is C++ so the compiler is the source of
truth and nothing parses a manifest at runtime.

**Generated frontend types.** `tooling_surface_dump` renders the table
as `tools/playground/frontend/src/generated/tooling_surface.ts`
(`task gen-tooling-surface`).  The frontend's typed client (`src/api.ts`)
takes route names, request bodies, and response types from it, and the
highlighter maps `TokenKind` to `dao-<group>` through the generated
`TOKEN_GROUP` record — an unknown kind or route is a `tsc` error, not a
silently unstyled token.

**Fences (all in `ctest`).**
- `tooling_surface_test` — the table equals the taxonomy in
  `CONTRACT_LANGUAGE_TOOLING.md`; every kind the emitter produces over
  `examples/` and `spec/syntax_probes/` is in the table; the checked-in
  TypeScript equals what the dump tool renders.
- `playground_service_test` — drives every route through `dispatch`
  (no HTTP) over every example: each route is bound and validates its
  request; each reply matches its declared shape; every lexical token
  of every example gets a semantic token; hover, definition, references,
  symbols, and completions answer; every example runs and prints its
  golden `testdata/examples/<name>.out` (`task update-example-goldens`),
  except those listed with a reason in `testdata/examples/known_failures.txt`.
- `npm run build` runs `tsc --noEmit` before bundling.

**The rule.** A change to a token kind, a payload field, a route, or a
diagnostic severity edits the table, regenerates the TypeScript, and
lands with the frontend change in the same PR.  A language feature lands
with an example that exercises it, which the service test then keeps
runnable and fully classified.

**Live loop.** `task playground-dev` runs `tools/playground/dev.sh`: the
compiler service restarts whenever its binary is rebuilt, Vite serves the
frontend with HMR, and the frontend retries requests while the service
restarts (a "compiler restarting…" badge shows meanwhile).
