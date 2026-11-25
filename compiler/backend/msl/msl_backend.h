#ifndef DAO_BACKEND_MSL_MSL_BACKEND_H
#define DAO_BACKEND_MSL_MSL_BACKEND_H

#include "frontend/diagnostics/diagnostic.h"
#include "frontend/diagnostics/source.h"
#include "frontend/module/source_map.h"
#include "ir/mir/mir.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dao {

inline constexpr std::string_view kMslTargetId = "metal-msl-2.4";
inline constexpr std::string_view kMslKernelEffectProfile =
    "pure_scalar_elementwise_checked_i32";

struct MslSourceOrigin {
  std::string module_name;
  std::string file;
  uint32_t line = 0;
  uint32_t col = 0;
};

struct MslKernelSignature {
  std::vector<std::string> param_names;
  std::vector<std::string> param_types;
  std::string return_type;
  std::string effects;
};

struct MslKernel {
  std::string operation_name;
  std::string target;
  MslSourceOrigin origin;
  MslKernelSignature signature;
  std::string msl_source;
};

struct MslRejection {
  Span span;
  std::string function_name;
  std::string message;

  [[nodiscard]] auto to_diagnostic() const -> Diagnostic {
    return Diagnostic::error(span, message);
  }
};

struct MslBackendOutcome {
  std::string bundle_source;
  std::string bundle_sha256;
  std::vector<MslKernel> kernels;
  std::vector<MslRejection> rejections;
};

// ---------------------------------------------------------------------------
// MslBackend — narrow verified MIR -> Metal Shading Language kernel emitter.
//
// Lowers explicitly named pure scalar elementwise Dao functions (`(i32...) -> i32`)
// with acyclic control flow into checked Metal compute kernels, preserving Dao's
// default signed i32 overflow and division-trap semantics via a status buffer.
// Any unsupported type, side effect, call, pointer operation, or CFG cycle is
// rejected explicitly with source coordinates.
// ---------------------------------------------------------------------------

class MslBackend {
public:
  static auto lower_kernels(const MirModule& mir_module,
                            std::span<const std::string_view> kernel_names,
                            const SourceMap* source_map = nullptr) -> MslBackendOutcome;

  static auto render_manifest_tsv(const MslBackendOutcome& outcome) -> std::string;
};

} // namespace dao

#endif // DAO_BACKEND_MSL_MSL_BACKEND_H
