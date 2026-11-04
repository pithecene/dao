# Heterogeneous Computation Experiment

This note is explanatory.  It records the first test of the proposal that
Dao could let one computation serve a service, a data pipeline and an
accelerator, with a reviewer able to see any change of meaning.  The language
stays uncommitted by anything here.

Run it with `./examples/forecast_plans` after building the example, and with
`task forecast-device` for the GPU measurements.

## The Example

The file `examples/forecast_plans.dao` computes one rule: a prediction reads
the latest forecast revision received by its decision time.  Revision r1
arrived at 08:55 with wind 8, and its correction r2 arrived at 09:07 with
wind 12.  A request decided at 09:00 returns 76 minutes and one decided at 09:10
returns 89.  Replaying the 09:00 request after r2 arrives still returns 76, and
a request decided at 08:00 returns an explicit missing result.

A plan is a list of steps, in ordinary Dao.  Its cutoff, archive, model,
observations and result expression are all derived from those steps, and the
same steps print for a person and as `record` lines for the device runner.

## Two Plans

| | Straightforward | Proposed |
|---|---|---|
| Uploads | 2 | 1 |
| Readbacks | 2 | 1 |
| Combine runs on | CPU | GPU |
| Payload at N = 2 | 16 B each way | 8 B each way |
| Reply | `adjust(score_v1(wind), high_wind(wind))` | the same |

The review accepts this rewrite because every derived fact is unchanged.

## What the Review Rejects

| Rewrite | Finding |
|---|---|
| Join against the latest revision now | the cutoff is gone, and the 09:00 request moves from 76 to 89 |
| Feed the score to both inputs of `adjust` | the result expression changes |
| Drop the audit callback's score readback | the observation is gone |
| Reuse a stored result across a model, archive or cutoff | the review refuses each by name |

The audited program can still share its upload, and the review accepts that:
one upload and two readbacks.

## Measurements

Measured on an Apple M5 running macOS 26.7.1, through Metal, with Swift
6.3.3.  Each figure is the median of five processes.  Warm is the median of 200
live or 40 backfill runs in each process, with the plans interleaved.  Every
output matched the oracle or the CPU calculation, and every byte count matched
the plan's prediction.

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

The tradeoff differs by caller.  The proposed plan is 2.4 times faster for a
backfill and about 6 percent faster for one live request, where the copy is
small.  For a live request the audit's extra wait costs about twice the time,
far more than the saved transfers return.  One plan is therefore not best for
every caller, which supports letting each caller choose its own.

### Limits of These Numbers

- The M5 has unified memory, while the proposal assumes discrete memory.  The
  copies are real blits between shared and private buffers, but they cross no bus, so a discrete GPU should penalize transfers more heavily.
- The first version of the runner waited for the device before each upload.
  That added a wait the plan never asked for and doubled the straightforward
  plan's live time.  The runner dropped that wait before these measurements.
- Cold times include library and pipeline creation, the operating system may
  cache compiled pipelines across processes, and the runs have a fixed order.
  The cold column supports no comparison between plans.

## Comparison with Handwritten Glue

The handwritten function in the runner is 44 lines for one plan, and each
further plan needs its own.  In the plan-driven version, a new plan is about 10
lines of data, and one executor serves them all.  The runner adds no visible
cost: the handwritten function was no faster in the backfill and equal within
noise for a live request.

The larger difference is checking.  The handwritten function computes the same
outputs, and nothing in it could report a lost cutoff or a dropped observation.

## Where Each Finding Belongs

**No syntax yet.**  Everything here is ordinary functions, classes and enums.
This example gives no evidence for new syntax.

**A library can hold the plan.**  The step list, derived meaning, review and
result key work as Dao code today.  A later process adapter could read the same
`record` lines.

**The gap is in the compiler.**  Three weaknesses here are beyond a library:

1. The Dao `score_v1` and the Metal `score_v1` are separate implementations
   tied together only by a name and by tests.  The review checks a rewrite against the plan, and nothing proves the kernel computes what the reference does.
2. The plan's `cutoff` is a string, unconnected to the code in `visible_to`.  A
   plan can claim a cutoff the reference does not enforce.
3. The review compares result expressions as text, so it would reject a correct rewrite that reorders an equivalent expression.

The compiler already knows these identities and lowers pipes to ordinary calls
that lose them.  The next experiment should lower one reference function to a
kernel from a single definition, which addresses the first weakness directly.

## Recommendation

Continue, with that one test as the condition.  One definition that produces
both the CPU and GPU code and keeps its identity into the plan makes the checked
rewrite evidence for a compiler feature.  If that needs new semantics, the need
itself names the first real language question.  Deployment,
distributed retries and scheduling stay out of scope.

## The Runner as a Stopgap

The Swift runner and its Metal kernels are not Dao, and
`docs/non_dao_inventory.md` records them.  They exist because Dao cannot yet
lower a kernel or drive a device, which is the same capability the next
experiment tests.

## Differences from the Design Handoff

- The sketch's `Result<Minutes, Missing>::Ok(...)` does not compile, so the
  example uses the accepted `Result::Ok(value = ...)`.
- The handoff describes a trace simulator for use without hardware.  This
  experiment had a device available and needed no simulator.
