// STOPGAP: this host driver is Swift, and docs/non_dao_inventory.md records it.
// All Metal compute kernels it executes are generated from authored Dao source
// (`examples/forecast_plans.dao`) by `dao_msl_emit`; zero handwritten Metal
// shader code or reimplemented reference formulas remain here.
//
//   task forecast-device
//
// Reads provenance-preserving `record` lines from standard input alongside the
// compiler-emitted `--msl <bundle.metal>`, `--kernels <manifest.tsv>`, and
// compiled Dao `--cpu-bin <forecast_plans>` executable.

import CryptoKit
import Foundation
import Metal

let windDomain: ClosedRange<Int32> = 0...1000
let expectedModule = "forecast_plans"
let expectedTarget = "metal-msl-2.4"
let expectedEffects = "pure_scalar_elementwise_checked_i32"

enum RunError: Error, CustomStringConvertible {
  case outOfDomain(Int32)
  case emptyBatch
  case noDevice
  case deviceExecutionFailed(String)
  case arithmeticTrap(String)
  case malformed(String)
  var description: String {
    switch self {
    case .outOfDomain(let wind): return "wind \(wind) is outside \(windDomain)"
    case .emptyBatch: return "an empty batch has nothing to run"
    case .noDevice: return "no usable Metal device"
    case .deviceExecutionFailed(let what): return "Metal command buffer failed: \(what)"
    case .arithmeticTrap(let what): return "checked i32 arithmetic trap on device: \(what)"
    case .malformed(let what): return "malformed plan: \(what)"
    }
  }
}

struct KernelSpec: Equatable {
  let moduleName: String
  let operation: String
  let origin: String
  let target: String
  let arity: Int
  let paramTypes: [String]
  let returnType: String
  let effects: String
}

struct RunnerArgs {
  let mslPath: String
  let manifestPath: String
  let cpuBinPath: String
}

func sha256Hex(_ text: String) -> String {
  SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined()
}

func isValidSha256Hex(_ text: String) -> Bool {
  guard text.count == 64 else { return false }
  return text.unicodeScalars.allSatisfy { scalar in
    (scalar.value >= 48 && scalar.value <= 57) || (scalar.value >= 97 && scalar.value <= 102)
  }
}

func parseArgs(_ argv: [String]) throws -> RunnerArgs {
  var mslPath = ""
  var manifestPath = ""
  var cpuBinPath = ""
  var idx = 1
  while idx < argv.count {
    let arg = argv[idx]
    guard idx + 1 < argv.count else {
      throw RunError.malformed("missing value for \(arg)")
    }
    if arg == "--msl" {
      mslPath = argv[idx + 1]
    } else if arg == "--kernels" {
      manifestPath = argv[idx + 1]
    } else if arg == "--cpu-bin" {
      cpuBinPath = argv[idx + 1]
    } else {
      throw RunError.malformed("unknown option \(arg)")
    }
    idx += 2
  }
  guard !mslPath.isEmpty, !manifestPath.isEmpty, !cpuBinPath.isEmpty else {
    throw RunError.malformed(
      "usage: metal_runner --msl <bundle.metal> --kernels <manifest.tsv> --cpu-bin <forecast_plans>"
    )
  }
  return RunnerArgs(mslPath: mslPath, manifestPath: manifestPath, cpuBinPath: cpuBinPath)
}

func parseKernelManifest(_ text: String, expectedBundleSha256: String) throws -> [String: KernelSpec] {
  var specs: [String: KernelSpec] = [:]
  var manifestDigest: String?
  for rawLine in text.split(whereSeparator: \.isNewline) {
    let fields = rawLine.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
    if fields.first == "bundle_sha256" {
      guard fields.count == 2, isValidSha256Hex(fields[1]) else {
        throw RunError.malformed("invalid bundle_sha256 row: \(rawLine)")
      }
      if manifestDigest != nil {
        throw RunError.malformed("duplicate bundle_sha256 row in kernel manifest")
      }
      manifestDigest = fields[1]
      continue
    }
    guard fields.count == 9, fields[0] == "kernel" else {
      throw RunError.malformed("invalid kernel manifest row: \(rawLine)")
    }
    guard let arity = Int(fields[5]), arity >= 1 else {
      throw RunError.malformed("invalid kernel arity in row: \(rawLine)")
    }
    let paramTypes = fields[6].split(separator: ",").map(String.init)
    guard paramTypes.count == arity, paramTypes.allSatisfy({ $0 == "i32" }) else {
      throw RunError.malformed("unsupported kernel parameter signature \(fields[6])")
    }
    let spec = KernelSpec(
      moduleName: fields[1],
      operation: fields[2],
      origin: fields[3],
      target: fields[4],
      arity: arity,
      paramTypes: paramTypes,
      returnType: fields[7],
      effects: fields[8]
    )
    guard spec.moduleName == expectedModule else {
      throw RunError.malformed("unexpected kernel module \(spec.moduleName) for \(spec.operation)")
    }
    guard spec.target == expectedTarget else {
      throw RunError.malformed("unsupported kernel target \(spec.target) for \(spec.operation)")
    }
    guard spec.returnType == "i32", spec.effects == expectedEffects else {
      throw RunError.malformed("unsupported kernel contract \(spec.returnType) / \(spec.effects)")
    }
    if specs[spec.operation] != nil {
      throw RunError.malformed("duplicate kernel manifest entry \(spec.operation)")
    }
    specs[spec.operation] = spec
  }
  guard let manifestDigest else {
    throw RunError.malformed("missing bundle_sha256 header in kernel manifest")
  }
  guard manifestDigest == expectedBundleSha256 else {
    throw RunError.malformed(
      "kernel manifest bundle_sha256 mismatch: manifest=\(manifestDigest), bundle=\(expectedBundleSha256)"
    )
  }
  guard !specs.isEmpty else {
    throw RunError.malformed("empty kernel manifest")
  }
  return specs
}

