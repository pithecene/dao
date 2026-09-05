// NOLINTBEGIN(readability-magic-numbers,readability-identifier-length)
#include "ir/mir/mir_monomorphize.h"

#include "frontend/types/type.h"
#include "frontend/types/type_printer.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dao {

namespace {

// ---------------------------------------------------------------------------
// Type substitution: replace TypeGenericParam with concrete types.
// ---------------------------------------------------------------------------

using TypeSubst = std::unordered_map<uint32_t, const Type*>;

auto substitute_type(const Type* type, const TypeSubst& subst,
                     TypeContext& types) -> const Type* {
  if (type == nullptr) {
    return nullptr;
  }

  switch (type->kind()) {
  case TypeKind::GenericParam: {
    const auto* gp = static_cast<const TypeGenericParam*>(type);
    auto it = subst.find(gp->index());
    if (it != subst.end()) {
      return it->second;
    }
    return type; // unbound — leave as-is (shouldn't happen in practice)
  }

  case TypeKind::Function: {
    const auto* fn = static_cast<const TypeFunction*>(type);
    std::vector<const Type*> params;
    params.reserve(fn->param_types().size());
    bool changed = false;
    for (const auto* param : fn->param_types()) {
      auto* sub = substitute_type(param, subst, types);
      if (sub != param) {
        changed = true;
      }
      params.push_back(sub);
    }
    auto* ret = substitute_type(fn->return_type(), subst, types);
    if (ret != fn->return_type()) {
      changed = true;
    }
    if (!changed) {
      return type;
    }
    return types.function_type(std::move(params), ret);
  }

  case TypeKind::Pointer: {
    const auto* ptr = static_cast<const TypePointer*>(type);
    auto* sub = substitute_type(ptr->pointee(), subst, types);
    if (sub == ptr->pointee()) {
      return type;
    }
    return types.pointer_to(sub);
  }

  case TypeKind::Generator: {
    const auto* gen = static_cast<const TypeGenerator*>(type);
    auto* sub = substitute_type(gen->yield_type(), subst, types);
    if (sub == gen->yield_type()) {
      return type;
    }
    return types.generator_type(sub);
  }

  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(type);
    bool changed = false;
    std::vector<StructField> new_fields;
    new_fields.reserve(st->fields().size());
    for (const auto& field : st->fields()) {
      auto* sub = substitute_type(field.type, subst, types);
      if (sub != field.type) {
        changed = true;
      }
      new_fields.push_back({field.name, sub});
    }
    if (!changed) {
      return type;
    }
    return types.make_struct(st->decl_id(), st->name(),
                             std::move(new_fields));
  }

  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    bool changed = false;
    std::vector<EnumVariant> new_variants;
    new_variants.reserve(en->variants().size());
    for (const auto& variant : en->variants()) {
      std::vector<const Type*> new_payload;
      new_payload.reserve(variant.payload_types.size());
      for (const auto* pt : variant.payload_types) {
        auto* sub = substitute_type(pt, subst, types);
        if (sub != pt) {
          changed = true;
        }
        new_payload.push_back(sub);
      }
      new_variants.push_back({variant.name, std::move(new_payload)});
    }
    // END DEBUG
    if (!changed) {
      return type;
    }
    return types.make_enum(en->decl_id(), en->name(),
                           std::move(new_variants));
  }

  default:
    return type;
  }
}

// ---------------------------------------------------------------------------
// Detect generic functions: any function whose locals, return type,
// or instruction types contain TypeGenericParam.
// ---------------------------------------------------------------------------

// Recursive walk with a visited set to tolerate cyclic types such as
// `class Node { next: *Node }` where following Struct fields through
// a Pointer pointee returns to the same Struct.  Without the visited
// set this would stack-overflow.
auto type_has_generic_impl(const Type* type,
                           std::unordered_set<const Type*>& visited)
    -> bool {
  if (type == nullptr) {
    return false;
  }
  if (!visited.insert(type).second) {
    return false; // already walked this node; cycle — assume no generic here
  }
  switch (type->kind()) {
  case TypeKind::GenericParam:
    return true;
  case TypeKind::Function: {
    const auto* fn = static_cast<const TypeFunction*>(type);
    for (const auto* param : fn->param_types()) {
      if (type_has_generic_impl(param, visited)) {
        return true;
      }
    }
    return type_has_generic_impl(fn->return_type(), visited);
  }
  case TypeKind::Pointer:
    return type_has_generic_impl(
        static_cast<const TypePointer*>(type)->pointee(), visited);
  case TypeKind::Generator:
    return type_has_generic_impl(
        static_cast<const TypeGenerator*>(type)->yield_type(), visited);
  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(type);
    for (const auto& field : st->fields()) {
      if (type_has_generic_impl(field.type, visited)) {
        return true;
      }
    }
    return false;
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    for (const auto& variant : en->variants()) {
      for (const auto* pt : variant.payload_types) {
        if (type_has_generic_impl(pt, visited)) {
          return true;
        }
      }
    }
    return false;
  }
  default:
    return false;
  }
}

