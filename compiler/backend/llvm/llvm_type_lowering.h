#ifndef DAO_BACKEND_LLVM_LLVM_TYPE_LOWERING_H
#define DAO_BACKEND_LLVM_LLVM_TYPE_LOWERING_H

#include "frontend/types/type.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include <string>
#include <unordered_map>

namespace dao {

// ---------------------------------------------------------------------------
// LlvmTypeLowering — centralized semantic Type* → llvm::Type* mapping.
//
// All LLVM type decisions live here. Backend code must not create LLVM
// types ad hoc; it must go through this layer.
// ---------------------------------------------------------------------------

class LlvmTypeLowering {
public:
  explicit LlvmTypeLowering(llvm::LLVMContext& ctx);

  // Set the module reference (must be called before lowering enum types).
  void set_module(llvm::Module& module) { module_ = &module; }

  // Lower a Dao semantic type to an LLVM type.
  // Returns nullptr and sets error if the type cannot be lowered.
  auto lower(const Type* type) -> llvm::Type*;

  // Check whether a type is the predeclared string type.
  static auto is_string_type(const Type* type) -> bool;

  // Check whether a type is an unsigned integer builtin.
  static auto is_unsigned(const Type* type) -> bool;

  // Get the LLVM string representation type: { i8*, i64 } (ptr + length).
  auto string_type() -> llvm::StructType*;

  // Get the LLVM generator representation type: { ptr frame, ptr resume_fn }.
  auto generator_type() -> llvm::StructType*;

  // Access the last lowering error (empty if none).
  [[nodiscard]] auto error() const -> const std::string& { return error_; }

private:
  llvm::LLVMContext& ctx_;
  llvm::Module* module_ = nullptr;
  llvm::StructType* string_type_ = nullptr;
  llvm::StructType* generator_type_ = nullptr;
  std::string error_;

  // Cache for struct type lowering to break cycles.
  // Keyed by what a class MEANS, not by the declaration alone: one
  // declaration gives as many lowered structs as it has instantiations,
  // and `Box<i32>` is no `Box<string>` (see `type_identity_key`).
  std::unordered_map<std::string, llvm::StructType*> struct_cache_;

  // The struct already laid out for each (class, body), so an
  // instantiation laid out like one already made is that one: two
  // lowered types with the same fields are the same type, and a call
  // passing one where the other is expected has to find them so.  The
  // body is the key, so finding it is a lookup rather than a scan.
  std::unordered_map<std::string, llvm::StructType*> laid_out_;

  auto lower_builtin(BuiltinKind kind) -> llvm::Type*;
  auto lower_struct(const TypeStruct* type) -> llvm::Type*;
};

} // namespace dao

#endif // DAO_BACKEND_LLVM_LLVM_TYPE_LOWERING_H
