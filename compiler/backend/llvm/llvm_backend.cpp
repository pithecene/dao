// NOLINTBEGIN(readability-identifier-length)
// Short names `fn`, `p`, `it`, `id` are idiomatic in this file:
//   - `fn`  = MIR function being lowered
//   - `p`   = typed payload from std::visit
//   - `it`  = iterator / map lookup
//   - `id`  = MIR value identifier

#include "backend/llvm/llvm_backend.h"
#include "backend/llvm/llvm_names.h"
#include "frontend/module/program.h"
#include "frontend/types/type_identity.h"
#include "frontend/types/type_printer.h"

#include "backend/llvm/llvm_abi.h"
#include "backend/llvm/llvm_runtime_hooks.h"
#include "frontend/resolve/resolve.h"
#include "ir/mir/mir.h"

#include <algorithm>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <cassert>
#include <string>

namespace dao {

// ---------------------------------------------------------------------------
// LlvmBackend
// ---------------------------------------------------------------------------

LlvmBackend::LlvmBackend(llvm::LLVMContext& ctx) : ctx_(ctx), types_(ctx) {}

auto LlvmBackend::lower(const MirModule& mir_module,
                        const SourceMap* source_map,
                        const ModuleInfo* entry) -> LlvmBackendResult {
  module_ = std::make_unique<llvm::Module>("dao_module", ctx_);
  entry_ = entry;
  diagnostics_.clear();
  extern_signatures_.clear(); // per run: one program's externs do not constrain the next

  // Set the target triple and data layout early so that ABI-sensitive
  // lowering (struct coercion, alignment) sees the correct target info.
  // Without this, module_->getDataLayout() returns a default layout that
  // does not match the host platform's alignment rules.
  auto triple = llvm::sys::getDefaultTargetTriple();
  module_->setTargetTriple(triple);
  std::string target_err;
  const auto* target =
      llvm::TargetRegistry::lookupTarget(triple, target_err);
  if (target != nullptr) {
    llvm::TargetOptions opts;
    auto target_machine = std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(triple, "generic", "", opts,
                                    llvm::Reloc::PIC_));
    if (target_machine != nullptr) {
      module_->setDataLayout(target_machine->createDataLayout());
    }
  }

  // Provide the module to the type lowering layer so it can use DataLayout
  // for enum payload sizing.
  types_.set_module(*module_);
  next_lambda_id_ = 0;

  // Declare runtime hooks with canonical signatures before processing
  // MIR functions. This makes LlvmRuntimeHooks the authoritative source.
  LlvmRuntimeHooks hooks(*module_, types_);
  hooks.declare_all();

  declare_functions(mir_module, source_map);
  // A declaration error leaves a name bound to the wrong signature; a
  // body lowered against it would trip LLVM's own assertions rather
  // than reach the diagnostic already recorded.
  if (std::ranges::any_of(
          diagnostics_, [](const Diagnostic& diag) { return diag.severity == Severity::Error; })) {
    module_.reset();
    return {.module = nullptr, .diagnostics = std::move(diagnostics_)};
  }
  lower_bodies(mir_module, source_map);

  // Kill the module if any hard errors remain.
  bool has_errors = false;
  for (const auto& diag : diagnostics_) {
    if (diag.severity == Severity::Error) {
      has_errors = true;
      break;
    }
  }
  if (has_errors) {
    module_.reset();
  }

  return {.module = std::move(module_), .diagnostics = std::move(diagnostics_)};
}

// ---------------------------------------------------------------------------
// Declaration pass — forward-declare all functions.
// ---------------------------------------------------------------------------

