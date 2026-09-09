// NOLINTBEGIN(readability-identifier-length)
#ifndef DAO_BACKEND_LLVM_RUNTIME_HOOKS_H
#define DAO_BACKEND_LLVM_RUNTIME_HOOKS_H

// llvm_runtime_hooks.h — Centralized LLVM-side runtime hook declarations.
//
// This is the backend's single authoritative home for Dao runtime hook
// names and LLVM signatures. It must agree with:
//
//   - docs/contracts/CONTRACT_RUNTIME_ABI.md  (normative spec)
//   - runtime/core/dao_abi.h                  (C-side declarations)
//   - stdlib/core/*.dao                       (Dao extern declarations)
//
// Disagreement between any layer and the contract is a bug.

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <string_view>

namespace dao {

class LlvmTypeLowering;

// ---------------------------------------------------------------------------
// Hook name constants — authoritative backend-side symbol names.
// ---------------------------------------------------------------------------

namespace runtime_hooks {

// IO domain
inline constexpr std::string_view kWriteStdout  = "__dao_io_write_stdout";
inline constexpr std::string_view kWriteStderr  = "__dao_io_write_stderr";
inline constexpr std::string_view kReadFile     = "__dao_io_read_file";
inline constexpr std::string_view kWriteFile    = "__dao_io_write_file";
inline constexpr std::string_view kFileExists   = "__dao_io_file_exists";

// Equality domain
inline constexpr std::string_view kEqI8     = "__dao_eq_i8";
inline constexpr std::string_view kEqI16    = "__dao_eq_i16";
inline constexpr std::string_view kEqI32    = "__dao_eq_i32";
inline constexpr std::string_view kEqI64    = "__dao_eq_i64";
inline constexpr std::string_view kEqU8     = "__dao_eq_u8";
inline constexpr std::string_view kEqU16    = "__dao_eq_u16";
inline constexpr std::string_view kEqU32    = "__dao_eq_u32";
inline constexpr std::string_view kEqU64    = "__dao_eq_u64";
inline constexpr std::string_view kEqF32    = "__dao_eq_f32";
inline constexpr std::string_view kEqF64    = "__dao_eq_f64";
inline constexpr std::string_view kEqBool   = "__dao_eq_bool";
inline constexpr std::string_view kEqString = "__dao_eq_string";

// Conversion domain (to_string)
inline constexpr std::string_view kConvI8ToString   = "__dao_conv_i8_to_string";
inline constexpr std::string_view kConvI16ToString  = "__dao_conv_i16_to_string";
inline constexpr std::string_view kConvI32ToString  = "__dao_conv_i32_to_string";
inline constexpr std::string_view kConvI64ToString  = "__dao_conv_i64_to_string";
inline constexpr std::string_view kConvU8ToString   = "__dao_conv_u8_to_string";
inline constexpr std::string_view kConvU16ToString  = "__dao_conv_u16_to_string";
inline constexpr std::string_view kConvU32ToString  = "__dao_conv_u32_to_string";
inline constexpr std::string_view kConvU64ToString  = "__dao_conv_u64_to_string";
inline constexpr std::string_view kConvF32ToString  = "__dao_conv_f32_to_string";
inline constexpr std::string_view kConvF64ToString  = "__dao_conv_f64_to_string";
inline constexpr std::string_view kConvBoolToString = "__dao_conv_bool_to_string";

// Conversion domain (numeric type conversions)
inline constexpr std::string_view kConvI32ToF64  = "__dao_conv_i32_to_f64";
inline constexpr std::string_view kConvI32ToI64  = "__dao_conv_i32_to_i64";
inline constexpr std::string_view kConvF64ToI32  = "__dao_conv_f64_to_i32";
inline constexpr std::string_view kConvI64ToI32  = "__dao_conv_i64_to_i32";
// Float ↔ float
inline constexpr std::string_view kConvF32ToF64  = "__dao_conv_f32_to_f64";
inline constexpr std::string_view kConvF64ToF32  = "__dao_conv_f64_to_f32";
// Integer → float
inline constexpr std::string_view kConvI32ToF32  = "__dao_conv_i32_to_f32";
inline constexpr std::string_view kConvI64ToF64  = "__dao_conv_i64_to_f64";
inline constexpr std::string_view kConvI64ToF32  = "__dao_conv_i64_to_f32";
// Float → integer (trapping)
inline constexpr std::string_view kConvF64ToI64  = "__dao_conv_f64_to_i64";
inline constexpr std::string_view kConvF32ToI32  = "__dao_conv_f32_to_i32";
inline constexpr std::string_view kConvF32ToI64  = "__dao_conv_f32_to_i64";
// Integer widening
inline constexpr std::string_view kConvI8ToI32   = "__dao_conv_i8_to_i32";
inline constexpr std::string_view kConvI16ToI32  = "__dao_conv_i16_to_i32";
inline constexpr std::string_view kConvI8ToI64   = "__dao_conv_i8_to_i64";
inline constexpr std::string_view kConvI16ToI64  = "__dao_conv_i16_to_i64";
inline constexpr std::string_view kConvU8ToU32   = "__dao_conv_u8_to_u32";
inline constexpr std::string_view kConvU16ToU32  = "__dao_conv_u16_to_u32";
inline constexpr std::string_view kConvU8ToU64   = "__dao_conv_u8_to_u64";
inline constexpr std::string_view kConvU16ToU64  = "__dao_conv_u16_to_u64";
inline constexpr std::string_view kConvU32ToU64  = "__dao_conv_u32_to_u64";
inline constexpr std::string_view kConvU32ToI64  = "__dao_conv_u32_to_i64";
// Integer narrowing (trapping)
inline constexpr std::string_view kConvI32ToI8   = "__dao_conv_i32_to_i8";
inline constexpr std::string_view kConvI32ToI16  = "__dao_conv_i32_to_i16";
inline constexpr std::string_view kConvU32ToU8   = "__dao_conv_u32_to_u8";
inline constexpr std::string_view kConvU32ToU16  = "__dao_conv_u32_to_u16";
// Sign conversions (trapping)
inline constexpr std::string_view kConvI32ToU32  = "__dao_conv_i32_to_u32";
inline constexpr std::string_view kConvU32ToI32  = "__dao_conv_u32_to_i32";
inline constexpr std::string_view kConvI64ToU64  = "__dao_conv_i64_to_u64";
inline constexpr std::string_view kConvU64ToI64  = "__dao_conv_u64_to_i64";

// Overflow domain (explicit operations)
inline constexpr std::string_view kWrappingAddI8     = "__dao_wrapping_add_i8";
inline constexpr std::string_view kWrappingSubI8     = "__dao_wrapping_sub_i8";
inline constexpr std::string_view kWrappingMulI8     = "__dao_wrapping_mul_i8";
inline constexpr std::string_view kWrappingAddI16    = "__dao_wrapping_add_i16";
inline constexpr std::string_view kWrappingSubI16    = "__dao_wrapping_sub_i16";
inline constexpr std::string_view kWrappingMulI16    = "__dao_wrapping_mul_i16";
inline constexpr std::string_view kWrappingAddI32    = "__dao_wrapping_add_i32";
inline constexpr std::string_view kWrappingSubI32    = "__dao_wrapping_sub_i32";
inline constexpr std::string_view kWrappingMulI32    = "__dao_wrapping_mul_i32";
inline constexpr std::string_view kWrappingAddI64    = "__dao_wrapping_add_i64";
inline constexpr std::string_view kWrappingSubI64    = "__dao_wrapping_sub_i64";
inline constexpr std::string_view kWrappingMulI64    = "__dao_wrapping_mul_i64";
inline constexpr std::string_view kSaturatingAddI8   = "__dao_saturating_add_i8";
inline constexpr std::string_view kSaturatingSubI8   = "__dao_saturating_sub_i8";
inline constexpr std::string_view kSaturatingMulI8   = "__dao_saturating_mul_i8";
inline constexpr std::string_view kSaturatingAddI16  = "__dao_saturating_add_i16";
inline constexpr std::string_view kSaturatingSubI16  = "__dao_saturating_sub_i16";
inline constexpr std::string_view kSaturatingMulI16  = "__dao_saturating_mul_i16";
inline constexpr std::string_view kSaturatingAddI32  = "__dao_saturating_add_i32";
inline constexpr std::string_view kSaturatingSubI32  = "__dao_saturating_sub_i32";
inline constexpr std::string_view kSaturatingMulI32  = "__dao_saturating_mul_i32";
inline constexpr std::string_view kSaturatingAddI64  = "__dao_saturating_add_i64";
inline constexpr std::string_view kSaturatingSubI64  = "__dao_saturating_sub_i64";
inline constexpr std::string_view kSaturatingMulI64  = "__dao_saturating_mul_i64";

// Generator domain
inline constexpr std::string_view kGenAlloc = "__dao_gen_alloc";
inline constexpr std::string_view kGenFree  = "__dao_gen_free";

// Memory/resource domain
inline constexpr std::string_view kMemResourceEnter = "__dao_mem_resource_enter";
inline constexpr std::string_view kMemResourceExit  = "__dao_mem_resource_exit";

// Allocation domain
inline constexpr std::string_view kMemAlloc   = "__dao_mem_alloc";
inline constexpr std::string_view kMemRealloc = "__dao_mem_realloc";
inline constexpr std::string_view kMemFree    = "__dao_mem_free";
inline constexpr std::string_view kMemAllocOuter = "__dao_mem_alloc_outer";
inline constexpr std::string_view kMemAllocOwner = "__dao_mem_alloc_owner";

// Panic domain
inline constexpr std::string_view kPanic = "__dao_panic";

// String domain
inline constexpr std::string_view kStrConcat     = "__dao_str_concat";
inline constexpr std::string_view kStrLength     = "__dao_str_length";
inline constexpr std::string_view kStrCharAt     = "__dao_str_char_at";
inline constexpr std::string_view kStrSubstring  = "__dao_str_substring";
inline constexpr std::string_view kStrIndexOf    = "__dao_str_index_of";
inline constexpr std::string_view kStrStartsWith = "__dao_str_starts_with";
inline constexpr std::string_view kStrEndsWith   = "__dao_str_ends_with";
inline constexpr std::string_view kStrCompare    = "__dao_str_compare";
inline constexpr std::string_view kStrHash       = "__dao_str_hash";
inline constexpr std::string_view kStrFromBytes = "__dao_str_from_bytes";
inline constexpr std::string_view kStrCopyOuter = "__dao_str_copy_outer";

// All hook names, for iteration / validation.
inline constexpr std::string_view kAllHooks[] = {
    kWriteStdout,
    kWriteStderr,
    kReadFile,
    kWriteFile,
    kFileExists,
    kEqI8,
    kEqI16,
    kEqI32,
    kEqI64,
    kEqU8,
    kEqU16,
    kEqU32,
    kEqU64,
    kEqF32,
    kEqF64,
    kEqBool,
    kEqString,
    kConvI8ToString,
    kConvI16ToString,
    kConvI32ToString,
    kConvI64ToString,
    kConvU8ToString,
    kConvU16ToString,
    kConvU32ToString,
    kConvU64ToString,
    kConvF32ToString,
    kConvF64ToString,
    kConvBoolToString,
    kConvI32ToF64,
    kConvI32ToI64,
    kConvF64ToI32,
    kConvI64ToI32,
    kConvF32ToF64,
    kConvF64ToF32,
    kConvI32ToF32,
    kConvI64ToF64,
    kConvI64ToF32,
    kConvF64ToI64,
    kConvF32ToI32,
    kConvF32ToI64,
    kConvI8ToI32,
    kConvI16ToI32,
    kConvI8ToI64,
    kConvI16ToI64,
    kConvU8ToU32,
    kConvU16ToU32,
    kConvU8ToU64,
    kConvU16ToU64,
    kConvU32ToU64,
    kConvU32ToI64,
    kConvI32ToI8,
    kConvI32ToI16,
    kConvU32ToU8,
    kConvU32ToU16,
    kConvI32ToU32,
    kConvU32ToI32,
    kConvI64ToU64,
    kConvU64ToI64,
    kWrappingAddI8,
    kWrappingSubI8,
    kWrappingMulI8,
    kWrappingAddI16,
    kWrappingSubI16,
    kWrappingMulI16,
    kWrappingAddI32,
    kWrappingSubI32,
    kWrappingMulI32,
    kWrappingAddI64,
    kWrappingSubI64,
    kWrappingMulI64,
    kSaturatingAddI8,
    kSaturatingSubI8,
    kSaturatingMulI8,
    kSaturatingAddI16,
    kSaturatingSubI16,
    kSaturatingMulI16,
    kSaturatingAddI32,
    kSaturatingSubI32,
    kSaturatingMulI32,
    kSaturatingAddI64,
    kSaturatingSubI64,
    kSaturatingMulI64,
    kGenAlloc,
    kGenFree,
    kMemResourceEnter,
    kMemResourceExit,
    kMemAlloc,
    kMemRealloc,
    kMemFree,
    kMemAllocOuter,
    kMemAllocOwner,
    kPanic,
    kStrConcat,
    kStrLength,
    kStrCharAt,
    kStrSubstring,
    kStrIndexOf,
    kStrStartsWith,
    kStrEndsWith,
    kStrCompare,
    kStrHash,
    kStrFromBytes,
    kStrCopyOuter,
};

} // namespace runtime_hooks

// ---------------------------------------------------------------------------
// LlvmRuntimeHooks — materializes runtime hook declarations in a module.
// ---------------------------------------------------------------------------

class LlvmRuntimeHooks {
public:
  explicit LlvmRuntimeHooks(llvm::Module& module, LlvmTypeLowering& types);

  // Ensure all runtime hooks are declared in the module.
  // Idempotent — safe to call multiple times.
  void declare_all();

  // Check whether a symbol name is a known runtime hook.
  static auto is_runtime_hook(std::string_view name) -> bool;

private:
  llvm::Module& module_;
  LlvmTypeLowering& types_;

  void declare_io_hooks();
  void declare_equality_hooks();
  void declare_conversion_hooks();
  void declare_overflow_hooks();
  void declare_generator_hooks();
  void declare_mem_resource_hooks();
  void declare_string_hooks();
  void declare_alloc_hooks();
  void declare_panic_hooks();

  // Helper: get-or-create a function declaration.
  auto ensure_declared(std::string_view name, llvm::FunctionType* fn_type)
      -> llvm::Function*;
};

} // namespace dao

#endif // DAO_BACKEND_LLVM_RUNTIME_HOOKS_H
// NOLINTEND(readability-identifier-length)
