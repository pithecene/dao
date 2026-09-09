// NOLINTBEGIN(readability-identifier-length)

#include "backend/llvm/llvm_runtime_hooks.h"

#include "backend/llvm/llvm_type_lowering.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Type.h>

#include <algorithm>
#include <string>

namespace dao {

LlvmRuntimeHooks::LlvmRuntimeHooks(llvm::Module& module,
                                     LlvmTypeLowering& types)
    : module_(module), types_(types) {}

void LlvmRuntimeHooks::declare_all() {
  declare_io_hooks();
  declare_equality_hooks();
  declare_conversion_hooks();
  declare_overflow_hooks();
  declare_generator_hooks();
  declare_mem_resource_hooks();
  declare_alloc_hooks();
  declare_panic_hooks();
  declare_string_hooks();
}

auto LlvmRuntimeHooks::is_runtime_hook(std::string_view name) -> bool {
  return std::any_of(
      std::begin(runtime_hooks::kAllHooks),
      std::end(runtime_hooks::kAllHooks),
      [&](std::string_view hook) { return hook == name; });
}

// ---------------------------------------------------------------------------
// IO hooks
// ---------------------------------------------------------------------------

void LlvmRuntimeHooks::declare_io_hooks() {
  auto& ctx = module_.getContext();
  auto* str_type = types_.string_type();
  auto* str_ptr = llvm::PointerType::getUnqual(str_type);
  auto* void_ty = llvm::Type::getVoidTy(ctx);
  auto* i1 = llvm::Type::getInt1Ty(ctx);

  // __dao_io_write_stdout(msg: *dao.string): void
  ensure_declared(runtime_hooks::kWriteStdout,
                  llvm::FunctionType::get(void_ty, {str_ptr}, false));

  // __dao_io_write_stderr(msg: *dao.string): void
  ensure_declared(runtime_hooks::kWriteStderr,
                  llvm::FunctionType::get(void_ty, {str_ptr}, false));

  // __dao_io_read_file(path: *dao.string): dao.string
  ensure_declared(runtime_hooks::kReadFile,
                  llvm::FunctionType::get(str_type, {str_ptr}, false));

  // __dao_io_write_file(path: *dao.string, content: *dao.string): bool
  ensure_declared(runtime_hooks::kWriteFile,
                  llvm::FunctionType::get(i1, {str_ptr, str_ptr}, false));

  // __dao_io_file_exists(path: *dao.string): bool
  ensure_declared(runtime_hooks::kFileExists,
                  llvm::FunctionType::get(i1, {str_ptr}, false));
}

// ---------------------------------------------------------------------------
// Equality hooks
// ---------------------------------------------------------------------------

void LlvmRuntimeHooks::declare_equality_hooks() {
  auto& ctx = module_.getContext();
  auto* i1 = llvm::Type::getInt1Ty(ctx);
  auto* i8 = llvm::Type::getInt8Ty(ctx);
  auto* i16 = llvm::Type::getInt16Ty(ctx);
  auto* i32 = llvm::Type::getInt32Ty(ctx);
  auto* i64 = llvm::Type::getInt64Ty(ctx);
  auto* f32 = llvm::Type::getFloatTy(ctx);
  auto* f64 = llvm::Type::getDoubleTy(ctx);

  // Signed integers
  ensure_declared(runtime_hooks::kEqI8,
                  llvm::FunctionType::get(i1, {i8, i8}, false));
  ensure_declared(runtime_hooks::kEqI16,
                  llvm::FunctionType::get(i1, {i16, i16}, false));
  ensure_declared(runtime_hooks::kEqI32,
                  llvm::FunctionType::get(i1, {i32, i32}, false));
  ensure_declared(runtime_hooks::kEqI64,
                  llvm::FunctionType::get(i1, {i64, i64}, false));

  // Unsigned integers (same LLVM types as signed)
  ensure_declared(runtime_hooks::kEqU8,
                  llvm::FunctionType::get(i1, {i8, i8}, false));
  ensure_declared(runtime_hooks::kEqU16,
                  llvm::FunctionType::get(i1, {i16, i16}, false));
  ensure_declared(runtime_hooks::kEqU32,
                  llvm::FunctionType::get(i1, {i32, i32}, false));
  ensure_declared(runtime_hooks::kEqU64,
                  llvm::FunctionType::get(i1, {i64, i64}, false));

  // Floats
  ensure_declared(runtime_hooks::kEqF32,
                  llvm::FunctionType::get(i1, {f32, f32}, false));
  ensure_declared(runtime_hooks::kEqF64,
                  llvm::FunctionType::get(i1, {f64, f64}, false));

  // Bool
  ensure_declared(runtime_hooks::kEqBool,
                  llvm::FunctionType::get(i1, {i1, i1}, false));

  // __dao_eq_string(a: *dao.string, b: *dao.string): bool
  auto* str_ptr = llvm::PointerType::getUnqual(types_.string_type());
  ensure_declared(runtime_hooks::kEqString,
                  llvm::FunctionType::get(i1, {str_ptr, str_ptr}, false));
}

// ---------------------------------------------------------------------------
// Conversion hooks
// ---------------------------------------------------------------------------

void LlvmRuntimeHooks::declare_conversion_hooks() {
  auto& ctx = module_.getContext();
  auto* str_type = types_.string_type();
  auto* i8 = llvm::Type::getInt8Ty(ctx);
  auto* i16 = llvm::Type::getInt16Ty(ctx);
  auto* i32 = llvm::Type::getInt32Ty(ctx);
  auto* i64 = llvm::Type::getInt64Ty(ctx);
  auto* f32 = llvm::Type::getFloatTy(ctx);

  // to_string for all numeric types
  ensure_declared(runtime_hooks::kConvI8ToString,
                  llvm::FunctionType::get(str_type, {i8}, false));
  ensure_declared(runtime_hooks::kConvI16ToString,
                  llvm::FunctionType::get(str_type, {i16}, false));
  ensure_declared(runtime_hooks::kConvI32ToString,
                  llvm::FunctionType::get(str_type, {i32}, false));
  ensure_declared(runtime_hooks::kConvI64ToString,
                  llvm::FunctionType::get(str_type, {i64}, false));
  ensure_declared(runtime_hooks::kConvU8ToString,
                  llvm::FunctionType::get(str_type, {i8}, false));
  ensure_declared(runtime_hooks::kConvU16ToString,
                  llvm::FunctionType::get(str_type, {i16}, false));
  ensure_declared(runtime_hooks::kConvU32ToString,
                  llvm::FunctionType::get(str_type, {i32}, false));
  ensure_declared(runtime_hooks::kConvU64ToString,
                  llvm::FunctionType::get(str_type, {i64}, false));
  ensure_declared(runtime_hooks::kConvF32ToString,
                  llvm::FunctionType::get(str_type, {f32}, false));

  // __dao_conv_f64_to_string(x: f64): dao.string
  auto* f64 = llvm::Type::getDoubleTy(ctx);
  ensure_declared(runtime_hooks::kConvF64ToString,
                  llvm::FunctionType::get(str_type, {f64}, false));

  // __dao_conv_bool_to_string(x: bool): dao.string
  auto* i1 = llvm::Type::getInt1Ty(ctx);
  ensure_declared(runtime_hooks::kConvBoolToString,
                  llvm::FunctionType::get(str_type, {i1}, false));

  // Numeric type conversions
  // __dao_conv_i32_to_f64(x: i32): f64
  ensure_declared(runtime_hooks::kConvI32ToF64,
                  llvm::FunctionType::get(f64, {i32}, false));

  // __dao_conv_i32_to_i64(x: i32): i64
  ensure_declared(runtime_hooks::kConvI32ToI64,
                  llvm::FunctionType::get(i64, {i32}, false));

  // __dao_conv_f64_to_i32(x: f64): i32
  ensure_declared(runtime_hooks::kConvF64ToI32,
                  llvm::FunctionType::get(i32, {f64}, false));

  // __dao_conv_i64_to_i32(x: i64): i32
  ensure_declared(runtime_hooks::kConvI64ToI32,
                  llvm::FunctionType::get(i32, {i64}, false));

  // Float ↔ float
  ensure_declared(runtime_hooks::kConvF32ToF64,
                  llvm::FunctionType::get(f64, {f32}, false));
  ensure_declared(runtime_hooks::kConvF64ToF32,
                  llvm::FunctionType::get(f32, {f64}, false));

  // Integer → float
  ensure_declared(runtime_hooks::kConvI32ToF32,
                  llvm::FunctionType::get(f32, {i32}, false));
  ensure_declared(runtime_hooks::kConvI64ToF64,
                  llvm::FunctionType::get(f64, {i64}, false));
  ensure_declared(runtime_hooks::kConvI64ToF32,
                  llvm::FunctionType::get(f32, {i64}, false));

  // Float → integer (trapping)
  ensure_declared(runtime_hooks::kConvF64ToI64,
                  llvm::FunctionType::get(i64, {f64}, false));
  ensure_declared(runtime_hooks::kConvF32ToI32,
                  llvm::FunctionType::get(i32, {f32}, false));
  ensure_declared(runtime_hooks::kConvF32ToI64,
                  llvm::FunctionType::get(i64, {f32}, false));

  // Integer widening
  ensure_declared(runtime_hooks::kConvI8ToI32,
                  llvm::FunctionType::get(i32, {i8}, false));
  ensure_declared(runtime_hooks::kConvI16ToI32,
                  llvm::FunctionType::get(i32, {i16}, false));
  ensure_declared(runtime_hooks::kConvI8ToI64,
                  llvm::FunctionType::get(i64, {i8}, false));
  ensure_declared(runtime_hooks::kConvI16ToI64,
                  llvm::FunctionType::get(i64, {i16}, false));
  ensure_declared(runtime_hooks::kConvU8ToU32,
                  llvm::FunctionType::get(i32, {i8}, false));
  ensure_declared(runtime_hooks::kConvU16ToU32,
                  llvm::FunctionType::get(i32, {i16}, false));
  ensure_declared(runtime_hooks::kConvU8ToU64,
                  llvm::FunctionType::get(i64, {i8}, false));
  ensure_declared(runtime_hooks::kConvU16ToU64,
                  llvm::FunctionType::get(i64, {i16}, false));
  ensure_declared(runtime_hooks::kConvU32ToU64,
                  llvm::FunctionType::get(i64, {i32}, false));
  ensure_declared(runtime_hooks::kConvU32ToI64,
                  llvm::FunctionType::get(i64, {i32}, false));

  // Integer narrowing (trapping)
  ensure_declared(runtime_hooks::kConvI32ToI8,
                  llvm::FunctionType::get(i8, {i32}, false));
  ensure_declared(runtime_hooks::kConvI32ToI16,
                  llvm::FunctionType::get(i16, {i32}, false));
  ensure_declared(runtime_hooks::kConvU32ToU8,
                  llvm::FunctionType::get(i8, {i32}, false));
  ensure_declared(runtime_hooks::kConvU32ToU16,
                  llvm::FunctionType::get(i16, {i32}, false));

  // Sign conversions (trapping)
  ensure_declared(runtime_hooks::kConvI32ToU32,
                  llvm::FunctionType::get(i32, {i32}, false));
  ensure_declared(runtime_hooks::kConvU32ToI32,
                  llvm::FunctionType::get(i32, {i32}, false));
  ensure_declared(runtime_hooks::kConvI64ToU64,
                  llvm::FunctionType::get(i64, {i64}, false));
  ensure_declared(runtime_hooks::kConvU64ToI64,
                  llvm::FunctionType::get(i64, {i64}, false));
}

// ---------------------------------------------------------------------------
// Overflow hooks (wrapping + saturating)
// ---------------------------------------------------------------------------

void LlvmRuntimeHooks::declare_overflow_hooks() {
  auto& ctx = module_.getContext();
  auto* i8 = llvm::Type::getInt8Ty(ctx);
  auto* i16 = llvm::Type::getInt16Ty(ctx);
  auto* i32 = llvm::Type::getInt32Ty(ctx);
  auto* i64 = llvm::Type::getInt64Ty(ctx);

  // Wrapping: (T, T) -> T
  auto* i8_bin  = llvm::FunctionType::get(i8, {i8, i8}, false);
  auto* i16_bin = llvm::FunctionType::get(i16, {i16, i16}, false);
  auto* i32_bin = llvm::FunctionType::get(i32, {i32, i32}, false);
  auto* i64_bin = llvm::FunctionType::get(i64, {i64, i64}, false);

  ensure_declared(runtime_hooks::kWrappingAddI8, i8_bin);
  ensure_declared(runtime_hooks::kWrappingSubI8, i8_bin);
  ensure_declared(runtime_hooks::kWrappingMulI8, i8_bin);
  ensure_declared(runtime_hooks::kWrappingAddI16, i16_bin);
  ensure_declared(runtime_hooks::kWrappingSubI16, i16_bin);
  ensure_declared(runtime_hooks::kWrappingMulI16, i16_bin);
  ensure_declared(runtime_hooks::kWrappingAddI32, i32_bin);
  ensure_declared(runtime_hooks::kWrappingSubI32, i32_bin);
  ensure_declared(runtime_hooks::kWrappingMulI32, i32_bin);
  ensure_declared(runtime_hooks::kWrappingAddI64, i64_bin);
  ensure_declared(runtime_hooks::kWrappingSubI64, i64_bin);
  ensure_declared(runtime_hooks::kWrappingMulI64, i64_bin);

  // Saturating: (T, T) -> T
  ensure_declared(runtime_hooks::kSaturatingAddI8, i8_bin);
  ensure_declared(runtime_hooks::kSaturatingSubI8, i8_bin);
  ensure_declared(runtime_hooks::kSaturatingMulI8, i8_bin);
  ensure_declared(runtime_hooks::kSaturatingAddI16, i16_bin);
  ensure_declared(runtime_hooks::kSaturatingSubI16, i16_bin);
  ensure_declared(runtime_hooks::kSaturatingMulI16, i16_bin);
  ensure_declared(runtime_hooks::kSaturatingAddI32, i32_bin);
  ensure_declared(runtime_hooks::kSaturatingSubI32, i32_bin);
  ensure_declared(runtime_hooks::kSaturatingMulI32, i32_bin);
  ensure_declared(runtime_hooks::kSaturatingAddI64, i64_bin);
  ensure_declared(runtime_hooks::kSaturatingSubI64, i64_bin);
  ensure_declared(runtime_hooks::kSaturatingMulI64, i64_bin);
}

// ---------------------------------------------------------------------------
// Generator hooks
// ---------------------------------------------------------------------------

void LlvmRuntimeHooks::declare_generator_hooks() {
  auto& ctx = module_.getContext();
  auto* ptr = llvm::PointerType::getUnqual(ctx);
  auto* i64 = llvm::Type::getInt64Ty(ctx);
  auto* void_ty = llvm::Type::getVoidTy(ctx);

  // __dao_gen_alloc(size: i64, align: i64): ptr
  ensure_declared(runtime_hooks::kGenAlloc,
                  llvm::FunctionType::get(ptr, {i64, i64}, false));

  // __dao_gen_free(ptr): void
  ensure_declared(runtime_hooks::kGenFree,
                  llvm::FunctionType::get(void_ty, {ptr}, false));
}

// ---------------------------------------------------------------------------
// Memory/resource domain hooks
// ---------------------------------------------------------------------------

void LlvmRuntimeHooks::declare_mem_resource_hooks() {
  auto& ctx = module_.getContext();
  auto* ptr = llvm::PointerType::getUnqual(ctx);
  auto* void_ty = llvm::Type::getVoidTy(ctx);

  // __dao_mem_resource_enter(): ptr
  ensure_declared(runtime_hooks::kMemResourceEnter,
                  llvm::FunctionType::get(ptr, {}, false));

  // __dao_mem_resource_exit(domain: ptr): void
  ensure_declared(runtime_hooks::kMemResourceExit,
                  llvm::FunctionType::get(void_ty, {ptr}, false));
}

// ---------------------------------------------------------------------------
// Allocation hooks
// ---------------------------------------------------------------------------

void LlvmRuntimeHooks::declare_alloc_hooks() {
  auto& ctx = module_.getContext();
  auto* ptr = llvm::PointerType::getUnqual(ctx);
  auto* i64 = llvm::Type::getInt64Ty(ctx);
  auto* void_ty = llvm::Type::getVoidTy(ctx);

  // __dao_mem_alloc(size: i64, align: i64): ptr
  ensure_declared(runtime_hooks::kMemAlloc,
                  llvm::FunctionType::get(ptr, {i64, i64}, false));

  // __dao_mem_realloc(ptr, old_size: i64, new_size: i64, align: i64): ptr
  ensure_declared(runtime_hooks::kMemRealloc,
                  llvm::FunctionType::get(ptr, {ptr, i64, i64, i64}, false));

  // __dao_mem_free(ptr): void
  ensure_declared(runtime_hooks::kMemFree,
                  llvm::FunctionType::get(void_ty, {ptr}, false));

  // __dao_mem_alloc_outer(size: i64, align: i64): ptr
  ensure_declared(runtime_hooks::kMemAllocOuter, llvm::FunctionType::get(ptr, {i64, i64}, false));
}

// ---------------------------------------------------------------------------
// Panic hooks
// ---------------------------------------------------------------------------

void LlvmRuntimeHooks::declare_panic_hooks() {
  auto* void_ty = llvm::Type::getVoidTy(module_.getContext());
  auto* str_ptr = llvm::PointerType::getUnqual(types_.string_type());

  // __dao_panic(msg: *dao.string): void
  auto* fn = ensure_declared(runtime_hooks::kPanic,
                             llvm::FunctionType::get(void_ty, {str_ptr}, false));
  fn->addFnAttr(llvm::Attribute::NoReturn);
}

// ---------------------------------------------------------------------------
// String hooks
// ---------------------------------------------------------------------------

void LlvmRuntimeHooks::declare_string_hooks() {
  auto& ctx = module_.getContext();
  auto* str_type = types_.string_type();
  auto* str_ptr = llvm::PointerType::getUnqual(str_type);
  auto* i64 = llvm::Type::getInt64Ty(ctx);

  // __dao_str_concat(a: *dao.string, b: *dao.string): dao.string
  ensure_declared(runtime_hooks::kStrConcat,
                  llvm::FunctionType::get(str_type, {str_ptr, str_ptr}, false));

  // __dao_str_length(s: *dao.string): i64
  ensure_declared(runtime_hooks::kStrLength,
                  llvm::FunctionType::get(i64, {str_ptr}, false));

  auto* i1 = llvm::Type::getInt1Ty(ctx);
  auto* i32 = llvm::Type::getInt32Ty(ctx);

  // __dao_str_char_at(s: *dao.string, index: i64): i32
  ensure_declared(runtime_hooks::kStrCharAt,
                  llvm::FunctionType::get(i32, {str_ptr, i64}, false));

  // __dao_str_substring(s: *dao.string, start: i64, len: i64): dao.string
  ensure_declared(runtime_hooks::kStrSubstring,
                  llvm::FunctionType::get(str_type, {str_ptr, i64, i64}, false));

  // __dao_str_index_of(s: *dao.string, needle: *dao.string): i64
  ensure_declared(runtime_hooks::kStrIndexOf,
                  llvm::FunctionType::get(i64, {str_ptr, str_ptr}, false));

  // __dao_str_starts_with(s: *dao.string, prefix: *dao.string): bool
  ensure_declared(runtime_hooks::kStrStartsWith,
                  llvm::FunctionType::get(i1, {str_ptr, str_ptr}, false));

  // __dao_str_ends_with(s: *dao.string, suffix: *dao.string): bool
  ensure_declared(runtime_hooks::kStrEndsWith,
                  llvm::FunctionType::get(i1, {str_ptr, str_ptr}, false));

  // __dao_str_compare(a: *dao.string, b: *dao.string): i32
  ensure_declared(runtime_hooks::kStrCompare,
                  llvm::FunctionType::get(i32, {str_ptr, str_ptr}, false));

  // __dao_hash_string(s: *dao.string): i64
  ensure_declared(runtime_hooks::kStrHash,
                  llvm::FunctionType::get(i64, {str_ptr}, false));

  // __dao_str_from_bytes(bytes: *u8, len: i64): dao.string
  auto* bytes_ptr = llvm::PointerType::getUnqual(ctx);
  ensure_declared(runtime_hooks::kStrFromBytes,
                  llvm::FunctionType::get(str_type, {bytes_ptr, i64}, false));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

auto LlvmRuntimeHooks::ensure_declared(std::string_view name,
                                        llvm::FunctionType* fn_type)
    -> llvm::Function* {
  auto name_str = std::string(name);
  auto* existing = module_.getFunction(name_str);
  if (existing != nullptr) {
    return existing;
  }
  return llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage,
                                name_str, module_);
}

} // namespace dao

// NOLINTEND(readability-identifier-length)
