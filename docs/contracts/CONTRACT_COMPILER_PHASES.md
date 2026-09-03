# CONTRACT_COMPILER_PHASES.md

## Purpose

Defines the mandatory compiler architecture and phase boundaries for Dao.
This contract is normative.

## Topology

Dao is split into these primary implementation strata:
- `compiler/frontend/`
- `compiler/ir/`
- `compiler/backend/`
- `compiler/driver/`
- `compiler/analysis/`
- `runtime/`

## Frontend Responsibilities

`compiler/frontend/` owns source-facing concerns only:
- source management and file loading
- indentation-aware lexing
- parsing
- concrete syntax tree / AST construction
- syntax validation
- source spans and diagnostics infrastructure
- desugaring from surface syntax into frontend-owned normalized forms

Required subroots:
- `compiler/frontend/lexer/`
- `compiler/frontend/parser/`
- `compiler/frontend/ast/`
- `compiler/frontend/resolve/` — name resolution and scope analysis
- `compiler/frontend/diagnostics/`
- `compiler/frontend/types/` — canonical semantic type universe
  (types, interning, comparison, printing)

- `compiler/frontend/typecheck/` — semantic type-checking pass

Surface-to-HIR lowering is implemented by the HIR builder under
`compiler/ir/hir/`; no separate `compiler/frontend/lower/` subroot is
required.

Dependency rules:
- `types/` must not depend on `typecheck/`
- `typecheck/` consumes syntax, resolution, and semantic types to
  validate programs

## IR Responsibilities

`compiler/ir/` owns compiler-internal program representations.

Required IR layers:
- `compiler/ir/hir/` — high-level IR after parsing/desugaring and before
  low-level control-flow expansion
- `compiler/ir/mir/` — mid-level IR suitable for explicit control flow,
  resource/mode lowering preparation, optimization, and backend handoff

HIR expectations:
- retains enough language structure to reason about declarations,
  expression bodies, modes, resources, and type-directed semantics
- is independent of LLVM details

MIR expectations:
- makes control flow explicit
- prepares resource and mode semantics for runtime/backend lowering
- normalizes pipeline/lambda sugar into analyzable operations
- is still target-agnostic

## Backend Responsibilities

`compiler/backend/` owns target-specific lowering only.

Initial required backend:
- `compiler/backend/llvm/`

LLVM backend expectations:
- lowers MIR into LLVM-facing structures only
- must not own parsing, syntax policy, or source-level semantics
- must not redefine diagnostics policy established by frontend/IR layers

Additional future backends may be added, but only as sibling directories
under `compiler/backend/`.

## Driver Responsibilities

`compiler/driver/` owns orchestration only:
- phase wiring
- compilation session configuration
- target selection
- artifact emission coordination
- top-level CLI/compiler entry plumbing

`compiler/driver/` must not become a god-module for frontend or backend
logic.

## Runtime Responsibilities

`runtime/` owns execution support required by lowered programs, including:
- scoped resource / memory support
- mode hooks and runtime integration
- GPU/runtime bindings

Required subroots:
- `runtime/memory/`
- `runtime/modes/`
- `runtime/gpu/`

## Analysis Responsibilities

`compiler/analysis/` owns compiler-produced semantic classification and
tooling-facing analysis outputs:
- semantic token streams
- hover payloads
- completion payloads
- definition/reference lookup
- document symbol trees
- diagnostics surfacing for CLI, playground, and LSP

`compiler/analysis/` consumes frontend and IR layers. It must not
reimplement parsing, name resolution, or type checking.

Dependency direction:
- `compiler/analysis/` depends on `compiler/frontend/` (AST, resolve,
  types, typecheck)
- `tools/{playground,lsp}` depend on `compiler/analysis/`
- `compiler/analysis/` must not depend on `tools/`

## Boundary Laws

1. Surface syntax decisions live in contracts/spec, not in backend code.
2. Frontend may normalize syntax but must not invent backend-only meaning.
3. IR layers must remain target-agnostic.
4. LLVM details must not leak upward into HIR or parser APIs.
5. Runtime APIs must support language semantics; they must not redefine
   them.
6. Resource and mode semantics must be representable before LLVM lowering.
7. Diagnostics ownership begins in the frontend and may be enriched later,
   but backend-only diagnostics must not become the primary user-facing
   error model.