auto LlvmBackend::in_prelude(const MirFunction& fn, const SourceMap* source_map) -> bool {
  return source_map != nullptr && source_map->is_prelude(fn.span.offset);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void LlvmBackend::declare_functions(const MirModule& mir_module,
                                     const SourceMap* source_map) {
  for (const auto* mir_fn : mir_module.functions) {
    if (mir_fn->symbol == nullptr) {
      continue;
    }

    // Runtime hooks are already declared by LlvmRuntimeHooks with
    // canonical signatures — skip re-declaration from MIR externs.
    if (mir_fn->is_extern && LlvmRuntimeHooks::is_runtime_hook(mir_fn->symbol->name)) {
      continue;
    }

    llvm::FunctionType* fn_type = nullptr;
    CAbiSignature c_sig;
    if (mir_fn->is_extern) {
      std::vector<const Type*> ext_param_types;
      for (const auto& local : mir_fn->locals) {
        if (!local.is_param) {
          break;
        }
        ext_param_types.push_back(local.type);
      }
      size_t diag_idx = diagnostics_.size();
      c_sig = build_c_abi_signature(ext_param_types, mir_fn->return_type, mir_fn->span);
      if (c_sig.fn_type == nullptr) {
        if (in_prelude(*mir_fn, source_map)) {
          for (size_t di = diag_idx; di < diagnostics_.size(); ++di) {
            diagnostics_[di].severity = Severity::Warning;
          }
        }
        continue;
      }
      fn_type = c_sig.fn_type;
    } else {
      auto* ret_type = types_.lower(mir_fn->return_type);
      if (ret_type == nullptr) {
        size_t diag_idx = diagnostics_.size();
        emit_diagnostic(mir_fn->span, "cannot lower return type: " + types_.error());
        if (in_prelude(*mir_fn, source_map)) {
          diagnostics_[diag_idx].severity = Severity::Warning;
        }
        continue;
      }
      std::vector<llvm::Type*> param_types;
      bool params_ok = true;
      for (const auto& local : mir_fn->locals) {
        if (!local.is_param) {
          break;
        }
        auto* lowered_param = types_.lower(local.type);
        if (lowered_param == nullptr) {
          emit_diagnostic(local.span, "cannot lower param type: " + types_.error());
          params_ok = false;
          break;
        }
        if (LlvmTypeLowering::is_string_type(local.type)) {
          lowered_param = llvm::PointerType::getUnqual(lowered_param);
        }
        param_types.push_back(lowered_param);
      }
      if (!params_ok) {
        continue;
      }
      fn_type = llvm::FunctionType::get(ret_type, param_types, /*isVarArg=*/false);
    }

    // An `extern fn` keeps the C symbol name exactly as written
    // (CONTRACT_C_ABI_INTEROP.md §3), so two modules declaring the same
    // one name the same symbol.  Declaring it twice would let LLVM
    // rename the second (`puts.1`) and leave later lookups by that name
    // bound to whichever came first; one declaration is emitted and a
    // disagreeing signature is a diagnostic rather than a silent pick.
    auto name = fn_name(*mir_fn->symbol);
    // Whether the name already on the module came from an extern is
    // decided BEFORE this function records itself: read afterwards, an
    // extern arriving second would see its own entry and take a Dao
    // definition for a fellow extern.
    const bool existing_is_extern = extern_signatures_.contains(name);
    if (mir_fn->is_extern) {
      // Compare the DAO types by identity, not the lowered ones and
      // not their printed form: `*i32` and `*f64` are both an opaque
      // `ptr` in LLVM, and two distinct classes named `Payload` print
      // alike, so either comparison would call incompatible
      // declarations of one C symbol identical
      // (CONTRACT_C_ABI_INTEROP.md §5).  Every declaration records its
      // signature, including the first, which is what later ones are
      // compared against.
      auto declared = extern_declaration(*mir_fn);
      auto [known, first] = extern_signatures_.try_emplace(name, declared);
      if (!first && known->second.identity != declared.identity) {
        auto describe = [](const ExternDeclaration& one) {
          return "'" + one.printed + "'" + (one.module.empty() ? "" : " (in " + one.module + ")");
        };
        emit_diagnostic(mir_fn->span,
                        "extern '" + name + "' is declared with conflicting signatures: " +
                            describe(known->second) + " and " + describe(declared));
      }
    }
    if (module_->getFunction(name) != nullptr) {
      // Already declared.  Two externs naming one C symbol coalesce
      // (their signatures were compared above); anything else -- a Dao
      // definition meeting an extern of the same name, in either order
      // -- is a collision, and lowering the body into the other's
      // declaration would emit a function whose body disagrees with
      // its type.
      const bool both_extern = mir_fn->is_extern && existing_is_extern;
      if (!both_extern) {
        emit_diagnostic(mir_fn->span,
                        "'" + name + "' is both defined in Dao and declared extern; " +
                            "a C symbol and a Dao function cannot share a name");
      }
      continue;
    }
    auto* llvm_fn =
        llvm::Function::Create(fn_type, llvm::Function::ExternalLinkage, name, module_.get());

    // Add sret / byval attributes for indirect struct returns and params.
    if (mir_fn->is_extern) {
      apply_c_abi_attrs(llvm_fn, c_sig);
    }

    // Name parameters (skip naming for ABI-coerced params).
    if (!mir_fn->is_extern) {
      size_t idx = 0;
      for (auto& arg : llvm_fn->args()) {
        if (idx < mir_fn->locals.size() && mir_fn->locals[idx].is_param) {
          if (mir_fn->locals[idx].symbol != nullptr) {
            arg.setName(std::string(mir_fn->locals[idx].symbol->name));
          }
        }
        ++idx;
      }
    }

    // For generator functions, also declare the resume function.
    if (is_generator_function(*mir_fn)) {
      auto* ptr_type = llvm::PointerType::getUnqual(ctx_);
      auto* void_type = llvm::Type::getVoidTy(ctx_);
      auto* resume_fn_type =
          llvm::FunctionType::get(void_type, {ptr_type}, /*isVarArg=*/false);
      llvm::Function::Create(resume_fn_type,
                             llvm::Function::ExternalLinkage,
                             fn_name(*mir_fn->symbol) + ".resume",
                             module_.get());
    }
  }
}

// ---------------------------------------------------------------------------
// Body pass — lower function bodies; prelude failures are non-fatal.
// ---------------------------------------------------------------------------

void LlvmBackend::lower_bodies(const MirModule& mir_module,
                                const SourceMap* source_map) {
  for (const auto* mir_fn : mir_module.functions) {
    size_t diag_before = diagnostics_.size();
    bool fn_ok = is_generator_function(*mir_fn)
        ? lower_generator_init(*mir_fn) && lower_generator_resume(*mir_fn)
        : lower_function(*mir_fn);
    if (!fn_ok) {
      bool is_prelude = in_prelude(*mir_fn, source_map);
      if (is_prelude) {
        // Prelude function body failed — remove body so it becomes a
        // declaration. The rest of the module can still compile; if
        // user code actually calls this function, linking will fail
        // with a clear undefined-reference error.
        if (mir_fn->symbol != nullptr) {
          auto* llvm_fn = module_->getFunction(fn_name(*mir_fn->symbol));
          if (llvm_fn != nullptr && !llvm_fn->empty()) {
            llvm_fn->deleteBody();
          }
        }
        // Downgrade prelude errors to warnings.
        for (size_t i = diag_before; i < diagnostics_.size(); ++i) {
          diagnostics_[i].severity = Severity::Warning;
        }
      }
      // User functions: diagnostics stay as errors (no downgrade).
    }
  }
}

void LlvmBackend::print_ir(std::ostream& out, const llvm::Module& module) {
  llvm::raw_os_ostream llvm_out(out);
  module.print(llvm_out, nullptr);
}

void LlvmBackend::print_ir(std::ostream& out, const llvm::Module& module,
                           const std::function<bool(std::string_view)>& keep_function) {
  llvm::raw_os_ostream llvm_out(out);
  for (const auto& global : module.globals()) {
    global.print(llvm_out);
    llvm_out << '\n';
  }
  if (!module.global_empty()) {
    llvm_out << '\n';
  }
  for (const auto& fn : module.functions()) {
    if (!fn.isDeclaration() && keep_function(std::string_view(fn.getName()))) {
      fn.print(llvm_out);
      llvm_out << '\n';
    }
  }
  for (const auto& fn : module.functions()) {
    if (fn.isDeclaration()) {
      fn.print(llvm_out);
    }
  }
}

void LlvmBackend::initialize_targets() {
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();
}

auto LlvmBackend::emit_object(llvm::Module& module,
                               const std::string& output_path,
                               std::string& error_out) -> bool {
  auto triple = llvm::sys::getDefaultTargetTriple();
  module.setTargetTriple(triple);

  std::string lookup_error;
  const auto* target = llvm::TargetRegistry::lookupTarget(triple, lookup_error);
  if (target == nullptr) {
    error_out = "failed to look up target: " + lookup_error;
    return false;
  }

  llvm::TargetOptions opts;
  auto target_machine = std::unique_ptr<llvm::TargetMachine>(
      target->createTargetMachine(triple, "generic", "", opts,
                                  llvm::Reloc::PIC_));
  if (target_machine == nullptr) {
    error_out = "failed to create target machine";
    return false;
  }

  module.setDataLayout(target_machine->createDataLayout());

  std::error_code err_code;
  llvm::raw_fd_ostream dest(output_path, err_code, llvm::sys::fs::OF_None);
  if (err_code) {
    error_out = "failed to open output file: " + err_code.message();
    return false;
  }

  llvm::legacy::PassManager pass;
  if (target_machine->addPassesToEmitFile(pass, dest, nullptr,
                                           llvm::CodeGenFileType::ObjectFile)) {
    error_out = "target machine cannot emit object files";
    return false;
  }

  pass.run(module);
  dest.flush();
  return true;
}

void LlvmBackend::emit_diagnostic(Span span, const std::string& message) {
  diagnostics_.push_back(
      Diagnostic{.severity = Severity::Error, .span = span, .message = message});
}

// ---------------------------------------------------------------------------
// Function lowering
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_function(const MirFunction& fn) -> bool {
  if (fn.symbol == nullptr) {
    return true; // skip anonymous functions (lambdas handled separately)
  }

  auto* llvm_fn = module_->getFunction(fn_name(*fn.symbol));
  if (llvm_fn == nullptr) {
    emit_diagnostic(fn.span, "function not declared: " + std::string(fn.symbol->name));
    return false;
  }

  // Extern declaration — no body to lower, leave as LLVM declare.
  if (fn.is_extern) {
    return true;
  }

  FunctionState state;
  state.llvm_fn = llvm_fn;

  // Create all basic blocks first (for forward branches).
  for (const auto* block : fn.blocks) {
    auto* llvm_block = llvm::BasicBlock::Create(
        ctx_, "bb" + std::to_string(block->id.id), llvm_fn);
    state.blocks[block->id.id] = llvm_block;
  }

  // Create entry block allocas for all locals.
  llvm::IRBuilder<> builder(ctx_);
  state.builder = &builder;

  auto* entry_block = state.blocks[fn.blocks[0]->id.id];
  builder.SetInsertPoint(entry_block);

  // Allocate locals. Params get their alloca here too (store from arg below).
  for (const auto& local : fn.locals) {
    // A binding has storage; `void` has none.  A generic instantiated
    // with `void` can bind one (`let v = p.get()` on `Ptr<void>`): say so
    // instead of handing LLVM an unsized allocation.
    if (local.type != nullptr && local.type->kind() == TypeKind::Void) {
      emit_diagnostic(local.span, "a binding of type 'void' has no storage");
      return false;
    }
    auto* local_type = types_.lower(local.type);
    if (local_type == nullptr) {
      emit_diagnostic(local.span, "cannot lower local type: " + types_.error());
      return false;
    }
    // String locals store the struct, not a pointer.
    auto* alloca = builder.CreateAlloca(
        local_type, nullptr,
        local.symbol != nullptr ? std::string(local.symbol->name) : "");
    state.locals.slots[local.id.id] = alloca;
    state.locals.types[local.id.id] = local.type;
  }

  // Store function parameters into their allocas.
  size_t param_idx = 0;
  for (auto& arg : llvm_fn->args()) {
    if (param_idx < fn.locals.size() && fn.locals[param_idx].is_param) {
      auto* alloca = state.locals.slots[fn.locals[param_idx].id.id];
      // For string params (passed by pointer), load the struct value.
      if (LlvmTypeLowering::is_string_type(fn.locals[param_idx].type)) {
        auto* loaded = builder.CreateLoad(types_.string_type(), &arg);
        builder.CreateStore(loaded, alloca);
      } else {
        builder.CreateStore(&arg, alloca);
      }
    }
    ++param_idx;
  }

  // Lower each block.
  for (const auto* block : fn.blocks) {
    if (!lower_block(*block, state)) {
      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Block lowering
// ---------------------------------------------------------------------------

auto LlvmBackend::entry_alloca(FunctionState& state, llvm::Type* type, const llvm::Twine& name)
    -> llvm::AllocaInst* {
  auto* entry = &state.llvm_fn->getEntryBlock();
  llvm::IRBuilder<> at_entry(entry, entry->begin());
  return at_entry.CreateAlloca(type, nullptr, name);
}

auto LlvmBackend::lower_block(const MirBlock& block,
                                FunctionState& state) -> bool {
  auto* target_block = state.blocks[block.id.id];
  state.builder->SetInsertPoint(target_block);

  for (const auto* inst : block.insts) {
    if (!lower_inst(*inst, state)) {
      return false;
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Instruction dispatch via std::visit
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto LlvmBackend::lower_inst(const MirInst& inst,
                               FunctionState& state) -> bool {
  return std::visit(
      overloaded{
          [&](const MirConstInt& p) { return lower_const_int(p, inst, state); },
          [&](const MirConstFloat& p) { return lower_const_float(p, inst, state); },
          [&](const MirConstBool& p) { return lower_const_bool(p, inst, state); },
          [&](const MirConstString& p) { return lower_const_string(p, inst, state); },
          [&](const MirUnary& p) { return lower_unary(p, inst, state); },
          [&](const MirBinary& p) { return lower_binary(p, inst, state); },
          [&](const MirStore& p) { return lower_store(p, inst, state); },
          [&](const MirLoad& p) { return lower_load(p, inst, state); },
          [&](const MirFnRef& p) { return lower_fn_ref(p, inst, state); },
          [&](const MirCall& p) { return lower_call(p, inst, state); },
          [&](const MirConstruct& p) { return lower_construct(p, inst, state); },
          [&](const MirEnumConstruct& p) { return lower_enum_construct(p, inst, state); },
          [&](const MirEnumDiscriminant& p) { return lower_enum_discriminant(p, inst, state); },
          [&](const MirEnumPayload& p) { return lower_enum_payload(p, inst, state); },
          [&](const MirReturn& p) { return lower_return(p, inst, state); },
          [&](const MirBr& p) { return lower_br(p, inst, state); },
          [&](const MirCondBr& p) { return lower_cond_br(p, inst, state); },
          [&](const MirFieldAccess& p) { return lower_field_access(p, inst, state); },

          [&](const MirPtrOp& p) { return lower_ptr_op(p, inst, state); },

          // Mode unsafe — permission semantics enforced upstream; no-op in codegen.
          [&](const MirModeEnter& p) -> bool {
            if (p.mode_kind != HirModeKind::Unsafe) {
              emit_diagnostic(inst.span,
                              "mode '" + std::string(p.region_name) +
                                  "' lowering not yet supported");
              return false;
            }
            return true;
          },
          [&](const MirModeExit& p) -> bool {
            if (p.mode_kind != HirModeKind::Unsafe) {
              // Entry already diagnosed; skip duplicate.
            }
            return true;
          },

          // Resource regions — dispatch by kind.
          [&](const MirResourceEnter& p) -> bool {
            if (p.region_kind != "memory") {
              emit_diagnostic(inst.span,
                              "resource '" + std::string(p.region_kind) +
                                  "' lowering not yet supported");
              return false;
            }
            auto* enter_fn = module_->getFunction(std::string(runtime_hooks::kMemResourceEnter));
            if (enter_fn == nullptr) {
              emit_diagnostic(inst.span, "resource_enter: runtime hook not found");
              return false;
            }
            auto* handle = state.builder->CreateCall(enter_fn, {}, "resource.domain");
            state.values[inst.result.id] = handle;
            return true;
          },
          [&](const MirResourceExit& p) -> bool {
            if (p.region_kind != "memory") {
              // Entry already diagnosed; skip duplicate.
              return true;
            }
            auto* handle = get_value(p.domain_handle, state);
            if (handle == nullptr) {
              emit_diagnostic(inst.span, "resource_exit: domain handle not found");
              return false;
            }
            auto* exit_fn = module_->getFunction(std::string(runtime_hooks::kMemResourceExit));
            if (exit_fn == nullptr) {
              emit_diagnostic(inst.span, "resource_exit: runtime hook not found");
              return false;
            }
            state.builder->CreateCall(exit_fn, {handle});
            return true;
          },

          // Unsupported constructs.
          [&](const MirIndexAccess&) -> bool {
            emit_diagnostic(inst.span, "index access lowering not yet implemented");
            return false;
          },
          [&](const MirIterInit& p) { return lower_iter_init(p, inst, state); },
          [&](const MirIterHasNext& p) { return lower_iter_has_next(p, inst, state); },
          [&](const MirIterNext& p) { return lower_iter_next(p, inst, state); },
          [&](const MirIterDestroy& p) { return lower_iter_destroy(p, inst, state); },
          [&](const MirYieldInst&) -> bool {
            // Yield is handled inline by lower_generator_resume; reaching
            // this visitor means yield appeared outside a generator context.
            emit_diagnostic(inst.span, "yield outside generator function");
            return false;
          },
          [&](const MirLambdaInst& p) { return lower_lambda(p, inst, state); },
      },
      inst.payload);
}

// ---------------------------------------------------------------------------
// Value lookup
// ---------------------------------------------------------------------------

auto LlvmBackend::get_value(MirValueId id, FunctionState& state) -> llvm::Value* {
  auto it = state.values.find(id.id);
  if (it != state.values.end()) {
    return it->second;
  }
  auto fn_it = state.callees.fn_ref_symbols.find(id.id);
  if (fn_it != state.callees.fn_ref_symbols.end()) {
    auto type_it = state.value_types.find(id.id);
    if (type_it == state.value_types.end() || type_it->second == nullptr ||
        type_it->second->kind() != TypeKind::Function) {
      return nullptr;
    }
    auto* thunk = get_or_create_fn_thunk(
        *fn_it->second, static_cast<const TypeFunction*>(type_it->second), Span{});
    if (thunk == nullptr) {
      return nullptr;
    }
    auto* null_env = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));
    auto* callable = llvm::ConstantStruct::get(types_.callable_type(), {thunk, null_env});
    state.values[id.id] = callable;
    return callable;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Checked signed integer arithmetic
// ---------------------------------------------------------------------------

auto LlvmBackend::emit_checked_signed_op(llvm::Intrinsic::ID intrinsic,
                                           llvm::Value* lhs, llvm::Value* rhs,
                                           const char* name,
                                           FunctionState& state)
    -> llvm::Value* {
  auto* fn = llvm::Intrinsic::getDeclaration(module_.get(), intrinsic,
                                              {lhs->getType()});
  auto* pair = state.builder->CreateCall(fn, {lhs, rhs});

  auto* result = state.builder->CreateExtractValue(pair, 0, name);
  auto* overflow = state.builder->CreateExtractValue(pair, 1, "overflow");

  auto* parent_fn = state.builder->GetInsertBlock()->getParent();
  auto* trap_bb = llvm::BasicBlock::Create(ctx_, "overflow.trap", parent_fn);
  auto* cont_bb = llvm::BasicBlock::Create(ctx_, "overflow.cont", parent_fn);

  state.builder->CreateCondBr(overflow, trap_bb, cont_bb);

  state.builder->SetInsertPoint(trap_bb);
  auto* trap_fn = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::trap);
  state.builder->CreateCall(trap_fn);
  state.builder->CreateUnreachable();

  state.builder->SetInsertPoint(cont_bb);
  return result;
}

// ---------------------------------------------------------------------------
// Constant lowering
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_const_int(const MirConstInt& p, const MirInst& inst,
                                    FunctionState& state) -> bool {
  auto* type = types_.lower(inst.type);
  if (type == nullptr) {
    emit_diagnostic(inst.span, "cannot lower const int type: " + types_.error());
    return false;
  }
  // Enum discriminant constants: the semantic type is the enum type, but
  // the LLVM value is the i32 tag. Use i32 for the constant.
  if (inst.type != nullptr && inst.type->kind() == TypeKind::Enum) {
    type = llvm::Type::getInt32Ty(ctx_);
  }
  auto* val = llvm::ConstantInt::get(type, static_cast<uint64_t>(p.value),
                                     /*isSigned=*/!LlvmTypeLowering::is_unsigned(inst.type));
  state.values[inst.result.id] = val;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

auto LlvmBackend::lower_const_float(const MirConstFloat& p, const MirInst& inst,
                                      FunctionState& state) -> bool {
  auto* type = types_.lower(inst.type);
  if (type == nullptr) {
    emit_diagnostic(inst.span, "cannot lower const float type: " + types_.error());
    return false;
  }
  auto* val = llvm::ConstantFP::get(type, p.value);
  state.values[inst.result.id] = val;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

auto LlvmBackend::lower_const_bool(const MirConstBool& p, const MirInst& inst,
                                     FunctionState& state) -> bool {
  auto* val = llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx_),
                                     p.value ? 1 : 0);
  state.values[inst.result.id] = val;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

auto LlvmBackend::lower_const_string(const MirConstString& p, const MirInst& inst,
                                       FunctionState& state) -> bool {
  // Create a global constant for the string data.
  // Strip surrounding quotes — the lexer preserves them in the token text.
  auto raw = p.value;
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
    raw = raw.substr(1, raw.size() - 2);
  }
  // Process escape sequences: \n, \t, \\, \", \r, \0.
  std::string str;
  str.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      switch (raw[i + 1]) {
        case 'n':  str += '\n'; ++i; break;
        case 't':  str += '\t'; ++i; break;
        case 'r':  str += '\r'; ++i; break;
        case '\\': str += '\\'; ++i; break;
        case '"':  str += '"';  ++i; break;
        case '0':  str += '\0'; ++i; break;
        default:   str += raw[i]; break;
      }
    } else {
      str += raw[i];
    }
  }
  auto* str_constant = llvm::ConstantDataArray::getString(ctx_, str, /*AddNull=*/true);
  auto* global = new llvm::GlobalVariable(
      *module_, str_constant->getType(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, str_constant, ".str");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  // Build a dao.string struct value: { i8* ptr, i64 len }.
  auto* str_type = types_.string_type();
  auto* zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0);
  auto* gep = state.builder->CreateInBoundsGEP(
      str_constant->getType(), global, {zero, zero}, "str.ptr");
  auto* len = llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), str.size());

  // Build the struct aggregate.
  llvm::Value* agg = llvm::UndefValue::get(str_type);
  agg = state.builder->CreateInsertValue(agg, gep, 0, "str.with_ptr");
  agg = state.builder->CreateInsertValue(agg, len, 1, "str.with_len");

  state.values[inst.result.id] = agg;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

// ---------------------------------------------------------------------------
// Unary / binary operations
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_unary(const MirUnary& p, const MirInst& inst,
                                FunctionState& state) -> bool {
  auto* operand = get_value(p.operand, state);
  if (operand == nullptr) {
    emit_diagnostic(inst.span, "unary operand not found");
    return false;
  }

  llvm::Value* result = nullptr;
  switch (p.op) {
  case UnaryOp::Negate:
    if (operand->getType()->isFloatingPointTy()) {
      result = state.builder->CreateFNeg(operand, "fneg");
    } else {
      result = state.builder->CreateNeg(operand, "neg");
    }
    break;
  case UnaryOp::Not:
    result = state.builder->CreateNot(operand, "not");
    break;
  }

  if (result != nullptr) {
    state.values[inst.result.id] = result;
    state.value_types[inst.result.id] = inst.type;
  }
  return result != nullptr;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto LlvmBackend::lower_binary(const MirBinary& p, const MirInst& inst,
                                 FunctionState& state) -> bool {
  auto* lhs = get_value(p.lhs, state);
  auto* rhs = get_value(p.rhs, state);
  if (lhs == nullptr || rhs == nullptr) {
    emit_diagnostic(inst.span, "binary operand not found");
    return false;
  }

  bool is_float = lhs->getType()->isFloatingPointTy();
  auto lhs_type_it = state.value_types.find(p.lhs.id);
  const Type* operand_type = lhs_type_it != state.value_types.end() ? lhs_type_it->second : inst.type;
  bool is_unsigned_op = LlvmTypeLowering::is_unsigned(operand_type);
  bool is_string_op = LlvmTypeLowering::is_string_type(operand_type);
  llvm::Value* result = nullptr;

  // String operators: dispatch to runtime hooks.
  if (is_string_op) {
    auto* str_type = types_.string_type();
    auto* str_ptr = llvm::PointerType::getUnqual(str_type);

    if (p.op == BinaryOp::Add) {
      // String concatenation: call __dao_str_concat(ptr, ptr) → dao.string
      auto* concat_fn = module_->getFunction(
          std::string(runtime_hooks::kStrConcat));
      if (concat_fn == nullptr) {
        emit_diagnostic(inst.span, "string concat hook not declared");
        return false;
      }
      auto* lhs_tmp = entry_alloca(state, str_type, "str.lhs");
      auto* rhs_tmp = entry_alloca(state, str_type, "str.rhs");
      state.builder->CreateStore(lhs, lhs_tmp);
      state.builder->CreateStore(rhs, rhs_tmp);
      result = state.builder->CreateCall(
          concat_fn->getFunctionType(), concat_fn, {lhs_tmp, rhs_tmp}, "concat");
    } else if (p.op == BinaryOp::EqEq || p.op == BinaryOp::BangEq) {
      // String equality: call __dao_eq_string(ptr, ptr) → i1
      auto* eq_fn = module_->getFunction(
          std::string(runtime_hooks::kEqString));
      if (eq_fn == nullptr) {
        emit_diagnostic(inst.span, "string equality hook not declared");
        return false;
      }
      auto* lhs_tmp = entry_alloca(state, str_type, "str.lhs");
      auto* rhs_tmp = entry_alloca(state, str_type, "str.rhs");
      state.builder->CreateStore(lhs, lhs_tmp);
      state.builder->CreateStore(rhs, rhs_tmp);
      result = state.builder->CreateCall(
          eq_fn->getFunctionType(), eq_fn, {lhs_tmp, rhs_tmp}, "str.eq");
      if (p.op == BinaryOp::BangEq) {
        result = state.builder->CreateNot(result, "str.ne");
      }
    } else {
      emit_diagnostic(inst.span,
                      "unsupported operator on string type");
      return false;
    }

    if (result != nullptr) {
      state.values[inst.result.id] = result;
      state.value_types[inst.result.id] = inst.type;
    }
    return result != nullptr;
  }

  // Fat-callable (`%dao.fn = { ptr, ptr }`) equality / inequality: compare
  // both the code pointer and the environment pointer.
  if ((operand_type != nullptr && operand_type->kind() == TypeKind::Function) ||
      lhs->getType() == types_.callable_type()) {
    if (p.op == BinaryOp::EqEq || p.op == BinaryOp::BangEq) {
      auto* lhs_code = state.builder->CreateExtractValue(lhs, 0, "fn.lhs.code");
      auto* lhs_env = state.builder->CreateExtractValue(lhs, 1, "fn.lhs.env");
      auto* rhs_code = state.builder->CreateExtractValue(rhs, 0, "fn.rhs.code");
      auto* rhs_env = state.builder->CreateExtractValue(rhs, 1, "fn.rhs.env");
      if (p.op == BinaryOp::EqEq) {
        auto* code_eq = state.builder->CreateICmpEQ(lhs_code, rhs_code, "fn.code.eq");
        auto* env_eq = state.builder->CreateICmpEQ(lhs_env, rhs_env, "fn.env.eq");
        result = state.builder->CreateAnd(code_eq, env_eq, "fn.eq");
      } else {
        auto* code_ne = state.builder->CreateICmpNE(lhs_code, rhs_code, "fn.code.ne");
        auto* env_ne = state.builder->CreateICmpNE(lhs_env, rhs_env, "fn.env.ne");
        result = state.builder->CreateOr(code_ne, env_ne, "fn.ne");
      }
      state.values[inst.result.id] = result;
      state.value_types[inst.result.id] = inst.type;
      return true;
    }
    emit_diagnostic(inst.span, "unsupported operator on function type");
    return false;
  }

  switch (p.op) {
  case BinaryOp::Add:
    if (is_float) {
      result = state.builder->CreateFAdd(lhs, rhs, "fadd");
    } else if (is_unsigned_op) {
      result = state.builder->CreateAdd(lhs, rhs, "add");
    } else {
      result = emit_checked_signed_op(
          llvm::Intrinsic::sadd_with_overflow, lhs, rhs, "add", state);
    }
    break;
  case BinaryOp::Sub:
    if (is_float) {
      result = state.builder->CreateFSub(lhs, rhs, "fsub");
    } else if (is_unsigned_op) {
      result = state.builder->CreateSub(lhs, rhs, "sub");
    } else {
      result = emit_checked_signed_op(
          llvm::Intrinsic::ssub_with_overflow, lhs, rhs, "sub", state);
    }
    break;
  case BinaryOp::Mul:
    if (is_float) {
      result = state.builder->CreateFMul(lhs, rhs, "fmul");
    } else if (is_unsigned_op) {
      result = state.builder->CreateMul(lhs, rhs, "mul");
    } else {
      result = emit_checked_signed_op(
          llvm::Intrinsic::smul_with_overflow, lhs, rhs, "mul", state);
    }
    break;
  case BinaryOp::Div:
    if (is_float) {
      result = state.builder->CreateFDiv(lhs, rhs, "fdiv");
    } else if (is_unsigned_op) {
      result = state.builder->CreateUDiv(lhs, rhs, "udiv");
    } else {
      result = state.builder->CreateSDiv(lhs, rhs, "sdiv");
    }
    break;
  case BinaryOp::Mod:
    if (is_float) {
      result = state.builder->CreateFRem(lhs, rhs, "fmod");
    } else if (is_unsigned_op) {
      result = state.builder->CreateURem(lhs, rhs, "urem");
    } else {
      result = state.builder->CreateSRem(lhs, rhs, "srem");
    }
    break;

  // Comparisons
  case BinaryOp::EqEq:
    result = is_float ? state.builder->CreateFCmpOEQ(lhs, rhs, "feq")
                      : state.builder->CreateICmpEQ(lhs, rhs, "eq");
    break;
  case BinaryOp::BangEq:
    // UNE (unordered or not equal): true if either operand is NaN or
    // if the operands are not equal. ONE would incorrectly return false
    // when either operand is NaN, violating IEEE 754 != semantics.
    result = is_float ? state.builder->CreateFCmpUNE(lhs, rhs, "fne")
                      : state.builder->CreateICmpNE(lhs, rhs, "ne");
    break;
  case BinaryOp::Lt:
    if (is_float) {
      result = state.builder->CreateFCmpOLT(lhs, rhs, "flt");
    } else if (is_unsigned_op) {
      result = state.builder->CreateICmpULT(lhs, rhs, "ult");
    } else {
      result = state.builder->CreateICmpSLT(lhs, rhs, "slt");
    }
    break;
  case BinaryOp::LtEq:
    if (is_float) {
      result = state.builder->CreateFCmpOLE(lhs, rhs, "fle");
    } else if (is_unsigned_op) {
      result = state.builder->CreateICmpULE(lhs, rhs, "ule");
    } else {
      result = state.builder->CreateICmpSLE(lhs, rhs, "sle");
    }
    break;
  case BinaryOp::Gt:
    if (is_float) {
      result = state.builder->CreateFCmpOGT(lhs, rhs, "fgt");
    } else if (is_unsigned_op) {
      result = state.builder->CreateICmpUGT(lhs, rhs, "ugt");
    } else {
      result = state.builder->CreateICmpSGT(lhs, rhs, "sgt");
    }
    break;
  case BinaryOp::GtEq:
    if (is_float) {
      result = state.builder->CreateFCmpOGE(lhs, rhs, "fge");
    } else if (is_unsigned_op) {
      result = state.builder->CreateICmpUGE(lhs, rhs, "uge");
    } else {
      result = state.builder->CreateICmpSGE(lhs, rhs, "sge");
    }
    break;

  // Boolean logic
  case BinaryOp::And:
    result = state.builder->CreateAnd(lhs, rhs, "and");
    break;
  case BinaryOp::Or:
    result = state.builder->CreateOr(lhs, rhs, "or");
    break;
  }

  if (result != nullptr) {
    state.values[inst.result.id] = result;
    state.value_types[inst.result.id] = inst.type;
  }
  return result != nullptr;
}

// ---------------------------------------------------------------------------
// Place resolution — walk projection chains to an LLVM pointer.
// ---------------------------------------------------------------------------
// Ptr<T> operations (ADR_RAW_POINTER_SURFACE.md).  Reads and writes are
// load / store through a Deref projection; these are the rest, in the
// backend's own vocabulary: the null constant, getelementptr over the
// pointee, the identity on opaque pointers, and a compare with null.
// ---------------------------------------------------------------------------

// The LLVM type a pointee is stored as, where a pointer can address
// one: a function value is stored as a `ptr`; `void` has no storage, so
// null.  The checker rejects `get`, `set`, and `offset` on `Ptr<void>`;
// a generic instantiated with `void` reaches here and is diagnosed.
auto LlvmBackend::pointee_storage_type(const Type* pointee) -> llvm::Type* {
  if (pointee == nullptr || pointee->kind() == TypeKind::Void) {
    return nullptr;
  }
  return types_.lower(pointee);
}

auto LlvmBackend::lower_ptr_op(const MirPtrOp& p, const MirInst& inst, FunctionState& state)
    -> bool {
  auto* builder = state.builder;
  auto* null_ptr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(module_->getContext()));
  llvm::Value* result = nullptr;
  switch (p.op) {
  case PtrOp::New:
    result = null_ptr;
    break;
  case PtrOp::Offset: {
    if (inst.type == nullptr || inst.type->kind() != TypeKind::Pointer) {
      emit_diagnostic(inst.span, "Ptr.offset: result is no pointer type");
      return false;
    }
    auto* pointer = get_value(p.pointer, state);
    auto* elements = get_value(p.argument, state);
    if (pointer == nullptr || elements == nullptr) {
      emit_diagnostic(inst.span, "Ptr.offset: operand not found");
      return false;
    }
    auto* element_type =
        pointee_storage_type(static_cast<const TypePointer*>(inst.type)->pointee());
    if (element_type == nullptr) {
      emit_diagnostic(inst.span, "Ptr.offset is invalid on 'Ptr<void>', which has no element size");
      return false;
    }
    result = builder->CreateGEP(element_type, pointer, {elements}, "ptr.offset");
    break;
  }
  case PtrOp::Cast:
    // Opaque pointers carry no pointee: the cast is the value itself.
    result = get_value(p.pointer, state);
    break;
  case PtrOp::IsNull: {
    auto* pointer = get_value(p.pointer, state);
    if (pointer == nullptr) {
      emit_diagnostic(inst.span, "Ptr.is_null: operand not found");
      return false;
    }
    result = builder->CreateICmpEQ(pointer, null_ptr, "ptr.is_null");
    break;
  }
  case PtrOp::Get:
  case PtrOp::Set:
    emit_diagnostic(inst.span, "Ptr.get / Ptr.set reach the backend as load / store");
    return false;
  }
  if (result == nullptr) {
    emit_diagnostic(inst.span, "Ptr operation operand not found");
    return false;
  }
  state.values[inst.result.id] = result;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

// ---------------------------------------------------------------------------
// Compiler builtin intrinsic lowering
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto LlvmBackend::lower_builtin_call(
    std::string_view name, const MirCall& p, const MirInst& inst,
    FunctionState& state) -> bool {
  auto* builder = state.builder;
  auto& ctx = module_->getContext();
  const auto& layout = module_->getDataLayout();

  // inst.type on a MirCall is the result type of the call, not a
  // TypeFunction wrapper. Use it directly as the semantic result type.
  const Type* result_type = inst.type;

  // size_of$T(): i64 — return sizeof(T) as constant.
  if (name == "size_of" || name.starts_with("size_of$")) {
    if (p.explicit_type_args != nullptr && !p.explicit_type_args->empty()) {
      auto* elem_type = types_.lower((*p.explicit_type_args)[0]);
      uint64_t size = layout.getTypeAllocSize(elem_type);
      state.values[inst.result.id] =
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), size);
      state.value_types[inst.result.id] = result_type;
      return true;
    }
    emit_diagnostic(inst.span, "size_of: missing type argument");
    return false;
  }

  // align_of$T(): i64 — return alignof(T) as constant.
  if (name == "align_of" || name.starts_with("align_of$")) {
    if (p.explicit_type_args != nullptr && !p.explicit_type_args->empty()) {
      auto* elem_type = types_.lower((*p.explicit_type_args)[0]);
      uint64_t align = layout.getABITypeAlign(elem_type).value();
      state.values[inst.result.id] =
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx), align);
      state.value_types[inst.result.id] = result_type;
      return true;
    }
    emit_diagnostic(inst.span, "align_of: missing type argument");
    return false;
  }

  emit_diagnostic(inst.span,
                  "unknown builtin intrinsic: " + std::string(name));
  return false;
}

