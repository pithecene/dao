// NOLINTBEGIN(readability-identifier-length)
#ifndef DAO_BACKEND_LLVM_LLVM_BACKEND_H
#define DAO_BACKEND_LLVM_LLVM_BACKEND_H

#include "backend/llvm/llvm_type_lowering.h"
#include "frontend/diagnostics/diagnostic.h"
#include "frontend/diagnostics/source.h"
#include "frontend/module/source_map.h"
#include "ir/mir/mir.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dao {

struct ModuleInfo;

// ---------------------------------------------------------------------------
// LlvmBackendResult — output of LLVM lowering.
// ---------------------------------------------------------------------------

struct LlvmBackendResult {
  std::unique_ptr<llvm::Module> module;
  std::vector<Diagnostic> diagnostics;
};

// ---------------------------------------------------------------------------
// LlvmBackend — top-level MIR → LLVM IR lowering.
//
// Consumes a MirModule and produces an llvm::Module. This is the only
// public entry point for the LLVM backend.
// ---------------------------------------------------------------------------

class LlvmBackend {
public:
  explicit LlvmBackend(llvm::LLVMContext& ctx);

  // Lower a MirModule to LLVM IR. The source map (when given) decides
  // which functions belong to the prelude group: those may have their
  // bodies dropped with a warning if they use unsupported constructs,
  // while user functions produce hard errors. Without a source map
  // every function is user code.
  // `entry` is the program's entry module: its `main` keeps the name
  // `main`; every other module-owned function is named `<module>::<name>`.
  auto lower(const MirModule& mir_module, const SourceMap* source_map = nullptr,
             const ModuleInfo* entry = nullptr) -> LlvmBackendResult;

  // Emit textual LLVM IR to a stream.
  static void print_ir(std::ostream& out, const llvm::Module& module);

  // Initialize LLVM target machinery (call once per process).
  static void initialize_targets();

  // Emit a native object file from a lowered module.
  // Returns true on success; on failure, writes a message to error_out.
  static auto emit_object(llvm::Module& module,
                          const std::string& output_path,
                          std::string& error_out) -> bool;

private:
  llvm::LLVMContext& ctx_;
  LlvmTypeLowering types_;
  std::unique_ptr<llvm::Module> module_;
  const ModuleInfo* entry_ = nullptr; // the program's entry module while lowering

  /// LLVM name of a function symbol (llvm_names.h) for this program.
  [[nodiscard]] auto fn_name(const Symbol& sym) const -> std::string;
  std::vector<Diagnostic> diagnostics_;

  void emit_diagnostic(Span span, const std::string& message);

  // Function lowering
  auto lower_function(const MirFunction& fn) -> bool;

  // Per-function state
  struct FunctionState {
    llvm::Function* llvm_fn = nullptr;
    llvm::IRBuilder<>* builder = nullptr;

    // MIR LocalId → LLVM pointer (alloca for normal fns, frame GEP for generators)
    std::unordered_map<uint32_t, llvm::Value*> locals;

    // MIR LocalId → semantic Type* (for place resolution)
    std::unordered_map<uint32_t, const Type*> local_types;

    // MIR MirValueId → LLVM Value* (for SSA temporaries)
    std::unordered_map<uint32_t, llvm::Value*> values;

    // MIR MirValueId → semantic Type* (for signedness decisions)
    std::unordered_map<uint32_t, const Type*> value_types;

    // MIR MirValueId → builtin intrinsic name (for compiler builtins)
    std::unordered_map<uint32_t, std::string_view> builtin_names;

    // MIR BlockId → LLVM BasicBlock*
    std::unordered_map<uint32_t, llvm::BasicBlock*> blocks;

    // --- Generator iteration state (consumer side) ---

    // MIR ValueId (from IterInit) → { frame_ptr, resume_fn_ptr } pair
    std::unordered_map<uint32_t, std::pair<llvm::Value*, llvm::Value*>> iter_state;

    // --- Generator function state (producer side) ---

    // Frame pointer (alloca/heap ptr) for the generator being lowered.
    llvm::Value* gen_frame_ptr = nullptr;

    // Frame struct type for the generator being lowered.
    llvm::StructType* gen_frame_type = nullptr;

    // Yield-point state index counter.
    uint32_t gen_next_state = 1;
  };

