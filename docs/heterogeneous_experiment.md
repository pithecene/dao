# Heterogeneous Computation Experiment

This note is explanatory. It records the test that Dao can let one computation
serve a live service, a historical replay, and an accelerator while rejecting
rewrites that alter point-in-time meaning, model identity, host observations,
or buffer lifetimes. The language stays uncommitted by anything here.

Run `./examples/forecast_plans` after building the example, and run
`task forecast-device` on macOS for the Metal measurements.

## Forecast Rule and Semantic Representation

The file `examples/forecast_plans.dao` computes one point-in-time rule: a
prediction reads the latest forecast revision received by its decision time.
Revision `r1` arrived at `08:55` with wind `8`, and correction `r2` arrived at
`09:07` with wind `12`. A request decided at `09:00` returns `76` minutes, and
one decided at `09:10` returns `89`. Replaying the `09:00` request after `r2`
arrives still returns `76`, while a request decided at `08:00` or for an absent
cell returns an explicit `Missing` result.

Each plan is a list of `Step` values lowered by `analyze_plan` into a typed
`ValueExpr` DAG (`ReadSource`, `UnaryOp`, `BinaryOp`) rather than plan-wide
mutable strings:

- Every `ReadVisible` binds its output to a `ReadSource` carrying `archive`, `cell`, `valid_at`, `decided_at`, and `Visibility`, and records its point-in-time read precondition.
- Every `Dispatch` and `HostAdjust` binds its output to exact producer node IDs and its own `(operation, model)` pair without string concatenation or substring deduplication.
- Every `Upload` and `Readback` preserves producer `ValueExpr` identity across host and device bindings while rejecting undefined inputs, duplicate definitions, and use-after-`Release`.
- Every `Observe` records an ordered list of `(name, value_id)` pairs so both dropped and duplicated host callbacks fail equivalence review.
- Function `record_line` preserves `archive`, `cell`, `valid_at`, `visibility`, `decided_at`, and `model` in the tab-delimited stream verified by `metal_runner.swift`.

## Accepted Placement Rewrites

| Metric | Straightforward | Proposed |
|---|---|---|
| Uploads | 2 | 1 |
| Readbacks | 2 | 1 |
| Combine runs on | CPU | GPU |
| Payload at N = 2 | 16 B each way | 8 B each way |
| Reply DAG | `adjust@v1(score_v1@v1(read(...)), high_wind@v1(read(...)))` | identical |

The reviewer accepts `straightforward -> proposed` and `audited -> audited_shared`
because read preconditions, ordered observations, and the reply DAG match
structurally while every buffer stays live through its final consumer.

## Rejected Rewrites and Reuse Counterexamples

| Case | Rejection reason |
|---|---|
| `latest_now` | `LatestNow` replaces `AsOfDecision` in both read preconditions and the reply leaf, moving `09:00` from `76` to `89` |
| `two_read_masked_cutoff` | Reply leaf still references the `LatestNow` read even when a trailing unused `AsOfDecision` read follows |
| `substring_model_drift` | Changing one stage from `v12` to `v1` fails structural `model` comparison on `UnaryOp` |
| `miswired` | Feeding `score` into both inputs of `adjust` alters the reply DAG |
| `audited -> proposed` | Dropping `audit_score` alters the ordered observation vector |
| `duplicated_audit` | Emitting `audit_score` twice alters observation multiplicity |
| `premature_release` | Releasing `wind_shared` before `high_wind` dispatches fails liveness validation |
| `ResultKey` reuse | Keys bind `(operation, model, archive, cell, valid_at, visibility, decided_at)`, rejecting cross-cell (`7` vs `8`), cross-valid-time, cross-visibility, cross-model, cross-archive, and cross-cutoff reuse |

## Measurements

Measured on an Apple M5 running macOS, through Metal, over the validated integer
wind domain `0..1000`. Each figure reports the median across five processes,
with warm medians over `200` live or `40` backfill runs and interleaved plans.

### Handwritten Unchecked Shader Baseline

Earlier iterations executed handwritten Metal compute shaders without checked
`i32` status buffers or `MTLCommandBuffer` error guards:

| Mode | Plan | Cold ms | Warm ms | Bytes moved | Waits |
|---|---|---|---|---|---|
| Live, N = 1 | Straightforward | 0.560 | 0.216 | 16 | 1 |
| Live, N = 1 | Proposed | 0.637 | 0.203 | 8 | 1 |
| Live, N = 1 | Audited and shared | 1.358 | 0.417 | 12 | 2 |
| Live, N = 1 | Handwritten | 0.708 | 0.212 | 8 | 1 |
| Backfill, N = 1,000,000 | Straightforward | 4.785 | 3.447 | 16,000,000 | 1 |
| Backfill, N = 1,000,000 | Proposed | 1.674 | 1.437 | 8,000,000 | 1 |
| Backfill, N = 1,000,000 | Audited and shared | 2.061 | 1.885 | 12,000,000 | 2 |
| Backfill, N = 1,000,000 | Handwritten | 1.600 | 1.641 | 8,000,000 | 1 |

