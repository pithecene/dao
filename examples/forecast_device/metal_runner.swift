// This runner executes the plans examples/forecast_plans.dao prints and
// reports what each one moved, waited for and cost on a real GPU.
//
//   task forecast-device
//
// Each plan comes from the `record` lines on standard input, so this file
// knows the three kernels and the step vocabulary and nothing about either
// plan.  An upload or readback is a blit between a shared buffer and a
// private one, which is a real copy even on unified memory.  The
// handwritten section solves the same workload without a plan, for the
// comparison in docs/heterogeneous_experiment.md.

import Foundation
import Metal

let kernelSource = """
#include <metal_stdlib>
using namespace metal;
kernel void score_v1(device const int* wind [[buffer(0)]], device int* out [[buffer(1)]],
                     uint i [[thread_position_in_grid]]) { out[i] = 60 + 2 * wind[i]; }
kernel void high_wind(device const int* wind [[buffer(0)]], device int* out [[buffer(1)]],
                      uint i [[thread_position_in_grid]]) { out[i] = wind[i] >= 10 ? 1 : 0; }
kernel void adjust(device const int* base [[buffer(0)]], device const int* flag [[buffer(1)]],
                   device int* out [[buffer(2)]], uint i [[thread_position_in_grid]]) {
  out[i] = base[i] + 5 * flag[i];
}
"""

// The domain over which device arithmetic cannot overflow, so a device run
// and the checked CPU reference agree on every accepted input.
let windDomain: ClosedRange<Int32> = 0...1000

enum RunError: Error, CustomStringConvertible {
  case outOfDomain(Int32)
  case malformed(String)
  var description: String {
    switch self {
    case .outOfDomain(let wind): return "wind \(wind) is outside \(windDomain)"
    case .malformed(let what): return "malformed plan: \(what)"
    }
  }
}

struct Step {
  let op: String
  let args: [String]
}

struct Transfers {
  var uploads = 0
  var readbacks = 0
  var uploadBytes = 0
  var readbackBytes = 0
}

struct Report {
  var eta: [Int32] = []
  var observed: [String: [Int32]] = [:]
  var transfers = Transfers()
  var dispatches = 0
  var waits = 0
  var seconds = 0.0
}

func readPlans() -> (order: [String], steps: [String: [Step]]) {
  var order: [String] = []
  var steps: [String: [Step]] = [:]
  while let line = readLine() {
    let fields = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
    guard fields.count >= 3, fields[0] == "record" else { continue }
    if steps[fields[1]] == nil {
      order.append(fields[1])
      steps[fields[1]] = []
    }
    steps[fields[1]]!.append(Step(op: fields[2], args: Array(fields[3...])))
  }
  return (order, steps)
}

final class Gpu {
  let device: MTLDevice
  let queue: MTLCommandQueue
  let pipelines: [String: MTLComputePipelineState]

  init() throws {
    device = MTLCreateSystemDefaultDevice()!
    queue = device.makeCommandQueue()!
    let library = try device.makeLibrary(source: kernelSource, options: nil)
    var made: [String: MTLComputePipelineState] = [:]
    for name in ["score_v1", "high_wind", "adjust"] {
      made[name] = try device.makeComputePipelineState(function: library.makeFunction(name: name)!)
    }
    pipelines = made
  }

  func dispatch(_ encoder: MTLComputeCommandEncoder, _ kernel: String, _ buffers: [MTLBuffer], _ rows: Int) {
    encoder.setComputePipelineState(pipelines[kernel]!)
    for (index, buffer) in buffers.enumerated() {
      encoder.setBuffer(buffer, offset: 0, index: index)
    }
    encoder.dispatchThreads(
      MTLSize(width: rows, height: 1, depth: 1),
      threadsPerThreadgroup: MTLSize(width: pipelines[kernel]!.threadExecutionWidth, height: 1, depth: 1))
  }
}

func contents(_ buffer: MTLBuffer, _ rows: Int) -> [Int32] {
  Array(UnsafeBufferPointer(start: buffer.contents().bindMemory(to: Int32.self, capacity: rows), count: rows))
}

// ---------------------------------------------------------------
// Executing a plan's steps
// ---------------------------------------------------------------