// ---------------------------------------------------------------------------

// The semantic type a place denotes: its local's type followed through
// its projections, or null where a step is unknown.
auto LlvmBackend::place_type(const MirPlace& place, const FunctionState& state) -> const Type* {
  auto local_type = state.locals.types.find(place.local.id);
  const Type* current = local_type != state.locals.types.end() ? local_type->second : nullptr;
  for (const auto& proj : place.projections) {
    if (current == nullptr) {
      return nullptr;
    }
    switch (proj.kind) {
    case MirProjectionKind::Deref:
      current = current->kind() == TypeKind::Pointer
                    ? static_cast<const TypePointer*>(current)->pointee()
                    : nullptr;
      break;
    case MirProjectionKind::Field: {
      const auto* fields = current->kind() == TypeKind::Struct ? static_cast<const TypeStruct*>(current) : nullptr;
      current = fields != nullptr && proj.field_index < fields->fields().size()
                    ? fields->fields()[proj.field_index].type
                    : nullptr;
      break;
    }
    case MirProjectionKind::Index:
      return nullptr;
    }
  }
  return current;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto LlvmBackend::resolve_place(const MirPlace& place,
                                 FunctionState& state) -> llvm::Value* {
  auto it = state.locals.slots.find(place.local.id);
  if (it == state.locals.slots.end()) {
    return nullptr;
  }

  llvm::Value* ptr = it->second;

  // Track the semantic type of what ptr points to, so that after a Deref
  // produces a raw loaded pointer we still know the LLVM type for the
  // subsequent Field GEP.
  const Type* current_type = nullptr;
  auto type_it = state.locals.types.find(place.local.id);
  if (type_it != state.locals.types.end()) {
    current_type = type_it->second;
  }

  for (const auto& proj : place.projections) {
    switch (proj.kind) {
    case MirProjectionKind::Deref: {
      // ptr holds a pointer value — load it.
      auto* load_type = types_.lower(current_type);
      if (load_type == nullptr) {
        return nullptr;
      }
      ptr = state.builder->CreateLoad(load_type, ptr, "deref.ptr");
      // Advance semantic type through the pointer.
      if (current_type != nullptr && current_type->kind() == TypeKind::Pointer) {
        current_type = static_cast<const TypePointer*>(current_type)->pointee();
      } else {
        current_type = nullptr;
      }
      break;
    }
    case MirProjectionKind::Field: {
      // ptr points to a struct — GEP into the field.
      auto* struct_type = (current_type != nullptr) ? types_.lower(current_type)
                                                     : nullptr;
      if (struct_type == nullptr || !struct_type->isStructTy()) {
        return nullptr;
      }
      auto* zero = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0);
      auto* idx = llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_),
                                          proj.field_index);
      ptr = state.builder->CreateInBoundsGEP(
          struct_type, ptr, {zero, idx},
          "field." + std::string(proj.field_name));
      // Advance semantic type to the field's type.
      if (current_type != nullptr && current_type->kind() == TypeKind::Struct) {
        const auto* sty = static_cast<const TypeStruct*>(current_type);
        if (proj.field_index < sty->fields().size()) {
          current_type = sty->fields()[proj.field_index].type;
        } else {
          current_type = nullptr;
        }
      } else {
        current_type = nullptr;
      }
      break;
    }
    case MirProjectionKind::Index:
      // Not yet supported.
      return nullptr;
    }
  }

  return ptr;
}

