# tools/playground

Dao product website and multi-file web playground.

This surface consumes compiler-owned semantic tokens, diagnostics, hover, go-to-definition, references, completions, document symbols, IR dumps, and native execution through `service_surface.h` and `tooling_surface.h` (`CONTRACT_LANGUAGE_TOOLING.md`).

## Quick start

```sh
task playground
# open http://localhost:8090
```

Options:
- `--port N` — listen port (default `8090`)
- `--root DIR` — repository root (default: compile-time `DAO_SOURCE_DIR`)

For frontend HMR against a live backend:

```sh
task playground-dev
```

To run the browser suite, which drives the built playground in headless Chromium:

```sh
task playground-browser-test
```

It keeps the browser under the build directory and starts the service on a port the system picks for the run.

## Architecture

- `compiler_service/` — C++ HTTP service (`cpp-httplib`) exposing program-shaped analysis, execution, example discovery, stdlib prelude inspection, and toolchain metadata
- `frontend/` — TypeScript + Vite bundle with Pithecene design tokens (`src/tokens.css`, dark and light themes), CodeMirror 6 editor, integrated site views (`#overview`, `#getting-started`, `#reference`, `#examples`, `#downloads`, `#playground`), resizable multi-file workspace (`src/document.ts`, `src/workspace_ui.ts`), `localStorage` draft persistence, and compiler-version-stamped snapshots

## HTTP Routes

Declared in `compiler_service/service_surface.h` and generated into `frontend/src/generated/tooling_surface.ts`:

- `POST /api/analyze` — tokens, semantic tokens, AST/HIR/MIR/LLVM IR dumps, and diagnostics for a multi-file `AnalyzeRequest`
- `POST /api/run` — compile, link against `libdao_runtime.a`, and execute a multi-file `ProgramRequest`
- `POST /api/format` — canonical trivia-preserving formatting for the active writable file
- `POST /api/hover`, `POST /api/goto-def`, `POST /api/references`, `POST /api/completions`, `POST /api/document-symbols` — cross-file language queries
- `GET /api/examples` and `GET /api/examples/:name` — single-file and multi-file example programs
- `POST /api/prelude` — read-only stdlib prelude file source, semantic tokens, and symbol outline
- `GET /api/version` — compiler commit, LLVM version, target triple, and prelude file list