func execute(_ steps: [Step], winds: [Int32], gpu: Gpu) throws -> Report {
  if let bad = winds.first(where: { !windDomain.contains($0) }) {
    throw RunError.outOfDomain(bad)
  }
  var report = Report()
  let rows = winds.count
  let bytes = rows * MemoryLayout<Int32>.stride
  var hostValues: [String: [Int32]] = [:]
  var deviceBuffers: [String: MTLBuffer] = [:]
  var staging: [String: MTLBuffer] = [:]
  var pendingHost: [String] = []
  var open: MTLCommandBuffer?

  func commandBuffer() -> MTLCommandBuffer {
    if let open { return open }
    let made = gpu.queue.makeCommandBuffer()!
    open = made
    return made
  }
  func privateBuffer(_ name: String) -> MTLBuffer {
    if let existing = deviceBuffers[name] { return existing }
    let made = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
    deviceBuffers[name] = made
    return made
  }
  // A host step needs its values, so finish the device work it depends on.
  func awaitDevice() {
    guard let running = open else { return }
    running.commit()
    running.waitUntilCompleted()
    report.waits += 1
    open = nil
    for name in pendingHost {
      hostValues[name] = contents(staging[name]!, rows)
    }
    pendingHost = []
  }
  // A value already on the host waits for nothing: a plan's cost must not
  // include a synchronization the plan never asked for.
  func hostValue(_ name: String) throws -> [Int32] {
    if pendingHost.contains(name) { awaitDevice() }
    guard let values = hostValues[name] else { throw RunError.malformed("no host value \(name)") }
    return values
  }

  let start = DispatchTime.now().uptimeNanoseconds
  for step in steps {
    switch step.op {
    case "read":
      hostValues[step.args[0]] = winds
    case "upload":
      let source = try hostValue(step.args[0])
      let shared = source.withUnsafeBytes { gpu.device.makeBuffer(bytes: $0.baseAddress!, length: bytes, options: .storageModeShared)! }
      let blit = commandBuffer().makeBlitCommandEncoder()!
      blit.copy(from: shared, sourceOffset: 0, to: privateBuffer(step.args[1]), destinationOffset: 0, size: bytes)
      blit.endEncoding()
      report.transfers.uploads += 1
      report.transfers.uploadBytes += bytes
    case "dispatch":
      let encoder = commandBuffer().makeComputeCommandEncoder()!
      var buffers = [privateBuffer(step.args[1])]
      if !step.args[2].isEmpty { buffers.append(privateBuffer(step.args[2])) }
      buffers.append(privateBuffer(step.args[3]))
      gpu.dispatch(encoder, step.args[0], buffers, rows)
      encoder.endEncoding()
      report.dispatches += 1
    case "readback":
      let shared = gpu.device.makeBuffer(length: bytes, options: .storageModeShared)!
      let blit = commandBuffer().makeBlitCommandEncoder()!
      blit.copy(from: privateBuffer(step.args[0]), sourceOffset: 0, to: shared, destinationOffset: 0, size: bytes)
      blit.endEncoding()
      staging[step.args[1]] = shared
      pendingHost.append(step.args[1])
      report.transfers.readbacks += 1
      report.transfers.readbackBytes += bytes
    case "host_adjust":
      let base = try hostValue(step.args[0])
      let flag = try hostValue(step.args[1])
      hostValues[step.args[2]] = zip(base, flag).map { $0 &+ 5 &* $1 }
    case "observe":
      report.observed[step.args[0]] = try hostValue(step.args[1])
    case "reply":
      report.eta = try hostValue(step.args[0])
    default:
      throw RunError.malformed("unknown step \(step.op)")
    }
  }
  // Work no host step waited for still counts toward the plan's cost.
  awaitDevice()
  report.seconds = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e9
  if report.eta.isEmpty { throw RunError.malformed("the plan never replies") }
  return report
}

// ---------------------------------------------------------------
// The same workload written by hand: one upload, three kernels, one readback
// ---------------------------------------------------------------

// HANDWRITTEN GLUE BEGIN
func handwritten(winds: [Int32], gpu: Gpu) throws -> Report {
  if let bad = winds.first(where: { !windDomain.contains($0) }) {
    throw RunError.outOfDomain(bad)
  }
  let rows = winds.count
  let bytes = rows * MemoryLayout<Int32>.stride
  let start = DispatchTime.now().uptimeNanoseconds
  let up = winds.withUnsafeBytes { gpu.device.makeBuffer(bytes: $0.baseAddress!, length: bytes, options: .storageModeShared)! }
  let wind = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
  let score = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
  let flag = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
  let eta = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
  let down = gpu.device.makeBuffer(length: bytes, options: .storageModeShared)!
  let commands = gpu.queue.makeCommandBuffer()!
  let upload = commands.makeBlitCommandEncoder()!
  upload.copy(from: up, sourceOffset: 0, to: wind, destinationOffset: 0, size: bytes)
  upload.endEncoding()
  var encoder = commands.makeComputeCommandEncoder()!
  gpu.dispatch(encoder, "score_v1", [wind, score], rows)
  encoder.endEncoding()
  encoder = commands.makeComputeCommandEncoder()!
  gpu.dispatch(encoder, "high_wind", [wind, flag], rows)
  encoder.endEncoding()
  encoder = commands.makeComputeCommandEncoder()!
  gpu.dispatch(encoder, "adjust", [score, flag, eta], rows)
  encoder.endEncoding()
  let readback = commands.makeBlitCommandEncoder()!
  readback.copy(from: eta, sourceOffset: 0, to: down, destinationOffset: 0, size: bytes)
  readback.endEncoding()
  commands.commit()
  commands.waitUntilCompleted()
  var report = Report()
  report.eta = contents(down, rows)
  report.transfers.uploads = 1
  report.transfers.readbacks = 1
  report.transfers.uploadBytes = bytes
  report.transfers.readbackBytes = bytes
  report.dispatches = 3
  report.waits = 1
  report.seconds = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e9
  return report
}
// HANDWRITTEN GLUE END