func verifyCommandBufferCompletion(
  status: MTLCommandBufferStatus,
  errorDescription: String?,
  context: String
) throws {
  if let errorDescription {
    throw RunError.deviceExecutionFailed("\(context) (status=\(status.rawValue)): \(errorDescription)")
  }
  guard status == .completed else {
    throw RunError.deviceExecutionFailed(
      "\(context) ended in status \(status.rawValue), expected completed (\(MTLCommandBufferStatus.completed.rawValue))"
    )
  }
}

func verifyCommandBuffer(_ commandBuffer: MTLCommandBuffer, context: String) throws {
  try verifyCommandBufferCompletion(
    status: commandBuffer.status,
    errorDescription: commandBuffer.error?.localizedDescription,
    context: context
  )
}

enum KernelEvalOutcome: Equatable, CustomStringConvertible {
  case value(Int32)
  case trapped
  var description: String {
    switch self {
    case .value(let v): return "value(\(v))"
    case .trapped: return "arithmetic_trap"
    }
  }
}

struct CpuDomainOracle {
  let sampleEta: [Int32]
  let sampleAuditScore: [Int32]
  let domainEta: [Int32]
}

final class CpuOracle {
  let executableURL: URL

  init(path: String) throws {
    let url = URL(fileURLWithPath: path)
    guard FileManager.default.isExecutableFile(atPath: url.path) else {
      throw RunError.malformed("compiled Dao CPU binary is not executable: \(path)")
    }
    self.executableURL = url
  }

  private func runProcess(_ arguments: [String]) throws -> (
    status: Int32,
    reason: Process.TerminationReason,
    stdout: String,
    stderr: String
  ) {
    let proc = Process()
    proc.executableURL = executableURL
    proc.arguments = arguments
    let outPipe = Pipe()
    let errPipe = Pipe()
    proc.standardOutput = outPipe
    proc.standardError = errPipe
    try proc.run()
    let outData = outPipe.fileHandleForReading.readDataToEndOfFile()
    let errData = errPipe.fileHandleForReading.readDataToEndOfFile()
    proc.waitUntilExit()
    return (
      status: proc.terminationStatus,
      reason: proc.terminationReason,
      stdout: String(decoding: outData, as: UTF8.self),
      stderr: String(decoding: errData, as: UTF8.self)
    )
  }

  private func parseCsvI32(_ csv: String, context: String) throws -> [Int32] {
    let parts = csv.split(separator: ",", omittingEmptySubsequences: false)
    var out: [Int32] = []
    out.reserveCapacity(parts.count)
    for part in parts {
      guard let val = Int32(part) else {
        throw RunError.malformed("invalid i32 '\(part)' in CPU oracle \(context)")
      }
      out.append(val)
    }
    return out
  }

  func loadDomainOracle() throws -> CpuDomainOracle {
    let res = try runProcess(["--oracle-domain"])
    guard res.reason == .exit, res.status == 0 else {
      throw RunError.malformed(
        "CPU --oracle-domain failed (reason=\(res.reason.rawValue), status=\(res.status)): \(res.stderr)"
      )
    }
    var sampleEta: [Int32]?
    var sampleAudit: [Int32]?
    var domainEta: [Int32]?
    for line in res.stdout.split(whereSeparator: \.isNewline) {
      let cols = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
      guard cols.count == 2 else {
        throw RunError.malformed("unexpected CPU --oracle-domain line: \(line)")
      }
      switch cols[0] {
      case "sample_eta":
        sampleEta = try parseCsvI32(cols[1], context: "sample_eta")
      case "sample_audit_score":
        sampleAudit = try parseCsvI32(cols[1], context: "sample_audit_score")
      case "domain_eta":
        domainEta = try parseCsvI32(cols[1], context: "domain_eta")
      default:
        throw RunError.malformed("unknown CPU --oracle-domain key \(cols[0])")
      }
    }
    guard let sampleEta, sampleEta.count == 2,
          let sampleAudit, sampleAudit.count == 2,
          let domainEta, domainEta.count == 1001 else {
      throw RunError.malformed("incomplete CPU --oracle-domain payload")
    }
    return CpuDomainOracle(
      sampleEta: sampleEta,
      sampleAuditScore: sampleAudit,
      domainEta: domainEta
    )
  }