  // Top-level lowering phases (called by lower()).
  void declare_functions(const MirModule& mir_module, const SourceMap* source_map);
  void lower_bodies(const MirModule& mir_module, const SourceMap* source_map);

  // True if the function's declaration lies in a prelude-group file.
  static auto in_prelude(const MirFunction& fn, const SourceMap* source_map) -> bool;

  auto lower_block(const MirBlock& block,
                    FunctionState& state) -> bool;
  auto lower_inst(const MirInst& inst,
                   FunctionState& state) -> bool;

  // Typed instruction lowering helpers
  auto lower_const_int(const MirConstInt& p, const MirInst& inst,
                       FunctionState& state) -> bool;
  auto lower_const_float(const MirConstFloat& p, const MirInst& inst,
                         FunctionState& state) -> bool;
  auto lower_const_bool(const MirConstBool& p, const MirInst& inst,
                        FunctionState& state) -> bool;
  auto lower_const_string(const MirConstString& p, const MirInst& inst,
                          FunctionState& state) -> bool;
  auto lower_unary(const MirUnary& p, const MirInst& inst,
                   FunctionState& state) -> bool;
  auto lower_binary(const MirBinary& p, const MirInst& inst,
                    FunctionState& state) -> bool;
  auto lower_store(const MirStore& p, const MirInst& inst,
                   FunctionState& state) -> bool;
  auto lower_load(const MirLoad& p, const MirInst& inst,
                  FunctionState& state) -> bool;
  auto lower_field_access(const MirFieldAccess& p, const MirInst& inst,
                          FunctionState& state) -> bool;
  auto lower_fn_ref(const MirFnRef& p, const MirInst& inst,
                    FunctionState& state) -> bool;
  auto lower_call(const MirCall& p, const MirInst& inst,
                  FunctionState& state) -> bool;
  auto lower_construct(const MirConstruct& p, const MirInst& inst,
                       FunctionState& state) -> bool;
  auto lower_enum_construct(const MirEnumConstruct& p, const MirInst& inst,
                            FunctionState& state) -> bool;
  auto lower_enum_discriminant(const MirEnumDiscriminant& p, const MirInst& inst,
                               FunctionState& state) -> bool;
  auto lower_enum_payload(const MirEnumPayload& p, const MirInst& inst,
                          FunctionState& state) -> bool;
  auto lower_return(const MirReturn& p, const MirInst& inst,
                    FunctionState& state) -> bool;
  auto lower_br(const MirBr& p, const MirInst& inst,
                FunctionState& state) -> bool;
  auto lower_cond_br(const MirCondBr& p, const MirInst& inst,
                     FunctionState& state) -> bool;

  // Generator function lowering
  static auto is_generator_function(const MirFunction& fn) -> bool;
  auto create_generator_frame_type(const MirFunction& fn)
      -> llvm::StructType*;
  auto lower_generator_init(const MirFunction& fn) -> bool;
  auto lower_generator_resume(const MirFunction& fn) -> bool;

  // Iterator instruction lowering (consumer side)
  auto lower_iter_init(const MirIterInit& p, const MirInst& inst,
                       FunctionState& state) -> bool;
  auto lower_iter_has_next(const MirIterHasNext& p, const MirInst& inst,
                           FunctionState& state) -> bool;
  auto lower_iter_next(const MirIterNext& p, const MirInst& inst,
                       FunctionState& state) -> bool;
  auto lower_iter_destroy(const MirIterDestroy& p, const MirInst& inst,
                          FunctionState& state) -> bool;

  // Checked signed integer arithmetic — traps on overflow.
  auto emit_checked_signed_op(llvm::Intrinsic::ID intrinsic,
                               llvm::Value* lhs, llvm::Value* rhs,
                               const char* name,
                               FunctionState& state) -> llvm::Value*;

  // Compiler builtin intrinsic lowering.
  auto lower_builtin_call(std::string_view name, const MirCall& p,
                          const MirInst& inst,
                          FunctionState& state) -> bool;

  // Place resolution — walk projection chains to an LLVM pointer.
  auto resolve_place(const MirPlace& place,
                      FunctionState& state) -> llvm::Value*;

  // Value lookup
  static auto get_value(MirValueId id, FunctionState& state) -> llvm::Value*;
};

} // namespace dao

#endif // DAO_BACKEND_LLVM_LLVM_BACKEND_H
// NOLINTEND(readability-identifier-length)