// ---------------------------------------------------------------
// Checks and measurements
// ---------------------------------------------------------------

func expected(_ winds: [Int32]) -> [Int32] {
  winds.map { 60 + 2 * $0 + 5 * ($0 >= 10 ? 1 : 0) }
}

var failures = 0
func check(_ condition: Bool, _ what: String) {
  print("\(condition ? "ok  " : "FAIL") \(what)")
  if !condition { failures += 1 }
}

func milliseconds(_ seconds: Double) -> String { String(format: "%.3f", seconds * 1000) }

func median(_ values: [Double]) -> Double { values.sorted()[values.count / 2] }

let (planOrder, plans) = readPlans()
// Without its plans the run would measure only the handwritten code and
// could still report success.
for required in ["straightforward", "proposed", "audited_shared"] where plans[required] == nil {
  print("no \(required) plan on standard input: pipe examples/forecast_plans into this runner")
  exit(2)
}
let gpu = try Gpu()
print("device\t\(gpu.device.name)\tunified_memory=\(gpu.device.hasUnifiedMemory)\tbackend=Metal")
print("plans\t\(planOrder.joined(separator: " "))")

// The fixture and the oracle.
print("\n--- oracle, N = 2, winds [8, 12] ---")
for name in planOrder {
  let run = try execute(plans[name]!, winds: [8, 12], gpu: gpu)
  check(run.eta == [76, 89], "\(name) returns \(run.eta)")
  print("     uploads \(run.transfers.uploads) (\(run.transfers.uploadBytes) B), readbacks \(run.transfers.readbacks) (\(run.transfers.readbackBytes) B), dispatches \(run.dispatches), waits \(run.waits)")
}
let glue = try handwritten(winds: [8, 12], gpu: gpu)
check(glue.eta == [76, 89], "handwritten returns \(glue.eta)")

// The observation the audited plan must keep, and when it arrives.
print("\n--- observation ---")
let audited = try execute(plans["audited_shared"]!, winds: [8, 12], gpu: gpu)
check(audited.observed["audit_score"] == [76, 84], "audit_score observed as \(audited.observed["audit_score"] ?? [])")
check(audited.waits == 2, "the callback forces a wait before the reply: \(audited.waits) waits")

// Both request modes on the error path: no plan touches the device.
print("\n--- error behavior ---")
for name in planOrder + ["handwritten"] {
  do {
    _ = name == "handwritten"
      ? try handwritten(winds: [8, 2_000_000_000], gpu: gpu)
      : try execute(plans[name]!, winds: [8, 2_000_000_000], gpu: gpu)
    check(false, "\(name) accepted an out-of-domain wind")
  } catch {
    check("\(error)".contains("outside"), "\(name) rejects: \(error)")
  }
}

// Cold: a fresh library and pipelines, then the first run.  Warm: the plans
// interleaved so neither always follows the other.
let modes: [(name: String, rows: Int, repeats: Int)] = [("live", 1, 201), ("backfill", 1_000_000, 41)]
print("\n--- timings, milliseconds ---")
print("mode\tN\tplan\tcold\twarm_median\twarm_min\tuploads\treadbacks\tbytes_moved\twaits")
for mode in modes {
  let winds = (0..<mode.rows).map { Int32($0 % 1001) }
  let truth = expected(winds)
  var cold: [String: Double] = [:]
  for name in planOrder + ["handwritten"] {
    let start = DispatchTime.now().uptimeNanoseconds
    let fresh = try Gpu()
    let run = name == "handwritten"
      ? try handwritten(winds: winds, gpu: fresh)
      : try execute(plans[name]!, winds: winds, gpu: fresh)
    cold[name] = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e9
    check(run.eta == truth, "\(mode.name) \(name) matches the CPU calculation")
  }
  var warm: [String: [Double]] = [:]
  var last: [String: Report] = [:]
  for iteration in 0..<mode.repeats {
    for name in planOrder + ["handwritten"] {
      let run = name == "handwritten"
        ? try handwritten(winds: winds, gpu: gpu)
        : try execute(plans[name]!, winds: winds, gpu: gpu)
      if iteration > 0 { warm[name, default: []].append(run.seconds) }
      last[name] = run
    }
  }
  for name in planOrder + ["handwritten"] {
    let run = last[name]!
    print([mode.name, "\(mode.rows)", name, milliseconds(cold[name]!), milliseconds(median(warm[name]!)),
           milliseconds(warm[name]!.min()!), "\(run.transfers.uploads)", "\(run.transfers.readbacks)",
           "\(run.transfers.uploadBytes + run.transfers.readbackBytes)", "\(run.waits)"].joined(separator: "\t"))
  }
}

print(failures == 0 ? "\nall device checks passed" : "\n\(failures) device check(s) failed")
exit(failures == 0 ? 0 : 1)