  func evaluateCycledBatch(rows: Int) throws -> [Int32] {
    let tempURL = FileManager.default.temporaryDirectory
      .appendingPathComponent("dao_cpu_oracle_\(ProcessInfo.processInfo.processIdentifier)_\(UUID().uuidString).txt")
    defer { try? FileManager.default.removeItem(at: tempURL) }
    let res = try runProcess(["--oracle-cycled", "\(rows)", tempURL.path])
    guard res.reason == .exit, res.status == 0 else {
      throw RunError.malformed(
        "CPU --oracle-cycled \(rows) failed (reason=\(res.reason.rawValue), status=\(res.status)): \(res.stderr)"
      )
    }
    let text = try String(contentsOf: tempURL, encoding: .utf8)
    var values: [Int32] = []
    values.reserveCapacity(rows)
    for line in text.split(whereSeparator: \.isNewline) {
      guard let v = Int32(line) else {
        throw RunError.malformed("invalid i32 line '\(line)' from CPU --oracle-cycled")
      }
      values.append(v)
    }
    guard values.count == rows else {
      throw RunError.malformed("CPU --oracle-cycled returned \(values.count) rows, expected \(rows)")
    }
    return values
  }

  func evaluateKernel(_ kernel: String, args: [Int32]) throws -> KernelEvalOutcome {
    let res = try runProcess(["--eval-kernel", kernel] + args.map(String.init))
    if res.reason == .uncaughtSignal {
      return .trapped
    }
    guard res.reason == .exit, res.status == 0 else {
      throw RunError.malformed(
        "CPU --eval-kernel \(kernel) failed unexpectedly (status=\(res.status)): \(res.stderr)"
      )
    }
    let trimmed = res.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let value = Int32(trimmed) else {
      throw RunError.malformed("non-i32 stdout '\(trimmed)' from CPU --eval-kernel \(kernel)")
    }
    return .value(value)
  }
}

func validate(_ winds: [Int32]) throws {
  if winds.isEmpty { throw RunError.emptyBatch }
  if let bad = winds.first(where: { !windDomain.contains($0) }) {
    throw RunError.outOfDomain(bad)
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
  var readProvenance: [String] = []
  var eta: [Int32] = []
  var observed: [String: [Int32]] = [:]
  var transfers = Transfers()
  var dispatches = 0
  var waits = 0
  var seconds = 0.0
}

let stepArguments = [
  "read": 6, "upload": 2, "dispatch": 5, "readback": 2,
  "host_adjust": 4, "release": 1, "observe": 2, "reply": 1,
]

func checkShape(_ step: Step, kernels: [String: KernelSpec]) throws {
  guard stepArguments[step.op] == step.args.count else {
    throw RunError.malformed("\(step.op) with \(step.args.count) arguments")
  }
  if step.op == "read" {
    guard Int32(step.args[0]) != nil, Int32(step.args[1]) != nil, Int32(step.args[2]) != nil,
          ["as_of_decision", "latest_now"].contains(step.args[3]), Int32(step.args[4]) != nil,
          !step.args[5].isEmpty else {
      throw RunError.malformed("invalid read provenance \(step.args)")
    }
  } else if step.op == "dispatch" {
    guard let spec = kernels[step.args[0]] else {
      throw RunError.malformed("unknown or unlowered kernel \(step.args[0])")
    }
    guard step.args[1] == "v1" else {
      throw RunError.malformed("unsupported device model \(step.args[1])")
    }
    let suppliedInputs = step.args[3].isEmpty ? 1 : 2
    guard suppliedInputs == spec.arity else {
      throw RunError.malformed("\(step.args[0]) takes \(spec.arity) input(s), got \(suppliedInputs)")
    }
  } else if step.op == "host_adjust" {
    guard step.args[0] == "v1" else {
      throw RunError.malformed("unsupported host model \(step.args[0])")
    }
  }
}

func readPlans(kernels: [String: KernelSpec]) throws -> (order: [String], steps: [String: [Step]]) {
  var order: [String] = []
  var steps: [String: [Step]] = [:]
  var announced: Int?
  var seen = 0
  while let line = readLine() {
    let fields = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
    if fields.count == 2 && fields[0] == "records" { announced = Int(fields[1]) }
    guard fields.count >= 3, fields[0] == "record" else { continue }
    seen += 1
    if steps[fields[1]] == nil {
      order.append(fields[1])
      steps[fields[1]] = []
    }
    let step = Step(op: fields[2], args: Array(fields[3...]))
    try checkShape(step, kernels: kernels)
    steps[fields[1]]!.append(step)
  }
  guard announced == seen else {
    throw RunError.malformed("\(seen) records, announced \(announced.map(String.init) ?? "none")")
  }
  return (order, steps)
}

final class Gpu {
  let device: MTLDevice
  let queue: MTLCommandQueue
  let pipelines: [String: MTLComputePipelineState]
  let specs: [String: KernelSpec]

  init(mslSource: String, specs: [String: KernelSpec]) throws {
    guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
      throw RunError.noDevice
    }
    self.device = device
    self.queue = queue
    self.specs = specs
    let library = try device.makeLibrary(source: mslSource, options: nil)
    var made: [String: MTLComputePipelineState] = [:]
    for name in specs.keys.sorted() {
      guard let fn = library.makeFunction(name: name) else {
        throw RunError.malformed("generated Metal library is missing entry point \(name)")
      }
      made[name] = try device.makeComputePipelineState(function: fn)
    }
    pipelines = made
  }

  func makeStatusBuffer() -> MTLBuffer {
    var zero: UInt32 = 0
    return device.makeBuffer(
      bytes: &zero,
      length: MemoryLayout<UInt32>.stride,
      options: .storageModeShared
    )!
  }

  func checkStatus(_ statusBuffer: MTLBuffer, context: String) throws {
    let status = statusBuffer.contents().load(as: UInt32.self)
    if status != 0 {
      throw RunError.arithmeticTrap(context)
    }
  }

  func dispatch(
    _ encoder: MTLComputeCommandEncoder,
    _ kernel: String,
    _ buffers: [MTLBuffer],
    _ statusBuffer: MTLBuffer,
    _ rows: Int
  ) {
    let pipeline = pipelines[kernel]!
    encoder.setComputePipelineState(pipeline)
    for (index, buffer) in buffers.enumerated() {
      encoder.setBuffer(buffer, offset: 0, index: index)
    }
    encoder.setBuffer(statusBuffer, offset: 0, index: buffers.count)
    encoder.dispatchThreads(
      MTLSize(width: rows, height: 1, depth: 1),
      threadsPerThreadgroup: MTLSize(width: pipeline.threadExecutionWidth, height: 1, depth: 1))
  }
}