// ---------------------------------------------------------------------------
// Memory operations
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_store(const MirStore& p, const MirInst& inst,
                                FunctionState& state) -> bool {
  if (p.place == nullptr) {
    emit_diagnostic(inst.span, "store with null place");
    return false;
  }

  auto* value = get_value(p.value, state);
  if (value == nullptr) {
    emit_diagnostic(inst.span, "store value not found");
    return false;
  }

  auto it = state.locals.slots.find(p.place->local.id);
  if (it == state.locals.slots.end()) {
    emit_diagnostic(inst.span, "store target local not found");
    return false;
  }

  if (!p.place->projections.empty()) {
    if (const auto* target = place_type(*p.place, state);
        target != nullptr && target->kind() == TypeKind::Void) {
      emit_diagnostic(inst.span, "Ptr.set is invalid on 'Ptr<void>', which has no value");
      return false;
    }
    auto* ptr = resolve_place(*p.place, state);
    if (ptr == nullptr) {
      emit_diagnostic(inst.span, "cannot resolve projected store target");
      return false;
    }
    state.builder->CreateStore(value, ptr);
    return true;
  }

  state.builder->CreateStore(value, it->second);
  return true;
}

auto LlvmBackend::lower_load(const MirLoad& p, const MirInst& inst,
                               FunctionState& state) -> bool {
  if (p.place == nullptr) {
    emit_diagnostic(inst.span, "load with null place");
    return false;
  }

  auto it = state.locals.slots.find(p.place->local.id);
  if (it == state.locals.slots.end()) {
    emit_diagnostic(inst.span, "load source local not found");
    return false;
  }

  if (!p.place->projections.empty()) {
    auto* ptr = resolve_place(*p.place, state);
    if (ptr == nullptr) {
      emit_diagnostic(inst.span, "cannot resolve projected load source");
      return false;
    }
    auto* load_type = pointee_storage_type(inst.type);
    if (load_type == nullptr) {
      emit_diagnostic(inst.span, inst.type != nullptr && inst.type->kind() == TypeKind::Void
                                     ? "Ptr.get is invalid on 'Ptr<void>', which has no value"
                                     : "cannot lower projected load type: " + types_.error());
      return false;
    }
    auto* val = state.builder->CreateLoad(load_type, ptr, "proj.load");
    state.values[inst.result.id] = val;
    state.value_types[inst.result.id] = inst.type;
    return true;
  }

  auto* local_type = types_.lower(inst.type);
  if (local_type == nullptr) {
    emit_diagnostic(inst.span, "cannot lower load type: " + types_.error());
    return false;
  }

  auto* val = state.builder->CreateLoad(local_type, it->second, "load");
  state.values[inst.result.id] = val;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

// ---------------------------------------------------------------------------
// Field access — extractvalue on struct SSA values
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_field_access(const MirFieldAccess& p,
                                      const MirInst& inst,
                                      FunctionState& state) -> bool {
  auto* obj = get_value(p.object, state);
  if (obj == nullptr) {
    emit_diagnostic(inst.span, "field access: object value not found");
    return false;
  }

  if (!obj->getType()->isStructTy()) {
    emit_diagnostic(inst.span, "field access: object is not a struct type");
    return false;
  }

  auto* val = state.builder->CreateExtractValue(
      obj, p.field_index,
      "field." + std::string(p.field));
  state.values[inst.result.id] = val;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

// ---------------------------------------------------------------------------
// Function reference and calls
// ---------------------------------------------------------------------------

/// A MIR function's signature as the source wrote it: parameter types
/// then the return type, printed from the Dao types rather than their
/// lowering, so distinctions LLVM erases (every pointer is `ptr`)
/// survive the comparison.
auto LlvmBackend::extern_declaration(const MirFunction& fn) -> ExternDeclaration {
  ExternDeclaration declared;
  auto add = [&declared](const Type* type, bool last) {
    declared.identity.push_back(type_identity_key(type));
    declared.printed += print_type(type);
    declared.printed += last ? "" : ",";
  };
  for (const auto& local : fn.locals) {
    if (!local.is_param) {
      break; // parameters come first
    }
    add(local.type, /*last=*/false);
  }
  declared.printed += "->";
  add(fn.return_type, /*last=*/true);
  declared.module =
      fn.symbol != nullptr && fn.symbol->module != nullptr ? fn.symbol->module->display : "";
  return declared;
}

auto LlvmBackend::fn_name(const Symbol& sym) const -> std::string {
  return llvm_function_name(sym, entry_);
}

auto LlvmBackend::lower_fn_ref(const MirFnRef& p, const MirInst& inst,
                                 FunctionState& state) -> bool {
  if (p.symbol == nullptr) {
    emit_diagnostic(inst.span, "FnRef with null symbol");
    return false;
  }

  // Compiler builtin intrinsics don't have LLVM function declarations.
  // Store a nullptr marker; lower_call will generate inline IR.
  if (is_builtin_intrinsic(*p.symbol)) {
    state.values[inst.result.id] = nullptr;
    state.value_types[inst.result.id] = inst.type;
    state.callees.builtin_names[inst.result.id] = p.symbol->name;
    return true;
  }

  auto* fn = module_->getFunction(fn_name(*p.symbol));
  if (fn == nullptr) {
    emit_diagnostic(inst.span,
                    "function not found: " + std::string(p.symbol->name));
    return false;
  }

  state.callees.fn_ref_symbols[inst.result.id] = p.symbol;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

auto LlvmBackend::build_c_abi_signature(std::span<const Type* const> param_sem_types,
                                        const Type* ret_sem_type,
                                        Span span) -> CAbiSignature {
  CAbiSignature sig;
  llvm::Type* ret_type = nullptr;
  if (ret_sem_type != nullptr && ret_sem_type->kind() == TypeKind::Function) {
    ret_type = llvm::PointerType::getUnqual(ctx_);
  } else {
    ret_type = types_.lower(ret_sem_type);
    if (ret_type == nullptr) {
      emit_diagnostic(span, "cannot lower return type: " + types_.error());
      return {};
    }
  }

  std::vector<llvm::Type*> param_types;
  auto* lowered_ret = ret_type;
  if (llvm::isa<llvm::StructType>(ret_type) && !LlvmTypeLowering::is_string_type(ret_sem_type)) {
    auto* struct_ty = llvm::cast<llvm::StructType>(ret_type);
    auto coercion = classify_struct_for_c_abi(struct_ty, module_->getDataLayout(), ctx_);
    if (coercion.indirect) {
      sig.sret_type = struct_ty;
      param_types.push_back(llvm::PointerType::getUnqual(ret_type));
      sig.byval_param_types.push_back(nullptr);
      lowered_ret = llvm::Type::getVoidTy(ctx_);
    } else if (coercion.coerced_types.size() == 1) {
      lowered_ret = coercion.coerced_types[0];
    } else {
      lowered_ret = llvm::StructType::get(ctx_, coercion.coerced_types);
    }
  }

  for (const auto* param_sem : param_sem_types) {
    if (param_sem != nullptr && param_sem->kind() == TypeKind::Function) {
      param_types.push_back(llvm::PointerType::getUnqual(ctx_));
      sig.byval_param_types.push_back(nullptr);
      continue;
    }
    auto* lowered_param = types_.lower(param_sem);
    if (lowered_param == nullptr) {
      emit_diagnostic(span, "cannot lower param type: " + types_.error());
      return {};
    }
    if (LlvmTypeLowering::is_string_type(param_sem)) {
      param_types.push_back(llvm::PointerType::getUnqual(lowered_param));
      sig.byval_param_types.push_back(nullptr);
      continue;
    }
    if (llvm::isa<llvm::StructType>(lowered_param)) {
      auto* struct_ty = llvm::cast<llvm::StructType>(lowered_param);
      auto coercion = classify_struct_for_c_abi(struct_ty, module_->getDataLayout(), ctx_);
      if (coercion.indirect) {
        param_types.push_back(llvm::PointerType::getUnqual(lowered_param));
        sig.byval_param_types.push_back(struct_ty);
      } else {
        for (auto* coerced : coercion.coerced_types) {
          param_types.push_back(coerced);
          sig.byval_param_types.push_back(nullptr);
        }
      }
    } else {
      param_types.push_back(lowered_param);
      sig.byval_param_types.push_back(nullptr);
    }
  }

  sig.fn_type = llvm::FunctionType::get(lowered_ret, param_types, /*isVarArg=*/false);
  return sig;
}

template <typename Target>
void LlvmBackend::apply_c_abi_attrs(Target* target, const CAbiSignature& sig) {
  if (sig.sret_type != nullptr) {
    target->addParamAttr(0, llvm::Attribute::get(ctx_, llvm::Attribute::StructRet, sig.sret_type));
  }
  for (unsigned idx = 0; idx < sig.byval_param_types.size(); ++idx) {
    if (sig.byval_param_types[idx] != nullptr) {
      target->addParamAttr(
          idx, llvm::Attribute::get(ctx_, llvm::Attribute::ByVal, sig.byval_param_types[idx]));
    }
  }
}

auto LlvmBackend::emit_c_abi_call(llvm::IRBuilder<>& builder,
                                  const CAbiSignature& sig,
                                  llvm::Value* c_callee,
                                  std::span<llvm::Value* const> dao_args,
                                  const TypeFunction* sem_fn,
                                  Span span) -> llvm::Value* {
  auto* enclosing_fn = builder.GetInsertBlock()->getParent();
  auto alloc_at_entry = [&](llvm::Type* type, const llvm::Twine& name) -> llvm::AllocaInst* {
    auto* entry = &enclosing_fn->getEntryBlock();
    llvm::IRBuilder<> at_entry(entry, entry->begin());
    return at_entry.CreateAlloca(type, nullptr, name);
  };

  std::vector<llvm::Value*> call_args;
  llvm::AllocaInst* sret_alloca = nullptr;
  if (sig.sret_type != nullptr) {
    sret_alloca = alloc_at_entry(sig.sret_type, "sret");
    call_args.push_back(sret_alloca);
  }

  for (size_t i = 0; i < dao_args.size(); ++i) {
    llvm::Value* arg_val = dao_args[i];
    const Type* param_sem = (sem_fn != nullptr && i < sem_fn->param_types().size())
                                ? sem_fn->param_types()[i]
                                : nullptr;
    if (LlvmTypeLowering::is_string_type(param_sem)) {
      if (arg_val->getType() == types_.string_type()) {
        auto* tmp = alloc_at_entry(types_.string_type(), "str.arg");
        builder.CreateStore(arg_val, tmp);
        arg_val = tmp;
      }
      call_args.push_back(arg_val);
      continue;
    }
    if (arg_val->getType()->isStructTy() &&
        (param_sem == nullptr || param_sem->kind() != TypeKind::Function)) {
      auto* struct_ty = llvm::cast<llvm::StructType>(arg_val->getType());
      auto coercion = classify_struct_for_c_abi(struct_ty, module_->getDataLayout(), ctx_);
      if (coercion.indirect) {
        auto* tmp = alloc_at_entry(struct_ty, "byval.arg");
        builder.CreateStore(arg_val, tmp);
        call_args.push_back(tmp);
      } else {
        auto* tmp = alloc_at_entry(struct_ty, "coerce.arg");
        builder.CreateStore(arg_val, tmp);
        for (size_t ci = 0; ci < coercion.coerced_types.size(); ++ci) {
          uint64_t byte_offset = ci * 8;
          auto* gep = builder.CreateGEP(
              builder.getInt8Ty(), tmp, builder.getInt64(byte_offset), "coerce.gep");
          call_args.push_back(builder.CreateLoad(coercion.coerced_types[ci], gep, "coerce.load"));
        }
      }
      continue;
    }
    call_args.push_back(arg_val);
  }

  auto* call = builder.CreateCall(sig.fn_type, c_callee, call_args);
  apply_c_abi_attrs(call, sig);

  const Type* ret_sem = sem_fn != nullptr ? sem_fn->return_type() : nullptr;
  if (sret_alloca != nullptr) {
    return builder.CreateLoad(sig.sret_type, sret_alloca, "sret.load");
  }
  if (ret_sem == nullptr || ret_sem->kind() == TypeKind::Void) {
    return llvm::UndefValue::get(llvm::Type::getVoidTy(ctx_));
  }
  if (ret_sem->kind() == TypeKind::Function && call->getType()->isPointerTy()) {
    return wrap_c_fn_ptr(builder, call, static_cast<const TypeFunction*>(ret_sem), span);
  }
  auto* expected_ret = types_.lower(ret_sem);
  if (expected_ret != nullptr && llvm::isa<llvm::StructType>(expected_ret) &&
      call->getType() != expected_ret) {
    auto* tmp = alloc_at_entry(expected_ret, "coerce.ret");
    builder.CreateStore(call, tmp);
    return builder.CreateLoad(expected_ret, tmp, "coerce.ret.load");
  }
  return call;
}

auto LlvmBackend::get_or_create_c_callback_wrapper(llvm::Function* dao_fn,
                                                   const TypeFunction* sem_fn,
                                                   Span span) -> llvm::Function* {
  if (dao_fn == nullptr || sem_fn == nullptr) {
    emit_diagnostic(span, "missing function type for C callback");
    return nullptr;
  }
  const Type* ret_sem = sem_fn->return_type();
  if (ret_sem != nullptr &&
      (ret_sem->kind() == TypeKind::Function || LlvmTypeLowering::is_string_type(ret_sem))) {
    emit_diagnostic(span, "C callback return type '" + print_type(ret_sem) + "' is not supported");
    return nullptr;
  }
  bool needs_struct_abi = ret_sem != nullptr && ret_sem->kind() == TypeKind::Struct;
  for (const auto* param_sem : sem_fn->param_types()) {
    if (param_sem != nullptr &&
        (param_sem->kind() == TypeKind::Function || LlvmTypeLowering::is_string_type(param_sem))) {
      emit_diagnostic(span,
                      "C callback parameter type '" + print_type(param_sem) + "' is not supported");
      return nullptr;
    }
    if (param_sem != nullptr && param_sem->kind() == TypeKind::Struct) {
      needs_struct_abi = true;
    }
  }
  if (!needs_struct_abi) {
    return dao_fn;
  }

  auto wrapper_name = std::string(dao_fn->getName()) + ".c_cb";
  if (auto* existing = module_->getFunction(wrapper_name)) {
    return existing;
  }
  auto sig = build_c_abi_signature(sem_fn->param_types(), ret_sem, span);
  if (sig.fn_type == nullptr) {
    return nullptr;
  }
  auto* wrapper_fn = llvm::Function::Create(
      sig.fn_type, llvm::Function::InternalLinkage, wrapper_name, module_.get());
  apply_c_abi_attrs(wrapper_fn, sig);

  auto* entry = llvm::BasicBlock::Create(ctx_, "entry", wrapper_fn);
  llvm::IRBuilder<> builder(ctx_);
  builder.SetInsertPoint(entry);

  auto arg_it = wrapper_fn->arg_begin();
  llvm::Value* sret_arg = nullptr;
  if (sig.sret_type != nullptr) {
    sret_arg = &*arg_it++;
    sret_arg->setName("sret");
  }

  std::vector<llvm::Value*> dao_args;
  dao_args.reserve(sem_fn->param_types().size());
  for (const auto* param_sem : sem_fn->param_types()) {
    if (param_sem != nullptr && param_sem->kind() == TypeKind::Struct) {
      auto* struct_ty = llvm::cast<llvm::StructType>(types_.lower(param_sem));
      auto coercion = classify_struct_for_c_abi(struct_ty, module_->getDataLayout(), ctx_);
      if (coercion.indirect) {
        llvm::Value* byval_ptr = &*arg_it++;
        dao_args.push_back(builder.CreateLoad(struct_ty, byval_ptr, "byval.load"));
      } else {
        auto* tmp = builder.CreateAlloca(struct_ty, nullptr, "uncoerce.arg");
        for (size_t ci = 0; ci < coercion.coerced_types.size(); ++ci) {
          uint64_t byte_offset = ci * 8;
          auto* gep = builder.CreateGEP(
              builder.getInt8Ty(), tmp, builder.getInt64(byte_offset), "uncoerce.gep");
          builder.CreateStore(&*arg_it++, gep);
        }
        dao_args.push_back(builder.CreateLoad(struct_ty, tmp, "uncoerce.val"));
      }
    } else {
      dao_args.push_back(&*arg_it++);
    }
  }

  auto* dao_ret = builder.CreateCall(dao_fn->getFunctionType(), dao_fn, dao_args);
  if (sret_arg != nullptr) {
    builder.CreateStore(dao_ret, sret_arg);
    builder.CreateRetVoid();
  } else if (sig.fn_type->getReturnType()->isVoidTy()) {
    builder.CreateRetVoid();
  } else if (dao_ret->getType() != sig.fn_type->getReturnType()) {
    auto* tmp = builder.CreateAlloca(dao_ret->getType(), nullptr, "coerce.cb.ret");
    builder.CreateStore(dao_ret, tmp);
    builder.CreateRet(builder.CreateLoad(sig.fn_type->getReturnType(), tmp, "coerce.cb.load"));
  } else {
    builder.CreateRet(dao_ret);
  }
  return wrapper_fn;
}

auto LlvmBackend::callable_fn_type(const TypeFunction* sem_fn, Span span) -> llvm::FunctionType* {
  if (sem_fn == nullptr) {
    emit_diagnostic(span, "callable value missing function type");
    return nullptr;
  }
  std::vector<llvm::Type*> params;
  params.reserve(sem_fn->param_types().size() + 1);
  params.push_back(llvm::PointerType::getUnqual(ctx_)); // hidden env_ptr
  for (const auto* param_type : sem_fn->param_types()) {
    auto* lowered = types_.lower(param_type);
    if (lowered == nullptr) {
      emit_diagnostic(span, "cannot lower callable param type: " + types_.error());
      return nullptr;
    }
    if (LlvmTypeLowering::is_string_type(param_type)) {
      lowered = llvm::PointerType::getUnqual(lowered);
    }
    params.push_back(lowered);
  }
  auto* adj_ret = types_.lower(sem_fn->return_type());
  if (adj_ret == nullptr) {
    emit_diagnostic(span, "cannot lower callable return type: " + types_.error());
    return nullptr;
  }
  return llvm::FunctionType::get(adj_ret, params, /*isVarArg=*/false);
}

auto LlvmBackend::get_or_create_fn_thunk(const Symbol& sym, const TypeFunction* sem_fn, Span span)
    -> llvm::Function* {
  auto target_name = fn_name(sym);
  auto thunk_name = target_name + ".thunk";
  if (auto* existing = module_->getFunction(thunk_name)) {
    return existing;
  }
  auto* target_fn = module_->getFunction(target_name);
  if (target_fn == nullptr) {
    emit_diagnostic(span, "function not found for thunk: " + std::string(sym.name));
    return nullptr;
  }
  const bool is_user_extern = extern_signatures_.contains(target_fn->getName().str());
  if (is_user_extern) {
    for (const auto* param_sem : sem_fn->param_types()) {
      if (param_sem != nullptr && param_sem->kind() == TypeKind::Function) {
        emit_diagnostic(span,
                        "extern function '" + std::string(sym.name) +
                            "' with callback parameter cannot be used as a first-class function "
                            "value");
        return nullptr;
      }
    }
  }
  auto* thunk_type = callable_fn_type(sem_fn, span);
  if (thunk_type == nullptr) {
    return nullptr;
  }
  auto* thunk_fn = llvm::Function::Create(
      thunk_type, llvm::Function::InternalLinkage, thunk_name, module_.get());
  auto* entry = llvm::BasicBlock::Create(ctx_, "entry", thunk_fn);
  llvm::IRBuilder<> builder(ctx_);
  builder.SetInsertPoint(entry);

  std::vector<llvm::Value*> forward_args;
  auto arg_it = thunk_fn->arg_begin();
  arg_it->setName("env");
  ++arg_it; // skip env_ptr
  for (; arg_it != thunk_fn->arg_end(); ++arg_it) {
    forward_args.push_back(&*arg_it);
  }

  if (is_user_extern) {
    auto sig = build_c_abi_signature(sem_fn->param_types(), sem_fn->return_type(), span);
    if (sig.fn_type == nullptr) {
      thunk_fn->eraseFromParent();
      return nullptr;
    }
    auto* result = emit_c_abi_call(builder, sig, target_fn, forward_args, sem_fn, span);
    if (result == nullptr) {
      thunk_fn->eraseFromParent();
      return nullptr;
    }
    if (thunk_type->getReturnType()->isVoidTy()) {
      builder.CreateRetVoid();
    } else {
      builder.CreateRet(result);
    }
    return thunk_fn;
  }

  auto* call = builder.CreateCall(target_fn->getFunctionType(), target_fn, forward_args);
  if (thunk_type->getReturnType()->isVoidTy()) {
    builder.CreateRetVoid();
  } else {
    builder.CreateRet(call);
  }
  return thunk_fn;
}

auto LlvmBackend::wrap_c_fn_ptr(llvm::IRBuilder<>& builder,
                                llvm::Value* raw_c_fn,
                                const TypeFunction* sem_fn,
                                Span span) -> llvm::Value* {
  auto* adapter = get_or_create_c_fn_adapter(sem_fn, span);
  if (adapter == nullptr) {
    return nullptr;
  }
  llvm::Value* fat = llvm::UndefValue::get(types_.callable_type());
  fat = builder.CreateInsertValue(fat, adapter, 0, "c_fn.code");
  return builder.CreateInsertValue(fat, raw_c_fn, 1, "c_fn.env");
}

auto LlvmBackend::get_or_create_c_fn_adapter(const TypeFunction* sem_fn, Span span)
    -> llvm::Function* {
  if (sem_fn == nullptr) {
    emit_diagnostic(span, "missing function type for C function adapter");
    return nullptr;
  }
  const Type* ret_sem = sem_fn->return_type();
  if (ret_sem != nullptr &&
      (ret_sem->kind() == TypeKind::Function || LlvmTypeLowering::is_string_type(ret_sem))) {
    emit_diagnostic(span,
                    "extern fn return function type '" + print_type(sem_fn) +
                        "' is not supported at the C ABI boundary");
    return nullptr;
  }
  for (const auto* param_sem : sem_fn->param_types()) {
    if (param_sem != nullptr &&
        (param_sem->kind() == TypeKind::Function || LlvmTypeLowering::is_string_type(param_sem))) {
      emit_diagnostic(span,
                      "extern fn return function type '" + print_type(sem_fn) +
                          "' is not supported at the C ABI boundary");
      return nullptr;
    }
  }

  auto adapter_name = "__dao.c_fn_adapter." + type_identity_key(sem_fn);
  if (auto* existing = module_->getFunction(adapter_name)) {
    return existing;
  }
  auto* adapter_type = callable_fn_type(sem_fn, span);
  if (adapter_type == nullptr) {
    return nullptr;
  }
  auto sig = build_c_abi_signature(sem_fn->param_types(), ret_sem, span);
  if (sig.fn_type == nullptr) {
    return nullptr;
  }

  auto* adapter_fn = llvm::Function::Create(
      adapter_type, llvm::Function::InternalLinkage, adapter_name, module_.get());
  auto* entry = llvm::BasicBlock::Create(ctx_, "entry", adapter_fn);
  llvm::IRBuilder<> builder(ctx_);
  builder.SetInsertPoint(entry);

  auto arg_it = adapter_fn->arg_begin();
  llvm::Value* c_fn_ptr = &*arg_it;
  c_fn_ptr->setName("c_fn");
  ++arg_it;
  std::vector<llvm::Value*> forward_args;
  for (; arg_it != adapter_fn->arg_end(); ++arg_it) {
    forward_args.push_back(&*arg_it);
  }
  auto* result = emit_c_abi_call(builder, sig, c_fn_ptr, forward_args, sem_fn, span);
  if (result == nullptr) {
    adapter_fn->eraseFromParent();
    return nullptr;
  }
  if (adapter_type->getReturnType()->isVoidTy()) {
    builder.CreateRetVoid();
  } else {
    builder.CreateRet(result);
  }
  return adapter_fn;
}

auto LlvmBackend::lower_lambda(const MirLambdaInst& p, const MirInst& inst, FunctionState& state)
    -> bool {
  if (p.fn == nullptr) {
    emit_diagnostic(inst.span, "lambda instruction missing MirFunction");
    return false;
  }
  if (inst.type == nullptr || inst.type->kind() != TypeKind::Function) {
    emit_diagnostic(inst.span, "lambda instruction missing function type");
    return false;
  }
  const auto* sem_fn = static_cast<const TypeFunction*>(inst.type);
  auto* lam_fn_type = callable_fn_type(sem_fn, inst.span);
  if (lam_fn_type == nullptr) {
    return false;
  }

  uint32_t lam_id = next_lambda_id_++;
  std::string parent_prefix =
      state.llvm_fn != nullptr ? std::string(state.llvm_fn->getName()) : "__dao";
  // Strip any ".resume" suffix so generator-nested lambdas still carry the
  // parent Dao function's prefix.
  if (parent_prefix.ends_with(".resume")) {
    parent_prefix.resize(parent_prefix.size() - 7);
  }
  auto lam_name = parent_prefix + ".lambda." + std::to_string(lam_id);
  auto* lam_llvm_fn =
      llvm::Function::Create(lam_fn_type, llvm::Function::InternalLinkage, lam_name, module_.get());

  auto* ptr_ty = llvm::PointerType::getUnqual(ctx_);
  llvm::Value* env_ptr = llvm::ConstantPointerNull::get(ptr_ty);
  llvm::StructType* env_struct_ty = nullptr;
  std::vector<llvm::Type*> cap_llvm_types;

  const size_t cap_count = p.captures != nullptr ? p.captures->size() : 0;
  if (cap_count > 0) {
    std::vector<llvm::Value*> cap_vals;
    cap_vals.reserve(cap_count);
    cap_llvm_types.reserve(cap_count);
    for (size_t i = 0; i < cap_count; ++i) {
      auto* cap_val = get_value((*p.captures)[i], state);
      if (cap_val == nullptr) {
        emit_diagnostic(inst.span, "lambda capture value not found");
        return false;
      }
      cap_vals.push_back(cap_val);
      cap_llvm_types.push_back(cap_val->getType());
    }
    env_struct_ty =
        llvm::StructType::create(ctx_, cap_llvm_types, "dao.env." + std::to_string(lam_id));

    auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
    auto* null_ptr = llvm::ConstantPointerNull::get(ptr_ty);
    auto* size_gep = state.builder->CreateGEP(
        env_struct_ty, null_ptr, {llvm::ConstantInt::get(i64_ty, 1)}, "env.sizeof.gep");
    auto* env_size = state.builder->CreatePtrToInt(size_gep, i64_ty, "env.size");

    auto* align_wrap = llvm::StructType::get(ctx_, {llvm::Type::getInt8Ty(ctx_), env_struct_ty});
    auto* align_gep =
        state.builder->CreateGEP(align_wrap,
                                 null_ptr,
                                 {llvm::ConstantInt::get(i64_ty, 0),
                                  llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 1)},
                                 "env.alignof.gep");
    auto* env_align = state.builder->CreatePtrToInt(align_gep, i64_ty, "env.align");

    auto* alloc_fn = module_->getFunction(std::string(runtime_hooks::kMemAlloc));
    if (alloc_fn == nullptr) {
      emit_diagnostic(inst.span, "__dao_mem_alloc hook not declared");
      return false;
    }
    env_ptr = state.builder->CreateCall(
        alloc_fn->getFunctionType(), alloc_fn, {env_size, env_align}, "lambda.env");
    for (size_t i = 0; i < cap_count; ++i) {
      auto* slot = state.builder->CreateStructGEP(
          env_struct_ty, env_ptr, static_cast<unsigned>(i), "env.cap.ptr");
      state.builder->CreateStore(cap_vals[i], slot);
    }
  }

  llvm::Value* callable = llvm::UndefValue::get(types_.callable_type());
  callable = state.builder->CreateInsertValue(callable, lam_llvm_fn, 0, "fn.code");
  callable = state.builder->CreateInsertValue(callable, env_ptr, 1, "fn.env");
  state.values[inst.result.id] = callable;
  state.value_types[inst.result.id] = inst.type;

  // --- Lower the lambda's MirFunction body into lam_llvm_fn ---
  FunctionState lam_state;
  lam_state.llvm_fn = lam_llvm_fn;
  for (const auto* block : p.fn->blocks) {
    auto* llvm_block =
        llvm::BasicBlock::Create(ctx_, "bb" + std::to_string(block->id.id), lam_llvm_fn);
    lam_state.blocks[block->id.id] = llvm_block;
  }

  llvm::IRBuilder<> lam_builder(ctx_);
  lam_state.builder = &lam_builder;
  lam_builder.SetInsertPoint(lam_state.blocks[p.fn->blocks[0]->id.id]);

  size_t param_count = 0;
  for (const auto& local : p.fn->locals) {
    if (local.is_param) {
      ++param_count;
    }
    if (local.type != nullptr && local.type->kind() == TypeKind::Void) {
      emit_diagnostic(local.span, "a binding of type 'void' has no storage");
      return false;
    }
    auto* local_type = types_.lower(local.type);
    if (local_type == nullptr) {
      emit_diagnostic(local.span, "cannot lower lambda local type: " + types_.error());
      return false;
    }
    auto* alloca = lam_builder.CreateAlloca(
        local_type, nullptr, local.symbol != nullptr ? std::string(local.symbol->name) : "");
    lam_state.locals.slots[local.id.id] = alloca;
    lam_state.locals.types[local.id.id] = local.type;
  }

  auto arg_it = lam_llvm_fn->arg_begin();
  llvm::Value* env_arg = &*arg_it;
  env_arg->setName("env");
  ++arg_it;

  for (size_t i = 0; i < param_count && arg_it != lam_llvm_fn->arg_end(); ++i, ++arg_it) {
    const auto& param_local = p.fn->locals[i];
    if (param_local.symbol != nullptr) {
      arg_it->setName(std::string(param_local.symbol->name));
    }
    auto* alloca = lam_state.locals.slots[param_local.id.id];
    if (LlvmTypeLowering::is_string_type(param_local.type)) {
      auto* loaded = lam_builder.CreateLoad(types_.string_type(), &*arg_it);
      lam_builder.CreateStore(loaded, alloca);
    } else {
      lam_builder.CreateStore(&*arg_it, alloca);
    }
  }

  if (env_struct_ty != nullptr) {
    for (size_t i = 0; i < p.fn->capture_count; ++i) {
      const auto& cap_local = p.fn->locals[param_count + i];
      auto* cap_gep =
          lam_builder.CreateStructGEP(env_struct_ty, env_arg, static_cast<unsigned>(i), "cap.gep");
      auto* cap_val = lam_builder.CreateLoad(cap_llvm_types[i], cap_gep, "cap.load");
      lam_builder.CreateStore(cap_val, lam_state.locals.slots[cap_local.id.id]);
    }
  }

  for (const auto* block : p.fn->blocks) {
    if (!lower_block(*block, lam_state)) {
      return false;
    }
  }

  return true;
}

auto LlvmBackend::lower_call(const MirCall& p, const MirInst& inst, FunctionState& state) -> bool {
  // Check for compiler builtin intrinsics (size_of$T, align_of$T).
  auto builtin_it = state.callees.builtin_names.find(p.callee.id);
  if (builtin_it != state.callees.builtin_names.end()) {
    return lower_builtin_call(builtin_it->second, p, inst, state);
  }

  llvm::Function* callee_fn = nullptr;
  auto fn_ref_it = state.callees.fn_ref_symbols.find(p.callee.id);
  if (fn_ref_it != state.callees.fn_ref_symbols.end()) {
    callee_fn = module_->getFunction(fn_name(*fn_ref_it->second));
    if (callee_fn == nullptr) {
      emit_diagnostic(inst.span, "call callee function not found");
      return false;
    }
  }

  // Indirect call: callee is a first-class Dao callable `%dao.fn`
  // (`{ ptr fn_ptr, ptr env_ptr }`), not a direct static function ref.
  if (callee_fn == nullptr) {
    auto* callee_val = get_value(p.callee, state);
    if (callee_val == nullptr) {
      emit_diagnostic(inst.span, "call callee not found");
      return false;
    }
    auto callee_type_it = state.value_types.find(p.callee.id);
    if (callee_type_it == state.value_types.end() ||
        callee_type_it->second == nullptr ||
        callee_type_it->second->kind() != TypeKind::Function) {
      emit_diagnostic(inst.span, "call callee is not a function");
      return false;
    }
    const auto* sem_fn = static_cast<const TypeFunction*>(callee_type_it->second);
    auto* llvm_fn_type = callable_fn_type(sem_fn, inst.span);
    if (llvm_fn_type == nullptr) {
      return false;
    }

    auto* fn_ptr = state.builder->CreateExtractValue(callee_val, 0, "fn.code");
    auto* env_ptr = state.builder->CreateExtractValue(callee_val, 1, "fn.env");

    std::vector<llvm::Value*> indirect_args;
    indirect_args.push_back(env_ptr);
    if (p.args != nullptr) {
      for (size_t i = 0; i < p.args->size(); ++i) {
        auto* arg_val = get_value((*p.args)[i], state);
        if (arg_val == nullptr) {
          emit_diagnostic(inst.span, "call argument not found");
          return false;
        }
        // String → pointer coercion (offset +1 past env_ptr).
        if (i + 1 < llvm_fn_type->getNumParams()) {
          auto* param_type = llvm_fn_type->getParamType(static_cast<unsigned>(i + 1));
          if (param_type->isPointerTy() && arg_val->getType() == types_.string_type()) {
            auto* tmp = entry_alloca(state, types_.string_type(), "str.arg");
            state.builder->CreateStore(arg_val, tmp);
            arg_val = tmp;
          }
        }
        indirect_args.push_back(arg_val);
      }
    }

    auto* call =
        state.builder->CreateCall(llvm_fn_type,
                                  fn_ptr,
                                  indirect_args,
                                  llvm_fn_type->getReturnType()->isVoidTy() ? "" : "icall");
    if (!llvm_fn_type->getReturnType()->isVoidTy() && inst.result.valid()) {
      state.values[inst.result.id] = call;
      state.value_types[inst.result.id] = inst.type;
    }
    return true;
  }

  if (extern_signatures_.contains(callee_fn->getName().str())) {
    const TypeFunction* callee_sem_fn = nullptr;
    auto callee_ty_it = state.value_types.find(p.callee.id);
    if (callee_ty_it != state.value_types.end() && callee_ty_it->second != nullptr &&
        callee_ty_it->second->kind() == TypeKind::Function) {
      callee_sem_fn = static_cast<const TypeFunction*>(callee_ty_it->second);
    }

    std::vector<llvm::Value*> dao_args;
    std::vector<const Type*> param_sem_types;
    if (p.args != nullptr) {
      dao_args.reserve(p.args->size());
      param_sem_types.reserve(p.args->size());
      for (size_t i = 0; i < p.args->size(); ++i) {
        const Type* arg_sem =
            (callee_sem_fn != nullptr && i < callee_sem_fn->param_types().size())
                ? callee_sem_fn->param_types()[i]
                : (state.value_types.contains((*p.args)[i].id) ? state.value_types[(*p.args)[i].id]
                                                               : nullptr);
        param_sem_types.push_back(arg_sem);

        auto arg_fn_it = state.callees.fn_ref_symbols.find((*p.args)[i].id);
        if (arg_fn_it != state.callees.fn_ref_symbols.end() && arg_sem != nullptr &&
            arg_sem->kind() == TypeKind::Function) {
          auto* cb_fn = module_->getFunction(fn_name(*arg_fn_it->second));
          if (cb_fn == nullptr) {
            emit_diagnostic(inst.span, "extern callback function not found");
            return false;
          }
          auto* cb_wrapped = get_or_create_c_callback_wrapper(
              cb_fn, static_cast<const TypeFunction*>(arg_sem), inst.span);
          if (cb_wrapped == nullptr) {
            return false;
          }
          dao_args.push_back(cb_wrapped);
          continue;
        }
        auto* arg_val = get_value((*p.args)[i], state);
        if (arg_val == nullptr) {
          emit_diagnostic(inst.span, "call argument not found");
          return false;
        }
        dao_args.push_back(arg_val);
      }
    }

    const Type* ret_sem = callee_sem_fn != nullptr ? callee_sem_fn->return_type() : inst.type;
    auto sig = build_c_abi_signature(param_sem_types, ret_sem, inst.span);
    if (sig.fn_type == nullptr) {
      return false;
    }
    CAbiSignature actual_sig = sig;
    actual_sig.fn_type = callee_fn->getFunctionType();
    auto* result =
        emit_c_abi_call(*state.builder, actual_sig, callee_fn, dao_args, callee_sem_fn, inst.span);
    if (result == nullptr) {
      return false;
    }
    if (ret_sem != nullptr && ret_sem->kind() != TypeKind::Void && inst.result.valid()) {
      state.values[inst.result.id] = result;
      state.value_types[inst.result.id] = inst.type;
    }
    return true;
  }

  std::vector<llvm::Value*> args;
  if (p.args != nullptr) {
    for (size_t i = 0; i < p.args->size(); ++i) {
      auto* arg_val = get_value((*p.args)[i], state);
      if (arg_val == nullptr) {
        emit_diagnostic(inst.span, "call argument not found");
        return false;
      }
      if (i < callee_fn->getFunctionType()->getNumParams()) {
        auto* param_type = callee_fn->getFunctionType()->getParamType(static_cast<unsigned>(i));
        if (param_type->isPointerTy() && arg_val->getType() == types_.string_type()) {
          auto* tmp = entry_alloca(state, types_.string_type(), "str.arg");
          state.builder->CreateStore(arg_val, tmp);
          arg_val = tmp;
        }
      }
      args.push_back(arg_val);
    }
  }

  auto* call = state.builder->CreateCall(callee_fn->getFunctionType(), callee_fn, args);
  if (!callee_fn->getReturnType()->isVoidTy() && inst.result.valid()) {
    state.values[inst.result.id] = call;
    state.value_types[inst.result.id] = inst.type;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Struct construction
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_construct(const MirConstruct& p, const MirInst& inst,
                                    FunctionState& state) -> bool {
  if (p.struct_type == nullptr) {
    emit_diagnostic(inst.span, "construct with null struct type");
    return false;
  }

  auto* llvm_type = types_.lower(p.struct_type);
  if (llvm_type == nullptr) {
    emit_diagnostic(inst.span,
                    "cannot lower construct type: " + types_.error());
    return false;
  }

  // Validate field count matches struct layout.
  size_t value_count =
      p.field_values != nullptr ? p.field_values->size() : 0;
  if (value_count != p.struct_type->fields().size()) {
    emit_diagnostic(inst.span,
                    "construct field count mismatch: expected " +
                        std::to_string(p.struct_type->fields().size()) +
                        ", got " + std::to_string(value_count));
    return false;
  }

  llvm::Value* agg = llvm::UndefValue::get(llvm_type);
  if (p.field_values != nullptr) {
    for (unsigned i = 0; i < p.field_values->size(); ++i) {
      auto* val = get_value((*p.field_values)[i], state);
      if (val == nullptr) {
        emit_diagnostic(inst.span, "construct field value not found");
        return false;
      }
      agg = state.builder->CreateInsertValue(
          agg, val, i, "ctor." + std::to_string(i));
    }
  }

  state.values[inst.result.id] = agg;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

// ---------------------------------------------------------------------------
// Enum operations
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_enum_construct(const MirEnumConstruct& p,
                                       const MirInst& inst,
                                       FunctionState& state) -> bool {
  if (p.enum_type == nullptr) {
    emit_diagnostic(inst.span, "enum construct with null type");
    return false;
  }

  auto* llvm_type = types_.lower(p.enum_type);
  if (llvm_type == nullptr) {
    emit_diagnostic(inst.span,
                    "cannot lower enum type: " + types_.error());
    return false;
  }

  // Build the payload struct type for this variant.
  const auto& variant = p.enum_type->variants()[p.variant_index];
  std::vector<llvm::Type*> field_types;
  for (const auto* pt : variant.payload_types) {
    auto* lt = types_.lower(pt);
    if (lt == nullptr) {
      emit_diagnostic(inst.span,
                      "cannot lower payload field type: " + types_.error());
      return false;
    }
    field_types.push_back(lt);
  }
  auto* payload_struct_type =
      llvm::StructType::get(ctx_, field_types, /*isPacked=*/false);

  // Allocate the enum struct on the stack.
  auto* alloca = entry_alloca(state, llvm_type, "enum.tmp");

  // Store discriminant (field 0).
  auto* tag_ptr = state.builder->CreateStructGEP(llvm_type, alloca, 0, "enum.tag");
  state.builder->CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), p.variant_index),
      tag_ptr);

  // Bitcast payload region (field 1) to the variant's struct type, store fields.
  auto* payload_ptr = state.builder->CreateStructGEP(llvm_type, alloca, 1, "enum.payload");
  auto* typed_payload = state.builder->CreateBitCast(
      payload_ptr, payload_struct_type->getPointerTo(), "enum.payload.typed");

  if (p.payload_values != nullptr) {
    for (unsigned i = 0; i < p.payload_values->size(); ++i) {
      auto* val = get_value((*p.payload_values)[i], state);
      if (val == nullptr) {
        emit_diagnostic(inst.span, "enum payload value not found");
        return false;
      }
      auto* field_ptr = state.builder->CreateStructGEP(
          payload_struct_type, typed_payload, i, "enum.field." + std::to_string(i));
      state.builder->CreateStore(val, field_ptr);
    }
  }

  // Load the complete enum value.
  auto* result = state.builder->CreateLoad(llvm_type, alloca, "enum.val");
  state.values[inst.result.id] = result;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

auto LlvmBackend::lower_enum_discriminant(const MirEnumDiscriminant& p,
                                          const MirInst& inst,
                                          FunctionState& state) -> bool {
  auto* enum_val = get_value(p.enum_value, state);
  if (enum_val == nullptr) {
    emit_diagnostic(inst.span, "enum discriminant: value not found");
    return false;
  }

  // For payload-free enums, the value IS the i32 discriminant.
  if (enum_val->getType()->isIntegerTy(32)) {
    state.values[inst.result.id] = enum_val;
    state.value_types[inst.result.id] = inst.type;
    return true;
  }

  // For payload-bearing enums, extract field 0 (the tag).
  auto* tag = state.builder->CreateExtractValue(enum_val, 0, "enum.tag");
  state.values[inst.result.id] = tag;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

auto LlvmBackend::lower_enum_payload(const MirEnumPayload& p,
                                     const MirInst& inst,
                                     FunctionState& state) -> bool {
  auto* enum_val = get_value(p.enum_value, state);
  if (enum_val == nullptr) {
    emit_diagnostic(inst.span, "enum payload: value not found");
    return false;
  }

  // Look up the semantic enum type from the value.
  auto type_it = state.value_types.find(p.enum_value.id);
  if (type_it == state.value_types.end() ||
      type_it->second->kind() != TypeKind::Enum) {
    emit_diagnostic(inst.span, "enum payload: cannot find enum type");
    return false;
  }
  const auto* enum_type = static_cast<const TypeEnum*>(type_it->second);
  const auto& variant = enum_type->variants()[p.variant_index];

  // Build the payload struct type for this variant.
  std::vector<llvm::Type*> field_types;
  for (const auto* pt : variant.payload_types) {
    auto* lt = types_.lower(pt);
    if (lt == nullptr) {
      return false;
    }
    field_types.push_back(lt);
  }
  auto* payload_struct_type =
      llvm::StructType::get(ctx_, field_types, /*isPacked=*/false);

  // Get the LLVM type of the whole enum.
  auto* llvm_enum_type = types_.lower(enum_type);
  if (llvm_enum_type == nullptr) {
    return false;
  }

  // Alloca the enum value, then GEP into the payload region, bitcast,
  // and load the requested field.
  auto* alloca = entry_alloca(state, llvm_enum_type, "enum.ext");
  state.builder->CreateStore(enum_val, alloca);

  auto* payload_ptr = state.builder->CreateStructGEP(
      llvm_enum_type, alloca, 1, "enum.payload");
  auto* typed_payload = state.builder->CreateBitCast(
      payload_ptr, payload_struct_type->getPointerTo(), "enum.payload.typed");
  auto* field_ptr = state.builder->CreateStructGEP(
      payload_struct_type, typed_payload, p.field_index, "enum.field");
  auto* result = state.builder->CreateLoad(
      field_types[p.field_index], field_ptr, "enum.field.val");

  state.values[inst.result.id] = result;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

// ---------------------------------------------------------------------------
// Terminators
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_return(const MirReturn& p, const MirInst& inst,
                                 FunctionState& state) -> bool {
  // Generator resume: mark done and return void.
  if (state.gen.frame_ptr != nullptr) {
    auto* done_ptr = state.builder->CreateStructGEP(
        state.gen.frame_type, state.gen.frame_ptr, 1, "done.ptr");
    state.builder->CreateStore(state.builder->getTrue(), done_ptr);
    state.builder->CreateRetVoid();
    return true;
  }

  // Void functions always emit ret void, even if MIR has a value
  // (e.g. returning a void call result).
  if (state.llvm_fn->getReturnType()->isVoidTy()) {
    state.builder->CreateRetVoid();
    return true;
  }

  if (p.has_value) {
    auto* val = get_value(p.value, state);
    if (val == nullptr) {
      emit_diagnostic(inst.span, "return value not found");
      return false;
    }
    state.builder->CreateRet(val);
  } else {
    // The builder's implicit return closes a block no path reaches, such
    // as the fall-through of an exhaustive match. A non-void function has
    // no valid `ret void`, and AArch64's CFG pass crashes on one.
    state.builder->CreateUnreachable();
  }
  return true;
}

auto LlvmBackend::lower_br(const MirBr& p, const MirInst& inst,
                              FunctionState& state) -> bool {
  auto it = state.blocks.find(p.target.id);
  if (it == state.blocks.end()) {
    emit_diagnostic(inst.span, "branch target block not found");
    return false;
  }
  state.builder->CreateBr(it->second);
  return true;
}

auto LlvmBackend::lower_cond_br(const MirCondBr& p, const MirInst& inst,
                                   FunctionState& state) -> bool {
  auto* cond = get_value(p.cond, state);
  if (cond == nullptr) {
    emit_diagnostic(inst.span, "conditional branch condition not found");
    return false;
  }

  auto then_it = state.blocks.find(p.then_block.id);
  auto else_it = state.blocks.find(p.else_block.id);
  if (then_it == state.blocks.end() || else_it == state.blocks.end()) {
    emit_diagnostic(inst.span, "conditional branch target block not found");
    return false;
  }

  state.builder->CreateCondBr(cond, then_it->second, else_it->second);
  return true;
}

// ---------------------------------------------------------------------------
// Generator detection
// ---------------------------------------------------------------------------

auto LlvmBackend::is_generator_function(const MirFunction& fn) -> bool {
  return fn.return_type != nullptr &&
         fn.return_type->kind() == TypeKind::Generator;
}

// ---------------------------------------------------------------------------
// Generator frame type construction
// ---------------------------------------------------------------------------

auto LlvmBackend::create_generator_frame_type(const MirFunction& fn)
    -> llvm::StructType* {
  auto frame_name = "dao.gen." + fn_name(*fn.symbol);

  // Return cached type if already created.
  auto* existing = llvm::StructType::getTypeByName(ctx_, frame_name);
  if (existing != nullptr) {
    return existing;
  }

  const auto* gen_type = static_cast<const TypeGenerator*>(fn.return_type);
  auto* yield_llvm = types_.lower(gen_type->yield_type());
  if (yield_llvm == nullptr) {
    return nullptr;
  }

  // Frame layout: { i32 state, i1 done, T yield_slot, locals... }
  std::vector<llvm::Type*> fields;
  fields.push_back(llvm::Type::getInt32Ty(ctx_));  // 0: state
  fields.push_back(llvm::Type::getInt1Ty(ctx_));   // 1: done
  fields.push_back(yield_llvm);                     // 2: yield_slot

  for (const auto& local : fn.locals) {
    if (local.type != nullptr && local.type->kind() == TypeKind::Void) {
      emit_diagnostic(local.span, "a binding of type 'void' has no storage");
      return nullptr;
    }
    auto* lt = types_.lower(local.type);
    if (lt == nullptr) {
      return nullptr;
    }
    fields.push_back(lt);
  }

  for (const auto* block : fn.blocks) {
    for (const auto* inst : block->insts) {
      if (std::holds_alternative<MirIterInit>(inst->payload)) {
        fields.push_back(types_.generator_type());
      }
    }
  }

  return llvm::StructType::create(ctx_, fields, frame_name);
}

// ---------------------------------------------------------------------------
// Generator init function lowering
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_generator_init(const MirFunction& fn) -> bool {
  if (fn.symbol == nullptr || fn.is_extern) {
    return true;
  }

  auto* init_fn = module_->getFunction(fn_name(*fn.symbol));
  if (init_fn == nullptr) {
    emit_diagnostic(fn.span, "generator init function not declared");
    return false;
  }

  auto* frame_type = create_generator_frame_type(fn);
  if (frame_type == nullptr) {
    emit_diagnostic(fn.span, "cannot create generator frame type");
    return false;
  }

  auto* entry = llvm::BasicBlock::Create(ctx_, "entry", init_fn);
  llvm::IRBuilder<> builder(ctx_);
  builder.SetInsertPoint(entry);

  // Compute sizeof(frame) and alignof(frame) via GEP-from-null tricks
  // (target-independent — correct regardless of data layout).
  auto* i64_ty = llvm::Type::getInt64Ty(ctx_);
  auto* null_ptr =
      llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(ctx_));

  // sizeof: offset of element 1 from a null pointer.
  auto* size_gep = builder.CreateGEP(
      frame_type, null_ptr,
      {llvm::ConstantInt::get(i64_ty, 1)}, "sizeof.gep");
  auto* frame_size =
      builder.CreatePtrToInt(size_gep, i64_ty, "frame.size");

  // alignof: offsetof(struct { i8; frame_type }, field_1).
  auto* align_wrap = llvm::StructType::get(
      ctx_, {llvm::Type::getInt8Ty(ctx_), frame_type});
  auto* align_gep = builder.CreateGEP(
      align_wrap, null_ptr,
      {llvm::ConstantInt::get(i64_ty, 0),
       llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 1)},
      "alignof.gep");
  auto* frame_align =
      builder.CreatePtrToInt(align_gep, i64_ty, "frame.align");

  // Call __dao_gen_alloc(size, align).
  auto* alloc_fn =
      module_->getFunction(std::string(runtime_hooks::kGenAlloc));
  auto* frame_ptr = builder.CreateCall(
      alloc_fn->getFunctionType(), alloc_fn,
      {frame_size, frame_align}, "frame");

  // Store state = 0.
  auto* state_ptr =
      builder.CreateStructGEP(frame_type, frame_ptr, 0, "state.ptr");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx_), 0), state_ptr);

  // Store done = false.
  auto* done_ptr =
      builder.CreateStructGEP(frame_type, frame_ptr, 1, "done.ptr");
  builder.CreateStore(builder.getFalse(), done_ptr);

  // Store parameters into frame (fields 3+).
  uint32_t frame_field = 3;
  for (auto& arg : init_fn->args()) {
    auto* field_ptr = builder.CreateStructGEP(
        frame_type, frame_ptr, frame_field, "param.ptr");
    builder.CreateStore(&arg, field_ptr);
    ++frame_field;
  }

  // Build the generator fat pair: { ptr frame, ptr resume_fn }.
  auto* resume_fn = module_->getFunction(fn_name(*fn.symbol) + ".resume");
  auto* gen_type = types_.generator_type();
  llvm::Value* gen_val = llvm::UndefValue::get(gen_type);
  gen_val =
      builder.CreateInsertValue(gen_val, frame_ptr, 0, "gen.frame");
  gen_val =
      builder.CreateInsertValue(gen_val, resume_fn, 1, "gen.resume");

  builder.CreateRet(gen_val);
  return true;
}

