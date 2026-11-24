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
wind domain `0..1000`. Each figure is the median of five processes, with warm
medians over `200` live or `40` backfill runs and interleaved plans.

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

### Measurement Limits

- Apple Silicon uses unified memory, whereas discrete GPUs cross PCIe and penalize redundant transfers more heavily.
- Waiting only when a host step demands a pending readback avoids spurious synchronization.
- Cold timings include pipeline creation and OS shader caching, so only warm medians compare steady-state plan cost.

## Comparison with Handwritten Glue

The handwritten Metal function is 40 lines for one fixed plan, and every new
placement policy needs another copy. Plan data takes roughly 10 lines per plan
on one shared executor with equal backfill throughput, while catching lost
cutoffs, model drift, dropped or duplicated callbacks, and premature buffer
releases that handwritten glue cannot diagnose.

## Architectural Boundary

**Ordinary Dao expresses the plan.** Functions, classes, and `enum class` carry
the typed DAG, equivalence check, and complete cache key without new syntax.

**Compiler codegen is the remaining gap.** Function `score_v1` in Dao and `score_v1`
in Metal are still separate bodies tied together by tests and explicit `(kernel, model)`
records. Generating both CPU code and a device kernel from one verified Dao
function definition is the next bounded experiment once this semantic model lands.

## Stopgap Runner

The Swift runner and its Metal kernels are stopgaps recorded in
`docs/non_dao_inventory.md` until Dao lowers a kernel and drives a device
natively.