func contents(_ buffer: MTLBuffer, _ rows: Int) -> [Int32] {
  Array(UnsafeBufferPointer(start: buffer.contents().bindMemory(to: Int32.self, capacity: rows), count: rows))
}

func execute(_ steps: [Step], winds: [Int32], gpu: Gpu) throws -> Report {
  try validate(winds)
  var report = Report()
  let rows = winds.count
  let bytes = rows * MemoryLayout<Int32>.stride
  var hostValues: [String: [Int32]] = [:]
  var deviceBuffers: [String: MTLBuffer] = [:]
  var releasedBuffers: Set<String> = []
  var staging: [String: MTLBuffer] = [:]
  var pendingHost: [String] = []
  var open: MTLCommandBuffer?
  var openStatus: MTLBuffer?

  func commandBuffer() -> (MTLCommandBuffer, MTLBuffer) {
    if let open, let openStatus { return (open, openStatus) }
    let made = gpu.queue.makeCommandBuffer()!
    let status = gpu.makeStatusBuffer()
    open = made
    openStatus = status
    return (made, status)
  }
  func bindHost(_ name: String, _ values: [Int32]) throws {
    if hostValues[name] != nil || staging[name] != nil {
      throw RunError.malformed("duplicate host binding \(name)")
    }
    hostValues[name] = values
  }
  func allocateBuffer(_ name: String) throws -> MTLBuffer {
    if deviceBuffers[name] != nil || releasedBuffers.contains(name) {
      throw RunError.malformed("duplicate device buffer \(name)")
    }
    let made = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
    deviceBuffers[name] = made
    return made
  }
  func liveBuffer(_ name: String) throws -> MTLBuffer {
    if releasedBuffers.contains(name) {
      throw RunError.malformed("device buffer \(name) used after release")
    }
    guard let existing = deviceBuffers[name] else {
      throw RunError.malformed("undefined device buffer \(name)")
    }
    return existing
  }
  func awaitDevice() throws {
    guard let running = open, let status = openStatus else { return }
    open = nil
    openStatus = nil
    running.commit()
    running.waitUntilCompleted()
    report.waits += 1
    try verifyCommandBuffer(running, context: "plan dispatch")
    try gpu.checkStatus(status, context: "plan dispatch")
    for name in pendingHost {
      hostValues[name] = contents(staging[name]!, rows)
    }
    pendingHost = []
  }
  func hostValue(_ name: String) throws -> [Int32] {
    if pendingHost.contains(name) { try awaitDevice() }
    guard let values = hostValues[name] else { throw RunError.malformed("no host value \(name)") }
    return values
  }

  let start = DispatchTime.now().uptimeNanoseconds
  for step in steps {
    try checkShape(step, kernels: gpu.specs)
    switch step.op {
    case "read":
      report.readProvenance.append(step.args[0...4].joined(separator: ":"))
      try bindHost(step.args[5], winds)
    case "upload":
      let source = try hostValue(step.args[0])
      let dest = try allocateBuffer(step.args[1])
      let shared = source.withUnsafeBytes {
        gpu.device.makeBuffer(bytes: $0.baseAddress!, length: bytes, options: .storageModeShared)!
      }
      let (cmd, _) = commandBuffer()
      let blit = cmd.makeBlitCommandEncoder()!
      blit.copy(from: shared, sourceOffset: 0, to: dest, destinationOffset: 0, size: bytes)
      blit.endEncoding()
      report.transfers.uploads += 1
      report.transfers.uploadBytes += bytes
    case "dispatch":
      var buffers = [try liveBuffer(step.args[2])]
      if !step.args[3].isEmpty { buffers.append(try liveBuffer(step.args[3])) }
      buffers.append(try allocateBuffer(step.args[4]))
      let (cmd, status) = commandBuffer()
      let encoder = cmd.makeComputeCommandEncoder()!
      gpu.dispatch(encoder, step.args[0], buffers, status, rows)
      encoder.endEncoding()
      report.dispatches += 1
    case "readback":
      let src = try liveBuffer(step.args[0])
      if hostValues[step.args[1]] != nil || staging[step.args[1]] != nil {
        throw RunError.malformed("duplicate host binding \(step.args[1])")
      }
      let shared = gpu.device.makeBuffer(length: bytes, options: .storageModeShared)!
      let (cmd, _) = commandBuffer()
      let blit = cmd.makeBlitCommandEncoder()!
      blit.copy(from: src, sourceOffset: 0, to: shared, destinationOffset: 0, size: bytes)
      blit.endEncoding()
      staging[step.args[1]] = shared
      pendingHost.append(step.args[1])
      report.transfers.readbacks += 1
      report.transfers.readbackBytes += bytes
    case "host_adjust":
      let base = try hostValue(step.args[1])
      let flag = try hostValue(step.args[2])
      let combined = try zip(base, flag).map { lhs, rhs -> Int32 in
        let prod = rhs.multipliedReportingOverflow(by: 5)
        if prod.overflow { throw RunError.arithmeticTrap("host_adjust") }
        let sum = lhs.addingReportingOverflow(prod.partialValue)
        if sum.overflow { throw RunError.arithmeticTrap("host_adjust") }
        return sum.partialValue
      }
      try bindHost(step.args[3], combined)
    case "release":
      let name = step.args[0]
      if releasedBuffers.contains(name) {
        throw RunError.malformed("double release of device buffer \(name)")
      }
      guard deviceBuffers[name] != nil else {
        throw RunError.malformed("release of unknown device buffer \(name)")
      }
      try awaitDevice()
      deviceBuffers.removeValue(forKey: name)
      releasedBuffers.insert(name)
    case "observe":
      report.observed[step.args[0]] = try hostValue(step.args[1])
    case "reply":
      if !report.eta.isEmpty { throw RunError.malformed("plan replies more than once") }
      report.eta = try hostValue(step.args[0])
    default:
      throw RunError.malformed("unknown step \(step.op)")
    }
  }
  try awaitDevice()
  report.seconds = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e9
  if report.eta.isEmpty { throw RunError.malformed("the plan never replies") }
  return report
}