// ---------------------------------------------------------------------------
// Generator resume function lowering
// ---------------------------------------------------------------------------

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto LlvmBackend::lower_generator_resume(const MirFunction& fn) -> bool {
  if (fn.symbol == nullptr || fn.is_extern) {
    return true;
  }

  auto resume_name = fn_name(*fn.symbol) + ".resume";
  auto* resume_fn = module_->getFunction(resume_name);
  if (resume_fn == nullptr) {
    emit_diagnostic(fn.span,
                    "resume function not declared: " + resume_name);
    return false;
  }

  auto* frame_type = create_generator_frame_type(fn);
  if (frame_type == nullptr) {
    emit_diagnostic(fn.span, "cannot create generator frame type");
    return false;
  }

  auto* frame_arg = resume_fn->getArg(0);
  frame_arg->setName("frame");

  FunctionState state;
  state.llvm_fn = resume_fn;
  state.gen.frame_ptr = frame_arg;
  state.gen.frame_type = frame_type;

  llvm::IRBuilder<> builder(ctx_);
  state.builder = &builder;

  // --- Pre-scan: count yield points ---
  uint32_t yield_count = 0;
  for (const auto* block : fn.blocks) {
    for (const auto* inst : block->insts) {
      if (std::holds_alternative<MirYieldInst>(inst->payload)) {
        ++yield_count;
      }
    }
  }

  // --- Create entry block first (must be first for LLVM entry) ---
  auto* entry_bb = llvm::BasicBlock::Create(ctx_, "entry", resume_fn);

  // --- Create LLVM blocks for MIR blocks ---
  for (const auto* block : fn.blocks) {
    auto* llvm_block = llvm::BasicBlock::Create(
        ctx_, "bb" + std::to_string(block->id.id), resume_fn);
    state.blocks[block->id.id] = llvm_block;
  }

  // --- Create resume blocks for each yield point ---
  std::vector<llvm::BasicBlock*> resume_blocks;
  for (uint32_t i = 0; i < yield_count; ++i) {
    auto* bb = llvm::BasicBlock::Create(
        ctx_, "resume." + std::to_string(i + 1), resume_fn);
    resume_blocks.push_back(bb);
  }

  auto* unreachable_bb =
      llvm::BasicBlock::Create(ctx_, "unreachable", resume_fn);

  // --- Entry block: compute frame GEPs for locals, then dispatch ---
  builder.SetInsertPoint(entry_bb);

  // Set up frame-based locals (GEPs at field indices 3+).
  uint32_t frame_field = 3;
  for (const auto& local : fn.locals) {
    auto name = local.symbol != nullptr
        ? std::string(local.symbol->name) + ".ptr"
        : "local." + std::to_string(local.id.id) + ".ptr";
    auto* gep = builder.CreateStructGEP(
        frame_type, frame_arg, frame_field, name);
    state.locals.slots[local.id.id] = gep;
    state.locals.types[local.id.id] = local.type;
    ++frame_field;
  }

  for (const auto* block : fn.blocks) {
    for (const auto* inst : block->insts) {
      if (std::holds_alternative<MirIterInit>(inst->payload)) {
        state.gen.iter_frame_fields[inst->result.id] = frame_field++;
      }
    }
  }

  // Load state and dispatch.
  auto* state_ptr = builder.CreateStructGEP(
      frame_type, frame_arg, 0, "state.ptr");
  auto* state_val = builder.CreateLoad(
      llvm::Type::getInt32Ty(ctx_), state_ptr, "state");

  auto* sw = builder.CreateSwitch(
      state_val, unreachable_bb, yield_count + 1);
  sw->addCase(builder.getInt32(0),
              state.blocks[fn.blocks[0]->id.id]);
  for (uint32_t i = 0; i < yield_count; ++i) {
    sw->addCase(builder.getInt32(i + 1), resume_blocks[i]);
  }

  // --- Unreachable block: set done, ret void ---
  builder.SetInsertPoint(unreachable_bb);
  auto* done_unr = builder.CreateStructGEP(
      frame_type, frame_arg, 1, "done.ptr");
  builder.CreateStore(builder.getTrue(), done_unr);
  builder.CreateRetVoid();

  // --- Lower MIR blocks with yield splitting ---
  uint32_t yield_idx = 0;
  for (const auto* block : fn.blocks) {
    builder.SetInsertPoint(state.blocks[block->id.id]);

    for (const auto* inst : block->insts) {
      const auto* yp = std::get_if<MirYieldInst>(&inst->payload);
      if (yp != nullptr) {
        // Store value to yield slot (frame field 2).
        auto* val = get_value(yp->value, state);
        if (val == nullptr) {
          emit_diagnostic(inst->span, "yield value not found");
          return false;
        }
        auto* yield_ptr = builder.CreateStructGEP(
            frame_type, frame_arg, 2, "yield.ptr");
        builder.CreateStore(val, yield_ptr);

        // Store next state number.
        auto* st_ptr = builder.CreateStructGEP(
            frame_type, frame_arg, 0, "state.ptr");
        builder.CreateStore(
            builder.getInt32(yield_idx + 1), st_ptr);

        // Return to caller.
        builder.CreateRetVoid();

        // Remaining instructions continue in resume block.
        builder.SetInsertPoint(resume_blocks[yield_idx]);
        ++yield_idx;
      } else {
        if (!lower_inst(*inst, state)) {
          return false;
        }
      }
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Iterator operations (consumer side)
// ---------------------------------------------------------------------------

auto LlvmBackend::lower_iter_init(const MirIterInit& p, const MirInst& inst,
                                    FunctionState& state) -> bool {
  auto* gen_val = get_value(p.iter_operand, state);
  if (gen_val == nullptr) {
    emit_diagnostic(inst.span, "iter_init: generator value not found");
    return false;
  }

  // Extract frame pointer and resume function from the generator pair.
  auto* frame_ptr =
      state.builder->CreateExtractValue(gen_val, 0, "gen.frame");
  auto* resume_ptr =
      state.builder->CreateExtractValue(gen_val, 1, "gen.resume");

  // Build the resume function type: void(ptr).
  auto* ptr_type = llvm::PointerType::getUnqual(ctx_);
  auto* void_type = llvm::Type::getVoidTy(ctx_);
  auto* resume_fn_type =
      llvm::FunctionType::get(void_type, {ptr_type}, /*isVarArg=*/false);

  // Call resume to advance to the first yield point (or completion).
  state.builder->CreateCall(resume_fn_type, resume_ptr, {frame_ptr});

  // Propagate tracking to iter_init result for has_next/next/destroy.
  state.values[inst.result.id] = frame_ptr;
  state.value_types[inst.result.id] = inst.type;
  auto frame_slot_it = state.gen.iter_frame_fields.find(inst.result.id);
  if (frame_slot_it != state.gen.iter_frame_fields.end()) {
    auto* slot_ptr = state.builder->CreateStructGEP(
        state.gen.frame_type, state.gen.frame_ptr, frame_slot_it->second, "iter.slot");
    state.builder->CreateStore(gen_val, slot_ptr);
  } else {
    state.gen.iter_state[inst.result.id] = {frame_ptr, resume_ptr};
  }

  return true;
}

auto LlvmBackend::load_iter_pair(uint32_t iter_id,
                                 FunctionState& state,
                                 llvm::Value*& frame_ptr,
                                 llvm::Value*& resume_ptr) -> bool {
  auto frame_slot_it = state.gen.iter_frame_fields.find(iter_id);
  if (frame_slot_it != state.gen.iter_frame_fields.end()) {
    auto* slot_ptr = state.builder->CreateStructGEP(
        state.gen.frame_type, state.gen.frame_ptr, frame_slot_it->second, "iter.slot");
    auto* gen_val = state.builder->CreateLoad(types_.generator_type(), slot_ptr, "iter.gen");
    frame_ptr = state.builder->CreateExtractValue(gen_val, 0, "iter.frame");
    resume_ptr = state.builder->CreateExtractValue(gen_val, 1, "iter.resume");
    return true;
  }
  auto iter_it = state.gen.iter_state.find(iter_id);
  if (iter_it == state.gen.iter_state.end()) {
    return false;
  }
  frame_ptr = iter_it->second.first;
  resume_ptr = iter_it->second.second;
  return true;
}

auto LlvmBackend::lower_iter_has_next(const MirIterHasNext& p,
                                      const MirInst& inst,
                                      FunctionState& state) -> bool {
  llvm::Value* frame_ptr = nullptr;
  llvm::Value* resume_ptr = nullptr;
  if (!load_iter_pair(p.iter_operand.id, state, frame_ptr, resume_ptr)) {
    emit_diagnostic(inst.span, "iter_has_next: iterator not found");
    return false;
  }

  // Derive yield type from the iter_operand's semantic type (Generator<T>).
  const auto* operand_type = state.value_types[p.iter_operand.id];
  if (operand_type == nullptr || operand_type->kind() != TypeKind::Generator) {
    emit_diagnostic(inst.span, "iter_has_next: operand is not a Generator");
    return false;
  }
  const auto* gen = static_cast<const TypeGenerator*>(operand_type);
  auto* yield_llvm = types_.lower(gen->yield_type());
  if (yield_llvm == nullptr) {
    emit_diagnostic(inst.span,
                    "iter_has_next: cannot lower yield type: " +
                    types_.error());
    return false;
  }

  // Consumer view of the frame header: { i32 state, i1 done, T yield_slot }.
  auto* view = llvm::StructType::get(ctx_, {
      llvm::Type::getInt32Ty(ctx_),
      llvm::Type::getInt1Ty(ctx_),
      yield_llvm});

  auto* done_ptr =
      state.builder->CreateStructGEP(view, frame_ptr, 1, "done.ptr");
  auto* done = state.builder->CreateLoad(
      llvm::Type::getInt1Ty(ctx_), done_ptr, "done");
  auto* has_next = state.builder->CreateNot(done, "has_next");

  state.values[inst.result.id] = has_next;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

auto LlvmBackend::lower_iter_next(const MirIterNext& p, const MirInst& inst, FunctionState& state)
    -> bool {
  llvm::Value* frame_ptr = nullptr;
  llvm::Value* resume_ptr = nullptr;
  if (!load_iter_pair(p.iter_operand.id, state, frame_ptr, resume_ptr)) {
    emit_diagnostic(inst.span, "iter_next: iterator not found");
    return false;
  }

  // Derive yield type from the iter_operand's semantic type (Generator<T>).
  const auto* operand_type = state.value_types[p.iter_operand.id];
  if (operand_type == nullptr || operand_type->kind() != TypeKind::Generator) {
    emit_diagnostic(inst.span, "iter_next: operand is not a Generator");
    return false;
  }
  const auto* gen = static_cast<const TypeGenerator*>(operand_type);
  auto* yield_llvm = types_.lower(gen->yield_type());
  if (yield_llvm == nullptr) {
    emit_diagnostic(inst.span,
                    "iter_next: cannot lower yield type: " +
                    types_.error());
    return false;
  }

  // Consumer view of the frame header: { i32 state, i1 done, T yield_slot }.
  auto* view = llvm::StructType::get(ctx_, {
      llvm::Type::getInt32Ty(ctx_),
      llvm::Type::getInt1Ty(ctx_),
      yield_llvm});

  // Read current yield value.
  auto* yield_ptr =
      state.builder->CreateStructGEP(view, frame_ptr, 2, "yield.ptr");
  auto* yield_val = state.builder->CreateLoad(
      yield_llvm, yield_ptr, "yield.val");

  // Call resume (indirect) to advance to next yield point.
  auto* ptr_type = llvm::PointerType::getUnqual(ctx_);
  auto* void_type = llvm::Type::getVoidTy(ctx_);
  auto* resume_fn_type =
      llvm::FunctionType::get(void_type, {ptr_type}, /*isVarArg=*/false);
  state.builder->CreateCall(resume_fn_type, resume_ptr, {frame_ptr});

  state.values[inst.result.id] = yield_val;
  state.value_types[inst.result.id] = inst.type;
  return true;
}

auto LlvmBackend::lower_iter_destroy(const MirIterDestroy& p,
                                     const MirInst& inst,
                                     FunctionState& state) -> bool {
  llvm::Value* frame_ptr = nullptr;
  llvm::Value* resume_ptr = nullptr;
  if (!load_iter_pair(p.iter_operand.id, state, frame_ptr, resume_ptr)) {
    emit_diagnostic(inst.span, "iter_destroy: iterator state not found");
    return false;
  }

  auto* free_fn =
      module_->getFunction(std::string(runtime_hooks::kGenFree));
  if (free_fn == nullptr) {
    emit_diagnostic(inst.span,
                    "iter_destroy: __dao_gen_free not declared");
    return false;
  }

  state.builder->CreateCall(
      free_fn->getFunctionType(), free_fn, {frame_ptr});
  return true;
}

} // namespace dao

// NOLINTEND(readability-identifier-length)