### Compiler-Generated Checked Kernels

Current runs lower `score_v1`, `high_wind`, and `adjust` through `dao_msl_emit`
with 64-bit range checks, `atomic_uint` trap propagation, SHA-256 bundle
verification, and `MTLCommandBuffer` completion checks:

| Mode | Plan | Cold ms | Warm ms | Bytes moved | Waits |
|---|---|---|---|---|---|
| Live, N = 1 | Straightforward | 1.383 | 0.331 | 16 | 1 |
| Live, N = 1 | Proposed | 0.957 | 0.309 | 8 | 1 |
| Live, N = 1 | Audited and shared | 1.387 | 0.626 | 12 | 2 |
| Live, N = 1 | Handwritten | 0.952 | 0.311 | 8 | 1 |
| Backfill, N = 1,000,000 | Straightforward | 12.855 | 10.557 | 16,000,000 | 1 |
| Backfill, N = 1,000,000 | Proposed | 4.620 | 2.549 | 8,000,000 | 1 |
| Backfill, N = 1,000,000 | Audited and shared | 4.510 | 3.361 | 12,000,000 | 2 |
| Backfill, N = 1,000,000 | Handwritten | 3.677 | 2.320 | 8,000,000 | 1 |

### Measurement Limits

- Apple Silicon uses unified memory, whereas discrete GPUs cross PCIe and penalize redundant transfers more heavily.
- Waiting only when a host step demands a pending readback avoids spurious synchronization.
- Cold timings include pipeline creation and OS shader caching, so only warm medians compare steady-state plan cost.
- Checked `Straightforward` runs perform two GPU dispatches, two readbacks, and checked host `i32` arithmetic across a million elements, whereas `Proposed` keeps intermediates on the GPU and runs `4.14x` faster (`2.549` ms vs `10.557` ms).

## Comparison with Handwritten Glue

The handwritten host dispatch function takes 46 lines for one fixed plan, and
every new placement policy needs another copy. Plan data takes roughly 10 lines
per plan on one shared executor with comparable backfill throughput, while
catching lost cutoffs, model drift, dropped or duplicated callbacks, and
premature buffer releases.

## Single-Source CPU and Metal Codegen

Both CPU native code (`daoc build`) and Metal compute kernels (`dao_msl_emit`)
now lower from the same authored functions (`score_v1`, `high_wind`, and
`adjust`) in `examples/forecast_plans.dao` without a second kernel body or
name-based template lookup:

- Backend `MslBackend` (`compiler/backend/msl/`) consumes `MirModule` over a verified pure scalar elementwise slice (`(i32...) -> i32`, `i32` and `bool` locals, integer and boolean constants, checked unary and binary operators, and acyclic multi-block `MirBr` / `MirCondBr` / `MirReturn` control flow).
- Signed `i32` arithmetic in generated Metal lowers through 64-bit `long` range checks and an `atomic_uint` status buffer, preserving `CONTRACT_NUMERIC_SEMANTICS.md` overflow traps.
- Unsupported parameter or return types, calls, raw pointers, `mode` or `resource` blocks, and cyclic control flow fail closed with source coordinates.
- Manifest output (`forecast_kernels.tsv`) emits a `bundle_sha256` digest alongside module identity, operation name, `file:line:col` origin, target (`metal-msl-2.4`), signature, and effect profile (`pure_scalar_elementwise_checked_i32`).
- Runner `metal_runner.swift` verifies the SHA-256 digest before `MTLDevice.makeLibrary`, checks `MTLCommandBuffer` `.completed` status and `nil` error before reading result buffers, and compares GPU outputs and overflow traps directly against the compiled `forecast_plans` CPU binary.

### Layer Attribution

This experiment required zero new Dao syntax or execution-mode surface:

- **Ordinary Dao library code** already expresses typed plan steps, point-in-time read provenance, observation vectors, liveness checks, and complete cache keys.
- **Existing typed MIR** already preserves concrete `i32` and `bool` types, acyclic control flow, and `SourceMap` origins needed to emit verified device kernels.
- **Target-specific compiler backend (`compiler/backend/msl/`)** owns kernel eligibility validation, checked `i32` status-buffer lowering, and provenance manifests.

## Stopgap Runner

The Swift host runner in `examples/forecast_device/metal_runner.swift` remains a
stopgap until Dao drives Metal command queues directly, while `dao_msl_emit`
retires all handwritten Metal shader code.