auto type_has_generic(const Type* type) -> bool {
  std::unordered_set<const Type*> visited;
  return type_has_generic_impl(type, visited);
}

auto is_generic_function(const MirFunction* fn) -> bool {
  if (type_has_generic(fn->return_type)) {
    return true;
  }
  for (const auto& local : fn->locals) {
    if (type_has_generic(local.type)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Mangled name for specializations: "name$i32" or "name$i32_f64".
// ---------------------------------------------------------------------------

auto mangle_name(std::string_view base,
                 const std::vector<const Type*>& type_args) -> std::string {
  std::string result(base);
  result += '$';
  for (size_t i = 0; i < type_args.size(); ++i) {
    if (i > 0) {
      result += '_';
    }
    result += print_type(type_args[i]);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Specialization key for deduplication.
// ---------------------------------------------------------------------------

struct SpecKey {
  const MirFunction* generic_fn;
  std::vector<const Type*> type_args;

  auto operator==(const SpecKey& other) const -> bool {
    return generic_fn == other.generic_fn && type_args == other.type_args;
  }
};

struct SpecKeyHash {
  auto operator()(const SpecKey& key) const -> size_t {
    size_t h = std::hash<const void*>{}(key.generic_fn);
    for (const auto* type : key.type_args) {
      h ^= std::hash<const void*>{}(type) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
  }
};

// ---------------------------------------------------------------------------
// Clone a MirFunction with type substitution.
// ---------------------------------------------------------------------------

auto clone_function(const MirFunction* src, const TypeSubst& subst,
                    const Symbol* new_symbol, MirContext& ctx,
                    TypeContext& types) -> MirFunction* {
  auto* dst = ctx.alloc<MirFunction>();
  dst->symbol = new_symbol;
  dst->return_type = substitute_type(src->return_type, subst, types);
  dst->span = src->span;
  dst->is_extern = src->is_extern;

  // Clone locals with substituted types.
  dst->locals.reserve(src->locals.size());
  for (const auto& local : src->locals) {
    MirLocal cloned = local;
    cloned.type = substitute_type(local.type, subst, types);
    dst->locals.push_back(cloned);
  }

  // Clone blocks and instructions.
  dst->blocks.reserve(src->blocks.size());
  for (const auto* src_block : src->blocks) {
    auto* dst_block = ctx.alloc<MirBlock>();
    dst_block->id = src_block->id;

    dst_block->insts.reserve(src_block->insts.size());
    for (const auto* src_inst : src_block->insts) {
      auto* dst_inst = ctx.alloc<MirInst>();
      dst_inst->result = src_inst->result;
      dst_inst->type = substitute_type(src_inst->type, subst, types);
      dst_inst->span = src_inst->span;

      // Deep-copy payload, handling heap-allocated members.
      dst_inst->payload = src_inst->payload;

      // For Call and Construct, the args/field_values vectors need
      // to be cloned since they're heap-allocated.
      if (auto* call = std::get_if<MirCall>(&dst_inst->payload)) {
        if (call->args != nullptr) {
          auto* new_args = ctx.alloc<std::vector<MirValueId>>(*call->args);
          call->args = new_args;
        }
        if (call->explicit_type_args != nullptr) {
          auto* new_ta = ctx.alloc<std::vector<const Type*>>();
          new_ta->reserve(call->explicit_type_args->size());
          for (const auto* ta : *call->explicit_type_args) {
            new_ta->push_back(substitute_type(ta, subst, types));
          }
          call->explicit_type_args = new_ta;
        }
      } else if (auto* ctor = std::get_if<MirConstruct>(&dst_inst->payload)) {
        if (ctor->struct_type != nullptr) {
          const auto* sub_st =
              substitute_type(ctor->struct_type, subst, types);
          if (sub_st != nullptr && sub_st->kind() == TypeKind::Struct) {
            ctor->struct_type = static_cast<const TypeStruct*>(sub_st);
          }
        }
        if (ctor->field_values != nullptr) {
          auto* new_fv =
              ctx.alloc<std::vector<MirValueId>>(*ctor->field_values);
          ctor->field_values = new_fv;
        }
      } else if (auto* econ =
                     std::get_if<MirEnumConstruct>(&dst_inst->payload)) {
        if (econ->enum_type != nullptr) {
          const auto* sub_et =
              substitute_type(econ->enum_type, subst, types);
          if (sub_et != nullptr && sub_et->kind() == TypeKind::Enum) {
            econ->enum_type = static_cast<const TypeEnum*>(sub_et);
          }
        }
        if (econ->payload_values != nullptr) {
          auto* new_pv =
              ctx.alloc<std::vector<MirValueId>>(*econ->payload_values);
          econ->payload_values = new_pv;
        }
      } else if (auto* store = std::get_if<MirStore>(&dst_inst->payload)) {
        if (store->place != nullptr) {
          store->place = ctx.alloc<MirPlace>(*store->place);
        }
      } else if (auto* load = std::get_if<MirLoad>(&dst_inst->payload)) {
        if (load->place != nullptr) {
          load->place = ctx.alloc<MirPlace>(*load->place);
        }
      } else if (auto* addr = std::get_if<MirAddrOf>(&dst_inst->payload)) {
        if (addr->place != nullptr) {
          addr->place = ctx.alloc<MirPlace>(*addr->place);
        }
      }

      dst_block->insts.push_back(dst_inst);
    }

    dst->blocks.push_back(dst_block);
  }

  return dst;
}

// ---------------------------------------------------------------------------
// Post-clone fixup: resolve method calls on non-struct types.
//
// After monomorphization substitutes T → i32, a MirFieldAccess on an
// i32-typed value (e.g. x.to_string) should become a MirFnRef to the
// extend method "i32.to_string". This scans for FieldAccess instructions
// whose object type is not a struct and replaces them with FnRef to the
// mangled extend method, looking it up in the module's function list.
// ---------------------------------------------------------------------------

void fixup_method_calls(MirFunction* fn, const MirModule& module,
                        MirContext& ctx, TypeContext& types) {
  // Build a name → (symbol, function) lookup from the module's functions.
  struct FnEntry {
    const Symbol* symbol;
    const MirFunction* mir_fn;
  };
  std::unordered_map<std::string, FnEntry> fn_by_name;
  for (const auto* mod_fn : module.functions) {
    if (mod_fn->symbol != nullptr) {
      fn_by_name[std::string(mod_fn->symbol->name)] = {mod_fn->symbol, mod_fn};
    }
  }

  // Build value-type index: MirValueId.id → Type* for O(1) lookups.
  std::unordered_map<uint32_t, const Type*> value_types;
  for (const auto* blk : fn->blocks) {
    for (const auto* inst : blk->insts) {
      if (inst->result.valid() && inst->type != nullptr) {
        value_types[inst->result.id] = inst->type;
      }
    }
  }

  // Build callee-use index: FnRef result id → list of MirCall instructions.
  std::unordered_map<uint32_t, std::vector<MirInst*>> callee_uses;
  for (auto* blk : fn->blocks) {
    for (auto* inst : blk->insts) {
      auto* call = std::get_if<MirCall>(&inst->payload);
      if (call != nullptr) {
        callee_uses[call->callee.id].push_back(inst);
      }
    }
  }

  for (auto* block : fn->blocks) {
    for (auto* inst : block->insts) {
      auto* field = std::get_if<MirFieldAccess>(&inst->payload);
      if (field == nullptr) {
        continue;
      }

      // Look up the type of the object being accessed.
      const Type* obj_type = nullptr;
      auto vt_it = value_types.find(field->object.id);
      if (vt_it != value_types.end()) {
        obj_type = vt_it->second;
      }

      // Skip struct types — those are real field accesses.
      if (obj_type == nullptr || obj_type->kind() == TypeKind::Struct) {
        continue;
      }

      // Build mangled method name: "<type>.<field>".
      auto method_name =
          print_type(obj_type) + "." + std::string(field->field);
      auto sym_it = fn_by_name.find(method_name);
      if (sym_it == fn_by_name.end()) {
        continue;
      }

      // Save the object value before replacing the payload.
      auto object_val = field->object;
      const auto& entry = sym_it->second;

      // Replace FieldAccess with FnRef to the extend method.
      // Update the instruction type to the method's function type,
      // matching what MirBuilder would produce for a direct FnRef.
      inst->payload = MirFnRef{entry.symbol};

      // Build the function type from the extend method's MIR signature.
      std::vector<const Type*> param_types;
      for (const auto& local : entry.mir_fn->locals) {
        if (!local.is_param) { break; }
        param_types.push_back(local.type);
      }
      inst->type = types.function_type(
          std::move(param_types), entry.mir_fn->return_type);

      // Find the MirCall(s) that use this instruction's result as callee
      // and prepend the object as the first argument (self).
      auto use_it = callee_uses.find(inst->result.id);
      if (use_it != callee_uses.end()) {
        for (auto* call_inst : use_it->second) {
          auto* call = std::get_if<MirCall>(&call_inst->payload);
          if (call == nullptr) {
            continue;
          }
          if (call->args == nullptr) {
            call->args = ctx.alloc<std::vector<MirValueId>>();
          }
          call->args->insert(call->args->begin(), object_val);
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Infer type substitution from a call site.
// ---------------------------------------------------------------------------

// Recursively extract generic param bindings by matching a pattern type
// (which may contain TypeGenericParam) against a concrete type.
void infer_bindings_recursive(const Type* pattern, const Type* concrete,
                              TypeSubst& subst) {
  if (pattern == nullptr || concrete == nullptr) {
    return;
  }

  if (pattern->kind() == TypeKind::GenericParam) {
    const auto* gp = static_cast<const TypeGenericParam*>(pattern);
    subst[gp->index()] = concrete; // last-write-wins at MIR level
    return;
  }

  if (pattern->kind() == TypeKind::Pointer &&
      concrete->kind() == TypeKind::Pointer) {
    infer_bindings_recursive(
        static_cast<const TypePointer*>(pattern)->pointee(),
        static_cast<const TypePointer*>(concrete)->pointee(), subst);
  } else if (pattern->kind() == TypeKind::Generator &&
             concrete->kind() == TypeKind::Generator) {
    infer_bindings_recursive(
        static_cast<const TypeGenerator*>(pattern)->yield_type(),
        static_cast<const TypeGenerator*>(concrete)->yield_type(), subst);
  } else if (pattern->kind() == TypeKind::Function &&
             concrete->kind() == TypeKind::Function) {
    const auto* fp = static_cast<const TypeFunction*>(pattern);
    const auto* fc = static_cast<const TypeFunction*>(concrete);
    if (fp->param_types().size() == fc->param_types().size()) {
      for (size_t i = 0; i < fp->param_types().size(); ++i) {
        infer_bindings_recursive(fp->param_types()[i],
                                 fc->param_types()[i], subst);
      }
      infer_bindings_recursive(fp->return_type(), fc->return_type(), subst);
    }
  } else if (pattern->kind() == TypeKind::Struct &&
             concrete->kind() == TypeKind::Struct) {
    const auto* sp = static_cast<const TypeStruct*>(pattern);
    const auto* sc = static_cast<const TypeStruct*>(concrete);
    if (sp->fields().size() == sc->fields().size()) {
      for (size_t i = 0; i < sp->fields().size(); ++i) {
        infer_bindings_recursive(sp->fields()[i].type,
                                 sc->fields()[i].type, subst);
      }
    }
  } else if (pattern->kind() == TypeKind::Enum &&
             concrete->kind() == TypeKind::Enum) {
    const auto* ep = static_cast<const TypeEnum*>(pattern);
    const auto* ec = static_cast<const TypeEnum*>(concrete);
    if (ep->variants().size() == ec->variants().size()) {
      for (size_t vi = 0; vi < ep->variants().size(); ++vi) {
        const auto& vp = ep->variants()[vi];
        const auto& vc = ec->variants()[vi];
        if (vp.payload_types.size() == vc.payload_types.size()) {
          for (size_t pi = 0; pi < vp.payload_types.size(); ++pi) {
            infer_bindings_recursive(vp.payload_types[pi],
                                     vc.payload_types[pi], subst);
          }
        }
      }
    }
  }
}

auto infer_substitution(const MirFunction* generic_fn,
                        const std::vector<const Type*>& arg_types)
    -> TypeSubst {
  TypeSubst subst;
  size_t param_count = 0;
  for (const auto& local : generic_fn->locals) {
    if (!local.is_param) {
      break;
    }
    if (param_count < arg_types.size() && local.type != nullptr) {
      infer_bindings_recursive(local.type, arg_types[param_count], subst);
    }
    ++param_count;
  }
  return subst;
}

// Extract ordered type args from a substitution map.
auto subst_to_type_args(const TypeSubst& subst) -> std::vector<const Type*> {
  if (subst.empty()) {
    return {};
  }
  uint32_t max_idx = 0;
  for (const auto& [idx, _] : subst) {
    if (idx > max_idx) {
      max_idx = idx;
    }
  }
  std::vector<const Type*> args(max_idx + 1, nullptr);
  for (const auto& [idx, type] : subst) {
    args[idx] = type;
  }
  return args;
}

// ---------------------------------------------------------------------------
// Build a value-type index for a function: MirValueId.id → Type*.
// ---------------------------------------------------------------------------

auto build_value_types(const MirFunction* fn)
    -> std::unordered_map<uint32_t, const Type*> {
  std::unordered_map<uint32_t, const Type*> value_types;
  for (const auto* blk : fn->blocks) {
    for (const auto* inst : blk->insts) {
      if (inst->result.valid() && inst->type != nullptr) {
        value_types[inst->result.id] = inst->type;
      }
    }
  }
  return value_types;
}

// ---------------------------------------------------------------------------
// Process a single call site that references a generic function.
// Returns true if a new specialization was created.
// ---------------------------------------------------------------------------

auto specialize_call_site(
    MirInst* inst, MirFnRef* fn_ref, const MirBlock* block,
    size_t inst_idx,
    const std::unordered_map<uint32_t, const Type*>& value_types,
    const std::unordered_map<const Symbol*, MirFunction*>& generic_fns,
    std::unordered_map<SpecKey, MirFunction*, SpecKeyHash>& spec_cache,
    MirModule& module, MirContext& ctx, TypeContext& types) -> bool {

  auto git = generic_fns.find(fn_ref->symbol);
  if (git == generic_fns.end()) {
    return false;
  }

  // Find the matching MirCall instruction that uses this FnRef.
  const MirCall* call_payload = nullptr;
  for (size_t j = inst_idx + 1; j < block->insts.size(); ++j) {
    auto* candidate = std::get_if<MirCall>(&block->insts[j]->payload);
    if (candidate != nullptr &&
        candidate->callee.id == inst->result.id) {
      call_payload = candidate;
      break;
    }
  }

  if (call_payload == nullptr || call_payload->args == nullptr) {
    return false;
  }

  // Collect argument types from the value-type index.
  std::vector<const Type*> arg_types;
  arg_types.reserve(call_payload->args->size());
  for (auto arg_id : *call_payload->args) {
    auto vt_it = value_types.find(arg_id.id);
    arg_types.push_back(vt_it != value_types.end() ? vt_it->second
                                                    : nullptr);
  }

  // Infer substitution from argument types.
  auto subst = infer_substitution(git->second, arg_types);

  // If inference failed (e.g. zero-arg builtin), use explicit type args.
  if (subst.empty() && call_payload->explicit_type_args != nullptr &&
      !call_payload->explicit_type_args->empty()) {
    for (size_t i = 0; i < call_payload->explicit_type_args->size(); ++i) {
      subst[static_cast<uint32_t>(i)] =
          (*call_payload->explicit_type_args)[i];
    }
  }

  if (subst.empty()) {
    return false;
  }

  auto type_args = subst_to_type_args(subst);
  SpecKey key{git->second, type_args};

  // Check cache.
  auto cache_it = spec_cache.find(key);
  if (cache_it != spec_cache.end()) {
    // Rewrite the FnRef to point at the cached specialization.
    fn_ref->symbol = cache_it->second->symbol;
    inst->type = substitute_type(inst->type, subst, types);
    return false; // no new specialization
  }

  // Create mangled symbol.
  // Allocate name string on arena so it outlives this scope.
  auto* name_str = ctx.alloc<std::string>(
      mangle_name(git->second->symbol->name, type_args));

  auto* new_sym = ctx.alloc<Symbol>();
  new_sym->kind = SymbolKind::Function;
  new_sym->name = std::string_view(*name_str);
  new_sym->decl_span = git->second->symbol->decl_span;
  new_sym->decl = git->second->symbol->decl;
  new_sym->module = git->second->symbol->module; // an instantiation belongs to its template's module

  // Clone the generic function with type substitution.
  auto* specialized =
      clone_function(git->second, subst, new_sym, ctx, types);

  // Resolve method calls on newly-concrete types.
  fixup_method_calls(specialized, module, ctx, types);

  // Register in cache and add to module.
  spec_cache[key] = specialized;
  module.functions.push_back(specialized);

  // Rewrite the FnRef.
  fn_ref->symbol = new_sym;
  inst->type = substitute_type(inst->type, subst, types);

  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Main pass
// ---------------------------------------------------------------------------

auto monomorphize(
    MirModule& module, MirContext& ctx, TypeContext& types,
    const std::unordered_map<const Symbol*, MirFunction*>& generic_templates)
    -> MonomorphizeResult {
  MonomorphizeResult result;

  // Phase 1: use the provided generic templates directly.
  // The MIR builder already separated generic function bodies into
  // templates (not in module.functions).  No scanning needed.
  //
  // When generic_templates is empty, skip specialization but still
  // run the concreteness invariant check below — the check must fire
  // even for programs with no generics, to catch synthetic residue.
  const bool has_templates = !generic_templates.empty();

  // Specialization cache: (generic fn, type args) → specialized fn.
  std::unordered_map<SpecKey, MirFunction*, SpecKeyHash> spec_cache;

  // Phase 2+3+4: iterate until no new specializations are produced.
  // Use index-based iteration because specialize_call_site() may
  // push_back new functions, invalidating range-for iterators.
  bool changed = has_templates;
  while (changed) {
    changed = false;

    const size_t fn_count = module.functions.size();
    for (size_t fn_idx = 0; fn_idx < fn_count; ++fn_idx) {
      auto* fn = module.functions[fn_idx];

      // Build per-function value-type index for O(1) arg type lookups.
      auto value_types = build_value_types(fn);

      for (auto* block : fn->blocks) {
        for (size_t inst_idx = 0; inst_idx < block->insts.size();
             ++inst_idx) {
          auto* inst = block->insts[inst_idx];
          auto* fn_ref = std::get_if<MirFnRef>(&inst->payload);
          if (fn_ref == nullptr) {
            continue;
          }

          if (specialize_call_site(inst, fn_ref, block, inst_idx,
                                   value_types, generic_templates,
                                   spec_cache, module, ctx, types)) {
            changed = true;
          }
        }
      }
    }
  }

  // No Phase 5 needed — generic originals were never in
  // module.functions (they live only in generic_templates).

  // MIR concreteness invariant (Task 28 §8.1, §14.2):
  //
  // After monomorphization, every function in module.functions must
  // be type-concrete.  No TypeGenericParam residue may remain in:
  //   - function return types
  //   - local variable types (including parameters)
  //   - per-instruction result types
  //
  // If this invariant is violated, the LLVM backend will abort on
  // unsized types later (`getTypeInfo() on unsized type`) with a
  // backtrace deep inside LLVM, making regressions hard to diagnose.
  // Emit an explicit internal-error diagnostic at the MIR boundary
  // instead so the regression is localized to the offending function.
  for (const auto* fn : module.functions) {
    if (is_generic_function(fn)) {
      std::string fn_name = fn->symbol != nullptr
                                ? std::string(fn->symbol->name)
                                : std::string("<anonymous>");
      result.diagnostics.push_back(Diagnostic::error(
          fn->span,
          "internal: MIR concreteness invariant violated — function '" +
              fn_name +
              "' contains unresolved generic parameter types after "
              "monomorphization"));
      continue;
    }
    // Also scan instruction types: is_generic_function() checks the
    // signature and locals, but an instruction may produce a value of
    // generic type even when locals are concrete (e.g. a call to a
    // generic function not yet monomorphized).
    for (const auto* block : fn->blocks) {
      for (const auto* inst : block->insts) {
        if (type_has_generic(inst->type)) {
          std::string fn_name = fn->symbol != nullptr
                                    ? std::string(fn->symbol->name)
                                    : std::string("<anonymous>");
          result.diagnostics.push_back(Diagnostic::error(
              inst->span,
              "internal: MIR concreteness invariant violated — "
              "instruction in '" +
                  fn_name +
                  "' produces a value with unresolved generic "
                  "parameter type after monomorphization"));
          break;
        }
      }
    }
  }

  return result;
}

} // namespace dao
// NOLINTEND(readability-magic-numbers,readability-identifier-length)
