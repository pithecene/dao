# Dao

Dao is a language project for high-performance structured computation.

Current priorities:
- language surface and grammar
- compiler architecture and diagnostics
- memory/execution model (`mode` / `resource`)
- numeric, graph, routing, and GPU-oriented workloads

The project is governance-first: contracts under `docs/contracts/` are
the authoritative source of intent. Implementation is well underway — a
C++23 host compiler targeting LLVM, a native runtime, an initial standard
library, a web playground, and a Dao-implemented bootstrap compiler that
lowers Tier A Dao to textual LLVM IR.

## Repository shape

- `bootstrap/` — self-hosting compiler subsystems written in Dao
- `compiler/` — C++23 host compiler: lexer → parser → resolve → typecheck → HIR → MIR → LLVM backend → driver, plus tooling-facing analysis
- `runtime/` — runtime and execution backends
- `stdlib/` — standard library surface
- `spec/` — grammar, syntax notes, semantic reference inputs
- `docs/` — contracts, architecture index, explanatory docs
- `examples/` — small Dao programs and syntax probes
- `testdata/` — parser/compiler fixtures
- `tools/` — playground, LSP, formatter, and diagnostics tooling
- `ai/` — Bonsai governance skills, baselines, outputs

## Governance docs

The authoritative files for AI-assisted work are:
1. `CLAUDE.md`
2. `AGENTS.md`
3. `docs/contracts/CONTRACT_*.md`
4. `docs/ARCH_INDEX.md`

`README.md` is informational only.

## Implementation roadmap

See `docs/ROADMAP.md` for the staged implementation plan and
`docs/compiler_bootstrap_and_architecture.md` for current architecture
notes around frontend/IR/backend split, C ABI interop, and self-hosting.


## Tooling

Dao treats the playground and IDE-facing tooling as first-class compiler
consumers. See `tools/`, `docs/PLAYGROUND_ARCHITECTURE.md`, and
`docs/IDE_AND_TOOLING.md`.


## Grammar

The current reference grammar and lexical notes live under `spec/grammar/`.
Use `docs/contracts/CONTRACT_SYNTAX_SURFACE.md` for the frozen language-facing syntax guarantees and `spec/syntax_probes/` for parser/readability probes.
