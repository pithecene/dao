#include "backend/llvm/llvm_type_lowering.h"

#include "frontend/types/type_identity.h"

#include <format>

#include "frontend/types/type.h"

#include <llvm/IR/DerivedTypes.h>

namespace dao {

LlvmTypeLowering::LlvmTypeLowering(llvm::LLVMContext& ctx)
    : ctx_(ctx) {}

auto LlvmTypeLowering::lower(const Type* type) -> llvm::Type* {
  if (type == nullptr) {
    error_ = "cannot lower null type";
    return nullptr;
  }

  switch (type->kind()) {
  case TypeKind::Builtin: {
    const auto* b = static_cast<const TypeBuiltin*>(type);
    return lower_builtin(b->builtin());
  }

  case TypeKind::Void:
    return llvm::Type::getVoidTy(ctx_);

  case TypeKind::Pointer: {
    const auto* p = static_cast<const TypePointer*>(type);
    // *void is an opaque pointer — don't try to lower void as a pointee.
    if (p->pointee()->kind() == TypeKind::Void) {
      return llvm::PointerType::getUnqual(ctx_);
    }
    auto* pointee = lower(p->pointee());
    if (pointee == nullptr) {
      return nullptr;
    }
    return llvm::PointerType::getUnqual(pointee);
  }

  case TypeKind::Function: {
    const auto* f = static_cast<const TypeFunction*>(type);
    auto* ret = lower(f->return_type());
    if (ret == nullptr) {
      return nullptr;
    }
    std::vector<llvm::Type*> params;
    params.reserve(f->param_types().size());
    for (const auto* param : f->param_types()) {
      auto* lowered = lower(param);
      if (lowered == nullptr) {
        return nullptr;
      }
      params.push_back(lowered);
    }
    return llvm::FunctionType::get(ret, params, /*isVarArg=*/false);
  }

  case TypeKind::Named: {
    const auto* n = static_cast<const TypeNamed*>(type);
    if (n->name() == "string") {
      return string_type();
    }
    error_ = "unsupported named type: " + std::string(n->name());
    return nullptr;
  }

  case TypeKind::Struct:
    return lower_struct(static_cast<const TypeStruct*>(type));

  case TypeKind::GenericParam: {
    const auto* g = static_cast<const TypeGenericParam*>(type);
    error_ = "cannot lower unresolved generic parameter: " + std::string(g->name());
    return nullptr;
  }

  case TypeKind::Enum: {
    const auto* enum_type = static_cast<const TypeEnum*>(type);
    // Check if any variant has a payload.
    bool has_payload = false;
    for (const auto& variant : enum_type->variants()) {
      if (!variant.payload_types.empty()) {
        has_payload = true;
        break;
      }
    }
    if (!has_payload) {
      // Payload-free enums lower to i32 discriminant tag.
      return llvm::Type::getInt32Ty(ctx_);
    }
    // Payload-bearing enums: { i32 tag, [N x <align_type>] payload }.
    // Use DataLayout to compute variant payload sizes and alignments.
    const auto& dl = module_->getDataLayout();
    uint64_t max_payload_size = 0;
    uint64_t max_payload_align = 4; // at least i32 tag alignment
    for (const auto& variant : enum_type->variants()) {
      if (variant.payload_types.empty()) {
        continue;
      }
      // Skip variants whose payload types contain unresolved generic
      // parameters — these are templates (e.g. Option<T>), not concrete
      // types.  Concrete instantiations (Option<i32>) will have real
      // payload types that can be lowered.
      bool has_generic_payload = false;
      for (const auto* pt : variant.payload_types) {
        if (pt != nullptr && pt->kind() == TypeKind::GenericParam) {
          has_generic_payload = true;
          break;
        }
      }
      if (has_generic_payload) {
        continue;
      }
      std::vector<llvm::Type*> field_types;
      for (const auto* pt : variant.payload_types) {
        auto* lt = lower(pt);
        if (lt == nullptr) {
          return nullptr;
        }
        field_types.push_back(lt);
      }
      auto* payload_struct =
          llvm::StructType::get(ctx_, field_types, /*isPacked=*/false);
      auto size = dl.getTypeAllocSize(payload_struct);
      auto align = dl.getABITypeAlign(payload_struct).value();
      if (size > max_payload_size) {
        max_payload_size = size;
      }
      if (align > max_payload_align) {
        max_payload_align = align;
      }
    }
    // Use an array of the maximally-aligned integer type.
    uint64_t array_count =
        (max_payload_size + max_payload_align - 1) / max_payload_align;
    if (array_count == 0) {
      array_count = 1;
    }
    llvm::Type* align_type =
        llvm::IntegerType::get(ctx_, max_payload_align * 8);
    auto* payload_array =
        llvm::ArrayType::get(align_type, array_count);
    return llvm::StructType::get(
        ctx_, {llvm::Type::getInt32Ty(ctx_), payload_array},
        /*isPacked=*/false);
  }

  case TypeKind::Generator:
    // Generator<T> is a fat pair: { ptr frame, ptr resume_fn }.
    return generator_type();
  }

  error_ = "unknown type kind";
  return nullptr;
}

auto LlvmTypeLowering::is_string_type(const Type* type) -> bool {
  if (type == nullptr) {
    return false;
  }
  if (type->kind() != TypeKind::Named) {
    return false;
  }
  const auto* n = static_cast<const TypeNamed*>(type);
  return n->name() == "string" && n->decl_id() == nullptr;
}

auto LlvmTypeLowering::is_unsigned(const Type* type) -> bool {
  if (type == nullptr || type->kind() != TypeKind::Builtin) {
    return false;
  }
  const auto* b = static_cast<const TypeBuiltin*>(type);
  switch (b->builtin()) {
  case BuiltinKind::U8:
  case BuiltinKind::U16:
  case BuiltinKind::U32:
  case BuiltinKind::U64:
    return true;
  default:
    return false;
  }
}

auto LlvmTypeLowering::string_type() -> llvm::StructType* {
  if (string_type_ == nullptr) {
    // String representation: { i8*, i64 } — pointer to bytes + length.
    // This is C-ABI-compatible per CONTRACT_BOOTSTRAP_AND_INTEROP.md.
    string_type_ = llvm::StructType::create(
        ctx_,
        {llvm::PointerType::getUnqual(llvm::Type::getInt8Ty(ctx_)),
         llvm::Type::getInt64Ty(ctx_)},
        "dao.string");
  }
  return string_type_;
}

auto LlvmTypeLowering::generator_type() -> llvm::StructType* {
  if (generator_type_ == nullptr) {
    // Generator representation: { ptr frame, ptr resume_fn }.
    // The frame pointer points to a compiler-generated struct;
    // the resume pointer is the generator's resume function.
    auto* ptr_type = llvm::PointerType::getUnqual(ctx_);
    generator_type_ = llvm::StructType::create(
        ctx_, {ptr_type, ptr_type}, "dao.generator");
  }
  return generator_type_;
}

auto LlvmTypeLowering::lower_builtin(BuiltinKind kind) -> llvm::Type* {
  switch (kind) {
  case BuiltinKind::I8:
  case BuiltinKind::U8:
    return llvm::Type::getInt8Ty(ctx_);
  case BuiltinKind::I16:
  case BuiltinKind::U16:
    return llvm::Type::getInt16Ty(ctx_);
  case BuiltinKind::I32:
  case BuiltinKind::U32:
    return llvm::Type::getInt32Ty(ctx_);
  case BuiltinKind::I64:
  case BuiltinKind::U64:
    return llvm::Type::getInt64Ty(ctx_);
  case BuiltinKind::F32:
    return llvm::Type::getFloatTy(ctx_);
  case BuiltinKind::F64:
    return llvm::Type::getDoubleTy(ctx_);
  case BuiltinKind::Bool:
    return llvm::Type::getInt1Ty(ctx_);
  }
  error_ = "unknown builtin kind";
  return nullptr;
}

auto LlvmTypeLowering::lower_struct(const TypeStruct* type) -> llvm::Type* {
  // Check cache first to break potential cycles.  Two instantiations of
  // one class are two types here, so the key is the type's meaning.
  auto identity = type_identity_key(type);
  auto it = struct_cache_.find(identity);
  if (it != struct_cache_.end()) {
    return it->second;
  }

  // Create opaque struct first (for potential self-reference).  LLVM
  // numbers a repeated name, so two instantiations stay apart in the IR.
  auto* st = llvm::StructType::create(ctx_, "dao." + std::string(type->name()));
  struct_cache_[identity] = st;

  // Lower field types.
  std::vector<llvm::Type*> field_types;
  field_types.reserve(type->fields().size());
  for (const auto& field : type->fields()) {
    auto* lowered = lower(field.type);
    if (lowered == nullptr) {
      return nullptr;
    }
    field_types.push_back(lowered);
  }

  st->setBody(field_types);

  // An instantiation laid out like one already made for this class IS
  // that one, however the two types were built; the shell just made is
  // left behind unreferenced.  A layout is named by its class and the
  // types it holds, so this is a lookup.
  std::string layout = std::format("{:x}(", reinterpret_cast<uintptr_t>(type->decl_id()));
  for (auto* field : field_types) {
    layout += std::format("{:x},", reinterpret_cast<uintptr_t>(field));
  }
  layout += ')';
  auto [made, fresh] = laid_out_.try_emplace(layout, st);
  if (!fresh) {
    struct_cache_[identity] = made->second;
    return made->second;
  }
  return st;
}

} // namespace dao