// HANDWRITTEN GLUE BEGIN
func handwritten(winds: [Int32], gpu: Gpu) throws -> Report {
  try validate(winds)
  let rows = winds.count
  let bytes = rows * MemoryLayout<Int32>.stride
  let start = DispatchTime.now().uptimeNanoseconds
  let up = winds.withUnsafeBytes {
    gpu.device.makeBuffer(bytes: $0.baseAddress!, length: bytes, options: .storageModeShared)!
  }
  let wind = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
  let score = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
  let flag = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
  let eta = gpu.device.makeBuffer(length: bytes, options: .storageModePrivate)!
  let down = gpu.device.makeBuffer(length: bytes, options: .storageModeShared)!
  let status = gpu.makeStatusBuffer()
  let commands = gpu.queue.makeCommandBuffer()!
  let upload = commands.makeBlitCommandEncoder()!
  upload.copy(from: up, sourceOffset: 0, to: wind, destinationOffset: 0, size: bytes)
  upload.endEncoding()
  var encoder = commands.makeComputeCommandEncoder()!
  gpu.dispatch(encoder, "score_v1", [wind, score], status, rows)
  encoder.endEncoding()
  encoder = commands.makeComputeCommandEncoder()!
  gpu.dispatch(encoder, "high_wind", [wind, flag], status, rows)
  encoder.endEncoding()
  encoder = commands.makeComputeCommandEncoder()!
  gpu.dispatch(encoder, "adjust", [score, flag, eta], status, rows)
  encoder.endEncoding()
  let readback = commands.makeBlitCommandEncoder()!
  readback.copy(from: eta, sourceOffset: 0, to: down, destinationOffset: 0, size: bytes)
  readback.endEncoding()
  commands.commit()
  commands.waitUntilCompleted()
  try verifyCommandBuffer(commands, context: "handwritten")
  try gpu.checkStatus(status, context: "handwritten")
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

// Direct single-kernel dispatch without the 0...1000 domain prefilter so
// differential tests can verify that generated Metal kernels match compiled Dao
// CPU execution across arbitrary i32 inputs and signed overflow traps.
func rawKernelDispatch(_ kernel: String, inputs: [[Int32]], gpu: Gpu) throws -> [Int32] {
  let rows = inputs[0].count
  let bytes = rows * MemoryLayout<Int32>.stride
  let inBuffers = inputs.map { arr in
    arr.withUnsafeBytes {
      gpu.device.makeBuffer(bytes: $0.baseAddress!, length: bytes, options: .storageModeShared)!
    }
  }
  let outBuffer = gpu.device.makeBuffer(length: bytes, options: .storageModeShared)!
  let status = gpu.makeStatusBuffer()
  let commands = gpu.queue.makeCommandBuffer()!
  let encoder = commands.makeComputeCommandEncoder()!
  gpu.dispatch(encoder, kernel, inBuffers + [outBuffer], status, rows)
  encoder.endEncoding()
  commands.commit()
  commands.waitUntilCompleted()
  try verifyCommandBuffer(commands, context: kernel)
  try gpu.checkStatus(status, context: kernel)
  return contents(outBuffer, rows)
}

var failures = 0
func check(_ condition: Bool, _ what: String) {
  print("\(condition ? "ok  " : "FAIL") \(what)")
  if !condition { failures += 1 }
}

func milliseconds(_ seconds: Double) -> String { String(format: "%.3f", seconds * 1000) }
func median(_ values: [Double]) -> Double { values.sorted()[values.count / 2] }

let runnerArgs: RunnerArgs
let mslSource: String
let mslSha256: String
let manifestText: String
let planOrder: [String]
let plans: [String: [Step]]
let kernelSpecs: [String: KernelSpec]
let cpuOracle: CpuOracle
let cpuDomain: CpuDomainOracle
let gpu: Gpu
do {
  runnerArgs = try parseArgs(CommandLine.arguments)
  mslSource = try String(contentsOfFile: runnerArgs.mslPath, encoding: .utf8)
  mslSha256 = sha256Hex(mslSource)
  manifestText = try String(contentsOfFile: runnerArgs.manifestPath, encoding: .utf8)
  kernelSpecs = try parseKernelManifest(manifestText, expectedBundleSha256: mslSha256)
  cpuOracle = try CpuOracle(path: runnerArgs.cpuBinPath)
  cpuDomain = try cpuOracle.loadDomainOracle()
  (planOrder, plans) = try readPlans(kernels: kernelSpecs)
  gpu = try Gpu(mslSource: mslSource, specs: kernelSpecs)
} catch {
  print("\(error)")
  exit(2)
}
for required in ["straightforward", "proposed", "audited_shared"] where plans[required] == nil {
  print("no \(required) plan on standard input: pipe examples/forecast_plans into this runner")
  exit(2)
}
print("device\t\(gpu.device.name)\tunified_memory=\(gpu.device.hasUnifiedMemory)\tbackend=Metal\ttarget=\(expectedTarget)\tbundle_sha256=\(mslSha256)")
print("plans\t\(planOrder.joined(separator: " "))")

print("\n--- compiler-generated kernel provenance and bundle binding ---")
check(isValidSha256Hex(mslSha256), "manifest cryptographically bound to Metal bundle (\(mslSha256))")
for name in ["score_v1", "high_wind", "adjust"] {
  if let spec = kernelSpecs[name] {
    check(
      spec.moduleName == expectedModule && spec.target == expectedTarget && spec.effects == expectedEffects,
      "\(spec.moduleName)::\(spec.operation) lowered from \(spec.origin) (\(spec.paramTypes.joined(separator: ", ")) -> \(spec.returnType), \(spec.effects))"
    )
  } else {
    check(false, "missing compiler-generated kernel \(name)")
  }
}

print("\n--- command-buffer status and error guard checks ---")
do {
  try verifyCommandBufferCompletion(status: .completed, errorDescription: nil, context: "unit_ok")
  check(true, "accepts completed MTLCommandBuffer with nil error")
} catch {
  check(false, "rejected valid MTLCommandBuffer completion: \(error)")
}
for (badStatus, badErr, label) in [
  (MTLCommandBufferStatus.error, "GPU hang / fault" as String?, "error status with description"),
  (MTLCommandBufferStatus.error, nil, "error status with dao_status == 0"),
  (MTLCommandBufferStatus.notEnqueued, nil, "incomplete status"),
  (MTLCommandBufferStatus.completed, "driver reported error" as String?, "non-nil error on completed status"),
] {
  do {
    try verifyCommandBufferCompletion(status: badStatus, errorDescription: badErr, context: label)
    check(false, "accepted failed MTLCommandBuffer (\(label))")
  } catch {
    check("\(error)".contains("Metal command buffer failed"), "rejects \(label): \(error)")
  }
}

print("\n--- differential CPU oracle, N = 2, winds [8, 12] ---")
var provenances = Set<String>()
for name in planOrder {
  let run = try execute(plans[name]!, winds: [8, 12], gpu: gpu)
  provenances.insert(run.readProvenance.joined(separator: ","))
  check(run.eta == cpuDomain.sampleEta, "\(name) matches compiled Dao CPU \(cpuDomain.sampleEta)")
  print("     uploads \(run.transfers.uploads) (\(run.transfers.uploadBytes) B), readbacks \(run.transfers.readbacks) (\(run.transfers.readbackBytes) B), dispatches \(run.dispatches), waits \(run.waits)")
}
check(provenances == ["1:7:600:as_of_decision:540"], "all device plans carry identical point-in-time read provenance")
let glue = try handwritten(winds: [8, 12], gpu: gpu)
check(glue.eta == cpuDomain.sampleEta, "handwritten matches compiled Dao CPU \(cpuDomain.sampleEta)")

print("\n--- differential CPU oracle across 0...1000 and signed i32 boundary / trap cases ---")
let domainWinds = (0...1000).map(Int32.init)
for name in planOrder + ["handwritten"] {
  let run = name == "handwritten"
    ? try handwritten(winds: domainWinds, gpu: gpu)
    : try execute(plans[name]!, winds: domainWinds, gpu: gpu)
  check(
    run.eta == cpuDomain.domainEta,
    "\(name) matches compiled Dao CPU across all 1001 bounded wind inputs (0...1000)"
  )
}

let differentialKernelCases: [(kernel: String, args: [Int32], expectTrap: Bool)] = [
  ("score_v1", [0], false),
  ("score_v1", [1000], false),
  ("score_v1", [1_073_741_793], false),
  ("score_v1", [1_073_741_794], true),
  ("score_v1", [1_500_000_000], true),
  ("score_v1", [-1_073_741_824], false),
  ("score_v1", [-1_073_741_825], true),
  ("score_v1", [-1_073_741_854], true),
  ("high_wind", [9], false),
  ("high_wind", [10], false),
  ("adjust", [2_147_483_642, 1], false),
  ("adjust", [2_147_483_643, 1], true),
  ("adjust", [-2_147_483_643, -1], false),
  ("adjust", [-2_147_483_644, -1], true),
  ("adjust", [0, 429_496_730], true),
]
for testCase in differentialKernelCases {
  let cpuOutcome = try cpuOracle.evaluateKernel(testCase.kernel, args: testCase.args)
  let gpuOutcome: KernelEvalOutcome
  do {
    let out = try rawKernelDispatch(
      testCase.kernel,
      inputs: testCase.args.map { [$0] },
      gpu: gpu
    )
    gpuOutcome = .value(out[0])
  } catch RunError.arithmeticTrap {
    gpuOutcome = .trapped
  }
  let expectedKindOk = testCase.expectTrap ? (cpuOutcome == .trapped) : (cpuOutcome != .trapped)
  check(
    cpuOutcome == gpuOutcome && expectedKindOk,
    "CPU Dao and Metal \(testCase.kernel)(\(testCase.args)) -> \(gpuOutcome)"
  )
}

print("\n--- observation ---")
let audited = try execute(plans["audited_shared"]!, winds: [8, 12], gpu: gpu)
check(
  audited.observed["audit_score"] == cpuDomain.sampleAuditScore,
  "audit_score matches compiled Dao CPU \(cpuDomain.sampleAuditScore)"
)
check(audited.waits == 2, "the callback forces a wait before the reply: \(audited.waits) waits")

print("\n--- record, manifest, and bundle SHA-256 checks ---")
let badRecords: [(Step, String)] = [
  (Step(op: "read", args: ["1", "7", "600", "unbounded", "540", "wind"]), "invalid read provenance"),
  (Step(op: "dispatch", args: ["score_v1", "v2", "wind", "", "score"]), "unsupported device model"),
  (Step(op: "dispatch", args: ["uncompiled_kernel", "v1", "wind", "", "score"]), "unknown or unlowered kernel"),
  (Step(op: "dispatch", args: ["score_v1", "v1", "wind", "extra", "score"]), "takes 1 input(s)"),
  (Step(op: "host_adjust", args: ["v2", "score", "flag", "eta"]), "unsupported host model"),
]
for (badStep, expectedReason) in badRecords {
  do {
    try checkShape(badStep, kernels: kernelSpecs)
    check(false, "accepted malformed step \(badStep.op)")
  } catch {
    check("\(error)".contains(expectedReason), "rejects \(badStep.op): \(error)")
  }
}
do {
  let foreignManifest =
    "bundle_sha256\t\(mslSha256)\n" +
    "kernel\tother_mod\tscore_v1\tf.dao:1:1\tmetal-msl-2.4\t1\ti32\ti32\tpure_scalar_elementwise_checked_i32\n"
  _ = try parseKernelManifest(foreignManifest, expectedBundleSha256: mslSha256)
  check(false, "accepted kernel manifest from foreign module")
} catch {
  check("\(error)".contains("unexpected kernel module"), "rejects foreign-module kernel manifest: \(error)")
}
do {
  _ = try parseKernelManifest(
    "kernel\tforecast_plans\tscore_v1\tf.dao:1:1\tmetal-msl-2.4\t1\ti32\ti32\tpure_scalar_elementwise_checked_i32\n",
    expectedBundleSha256: mslSha256
  )
  check(false, "accepted kernel manifest without bundle_sha256")
} catch {
  check("\(error)".contains("missing bundle_sha256"), "rejects manifest missing bundle_sha256: \(error)")
}
do {
  _ = try parseKernelManifest(manifestText, expectedBundleSha256: sha256Hex(mslSource + "\n// tampered\n"))
  check(false, "accepted tampered Metal bundle against manifest SHA-256")
} catch {
  check("\(error)".contains("bundle_sha256 mismatch"), "rejects tampered Metal bundle digest: \(error)")
}

print("\n--- buffer lifetime checks ---")
let readStep = Step(op: "read", args: ["1", "7", "600", "as_of_decision", "540", "wind"])
let validReleased = try execute([
  readStep,
  Step(op: "upload", args: ["wind", "wind_shared"]),
  Step(op: "dispatch", args: ["score_v1", "v1", "wind_shared", "", "score"]),
  Step(op: "dispatch", args: ["high_wind", "v1", "wind_shared", "", "flag"]),
  Step(op: "release", args: ["wind_shared"]),
  Step(op: "dispatch", args: ["adjust", "v1", "score", "flag", "eta"]),
  Step(op: "release", args: ["score"]),
  Step(op: "release", args: ["flag"]),
  Step(op: "readback", args: ["eta", "eta_host"]),
  Step(op: "release", args: ["eta"]),
  Step(op: "reply", args: ["eta_host"]),
], winds: [8, 12], gpu: gpu)
check(validReleased.eta == cpuDomain.sampleEta, "post-consumer buffer releases preserve \(validReleased.eta)")

let lifetimeRejections: [(String, [Step], String)] = [
  ("premature release", [
    readStep,
    Step(op: "upload", args: ["wind", "wind_shared"]),
    Step(op: "dispatch", args: ["score_v1", "v1", "wind_shared", "", "score"]),
    Step(op: "release", args: ["wind_shared"]),
    Step(op: "dispatch", args: ["high_wind", "v1", "wind_shared", "", "flag"]),
  ], "used after release"),
  ("unknown release", [readStep, Step(op: "release", args: ["missing"])], "release of unknown device buffer"),
  ("double release", [
    readStep,
    Step(op: "upload", args: ["wind", "wind_shared"]),
    Step(op: "release", args: ["wind_shared"]),
    Step(op: "release", args: ["wind_shared"]),
  ], "double release of device buffer"),
  ("undefined input", [
    readStep,
    Step(op: "dispatch", args: ["score_v1", "v1", "missing", "", "score"]),
  ], "undefined device buffer"),
]
for (label, badPlan, expectedReason) in lifetimeRejections {
  do {
    _ = try execute(badPlan, winds: [8, 12], gpu: gpu)
    check(false, "accepted \(label)")
  } catch {
    check("\(error)".contains(expectedReason), "rejects \(label): \(error)")
  }
}

print("\n--- error behavior ---")
for (winds, reason) in [([Int32(8), 2_000_000_000], "outside"), ([], "empty")] {
  for name in planOrder + ["handwritten"] {
    do {
      _ = name == "handwritten"
        ? try handwritten(winds: winds, gpu: gpu)
        : try execute(plans[name]!, winds: winds, gpu: gpu)
      check(false, "\(name) accepted the invalid batch \(winds)")
    } catch {
      check("\(error)".contains(reason), "\(name) rejects: \(error)")
    }
  }
}

let modes: [(name: String, rows: Int, repeats: Int)] = [("live", 1, 201), ("backfill", 1_000_000, 41)]
print("\n--- timings, milliseconds ---")
print("mode\tN\tplan\tcold\twarm_median\twarm_min\tuploads\treadbacks\tbytes_moved\twaits")
for mode in modes {
  let winds = (0..<mode.rows).map { Int32($0 % 1001) }
  let truth = try cpuOracle.evaluateCycledBatch(rows: mode.rows)
  var cold: [String: Double] = [:]
  for name in planOrder + ["handwritten"] {
    let start = DispatchTime.now().uptimeNanoseconds
    let freshMsl = try String(contentsOfFile: runnerArgs.mslPath, encoding: .utf8)
    let fresh = try Gpu(mslSource: freshMsl, specs: kernelSpecs)
    let run = name == "handwritten"
      ? try handwritten(winds: winds, gpu: fresh)
      : try execute(plans[name]!, winds: winds, gpu: fresh)
    cold[name] = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e9
    check(run.eta == truth, "\(mode.name) \(name) matches compiled Dao CPU batch (\(mode.rows) rows)")
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
    print([
      mode.name, "\(mode.rows)", name, milliseconds(cold[name]!), milliseconds(median(warm[name]!)),
      milliseconds(warm[name]!.min()!), "\(run.transfers.uploads)", "\(run.transfers.readbacks)",
      "\(run.transfers.uploadBytes + run.transfers.readbackBytes)", "\(run.waits)",
    ].joined(separator: "\t"))
  }
}

print(failures == 0 ? "\nall device checks passed" : "\n\(failures) device check(s) failed")
exit(failures == 0 ? 0 : 1)
