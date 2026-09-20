// NOLINTBEGIN(readability-magic-numbers,readability-identifier-length)
#include "ir/mir/mir_monomorphize.h"

#include "frontend/types/type_query.h"

#include "frontend/ast/ast.h"
#include "frontend/types/type_identity.h"

#include "frontend/module/program.h"
#include "frontend/module/source_map.h"
#include "frontend/types/type.h"
#include "frontend/types/type_ownership.h"
#include "frontend/types/type_printer.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <format>
#include <unordered_set>
#include <vector>

namespace dao {

namespace {

// ---------------------------------------------------------------------------
// Type substitution: replace TypeGenericParam with concrete types.
// ---------------------------------------------------------------------------

using TypeSubst = std::unordered_map<ParamKey, const Type*, ParamKeyHash>;

/// Nominal instantiations this walk is building, by the generic type
/// they come from.  A class may reach itself through a pointer
/// (`class Node<T>: next: Ptr<Node<T>>`), so its instantiation is
/// registered before its fields are walked and the recursive occurrence
/// resolves to it.
using TypePair = std::pair<const Type*, const Type*>;

struct TypePairHash {
  auto operator()(const TypePair& pair) const -> size_t {
    const std::hash<const void*> hash;
    return hash(pair.first) ^ (hash(pair.second) << 1U);
  }
};

using Instantiating = std::unordered_map<const Type*, Type*>;

/// What a substitution has already answered, by the type asked about:
/// a class holding two pointers to the same inner type instantiates it
/// once, however many paths reach it.
using Answered = std::unordered_map<const Type*, const Type*>;

auto substitute_type(const Type* type, const TypeSubst& subst, TypeContext& types,
                     Instantiating& building, Answered& answered, GenericParamReach& reach)
    -> const Type*;

/// One substitution, and the only way in: the walk it starts owns both
/// what it is building and what it has answered.
auto substitute_type(const Type* type, const TypeSubst& subst,
                     TypeContext& types) -> const Type* {
  Instantiating building;
  Answered answered;
  // Which types hold a parameter at all, for this walk: a nesting of
  // classes would otherwise be walked from each level down, once per
  // level.  The answers go with the walk, since a shell's fields are
  // filled in as it finishes.
  GenericParamReach reach;
  return substitute_type(type, subst, types, building, answered, reach);
}

auto substitute_type(const Type* type, const TypeSubst& subst, TypeContext& types,
                     Instantiating& building, Answered& answered, GenericParamReach& reach)
    -> const Type* {
  if (type == nullptr) {
    return nullptr;
  }
  if (auto done = answered.find(type); done != answered.end()) {
    return done->second;
  }

  switch (type->kind()) {
  case TypeKind::GenericParam: {
    const auto* gp = static_cast<const TypeGenericParam*>(type);
    auto it = subst.find(param_key(gp));
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
      auto* sub = substitute_type(param, subst, types, building, answered, reach);
      if (sub != param) {
        changed = true;
      }
      params.push_back(sub);
    }
    auto* ret = substitute_type(fn->return_type(), subst, types, building, answered, reach);
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
    auto* sub = substitute_type(ptr->pointee(), subst, types, building, answered, reach);
    if (sub == ptr->pointee()) {
      return type;
    }
    return types.pointer_to(sub);
  }

  case TypeKind::Generator: {
    const auto* gen = static_cast<const TypeGenerator*>(type);
    auto* sub = substitute_type(gen->yield_type(), subst, types, building, answered, reach);
    if (sub == gen->yield_type()) {
      return type;
    }
    return types.generator_type(sub);
  }

  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(type);
    auto built = building.find(type);
    if (built != building.end()) {
      return built->second;
    }
    if (!reach.mentions(type)) {
      return type;
    }
    auto* shell = types.make_struct_shell(st->decl_id(), st->name());
    building.emplace(type, shell);
    std::vector<StructField> new_fields;
    new_fields.reserve(st->fields().size());
    for (const auto& field : st->fields()) {
      new_fields.push_back({field.name, substitute_type(field.type, subst, types, building, answered, reach)});
    }
    // The instantiation travels with the type: a parameter no field
    // mentions lives only there, and specialization is keyed by it.
    std::vector<const Type*> new_args;
    new_args.reserve(st->type_args().size());
    for (const auto* arg : st->type_args()) {
      new_args.push_back(substitute_type(arg, subst, types, building, answered, reach));
    }
    building.erase(type);
    shell->set_fields(std::move(new_fields));
    shell->set_type_args(std::move(new_args));
    answered.emplace(type, shell);
    return shell;
  }

  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    auto built = building.find(type);
    if (built != building.end()) {
      return built->second;
    }
    if (!reach.mentions(type)) {
      return type;
    }
    auto* shell = types.make_enum_shell(en->decl_id(), en->name());
    building.emplace(type, shell);
    std::vector<EnumVariant> new_variants;
    new_variants.reserve(en->variants().size());
    for (const auto& variant : en->variants()) {
      std::vector<const Type*> new_payload;
      new_payload.reserve(variant.payload_types.size());
      for (const auto* pt : variant.payload_types) {
        new_payload.push_back(substitute_type(pt, subst, types, building, answered, reach));
      }
      new_variants.push_back({variant.name, std::move(new_payload)});
    }
    std::vector<const Type*> new_args;
    new_args.reserve(en->type_args().size());
    for (const auto* arg : en->type_args()) {
      new_args.push_back(substitute_type(arg, subst, types, building, answered, reach));
    }
    building.erase(type);
    shell->set_variants(std::move(new_variants));
    shell->set_type_args(std::move(new_args));
    answered.emplace(type, shell);
    return shell;
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
auto is_generic_function(const MirFunction* fn) -> bool {
  if (type_mentions_generic_param(fn->return_type)) {
    return true;
  }
  for (const auto& local : fn->locals) {
    if (type_mentions_generic_param(local.type)) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Mangled name for specializations: "name$i32" or "name$i32_f64".
// ---------------------------------------------------------------------------

/// Spellings for the type arguments in a specialization's name.  Two
/// distinct types can print alike — a `Box` declared in each of two
/// modules, which §11 of the type-system contract keeps distinct, or
/// `Vector<i32>` and `Vector<f64>`, which both print as the bare name
/// — and one symbol for both would let a definition answer a call it
/// does not match.  The first type to claim a spelling keeps it and
/// later ones are numbered; claiming follows specialization order,
/// which the program's file order fixes (§8.4).
class MangledTypeNames {
public:
  auto mangle(std::string_view base, const std::vector<const Type*>& type_args) -> std::string {
    std::string result(base);
    result += '$';
    for (size_t i = 0; i < type_args.size(); ++i) {
      if (i > 0) {
        result += '_';
      }
      result += spelling_of(type_args[i]);
    }
    return result;
  }

private:
  auto spelling_of(const Type* type) -> const std::string& {
    // Keyed by what the type IS, not by where it sits: nominal types
    // are not interned, so `Box<i32>` written twice is two objects and
    // an address would number the second one as a different type.
    auto [it, inserted] = spelling_.try_emplace(type_identity_key(type), std::string{});
    if (inserted) {
      auto base = print_type_name(type);
      auto taken = claims_[base]++;
      it->second = taken == 0 ? base : base + "." + std::to_string(taken);
    }
    return it->second;
  }

  std::unordered_map<std::string, std::string> spelling_;
  std::unordered_map<std::string, size_t> claims_;
};

// ---------------------------------------------------------------------------
// Specialization key for deduplication.
// ---------------------------------------------------------------------------

/// One specialization: the template and what it was instantiated with.
/// The arguments are held as identity keys rather than as pointers,
/// since two occurrences of `Box<i32>` are two objects and would
/// otherwise specialize the same template twice.
struct SpecKey {
  const MirFunction* generic_fn;
  std::vector<std::string> type_args;

  auto operator==(const SpecKey& other) const -> bool {
    return generic_fn == other.generic_fn && type_args == other.type_args;
  }
};

struct SpecKeyHash {
  auto operator()(const SpecKey& key) const -> size_t {
    size_t h = std::hash<const void*>{}(key.generic_fn);
    for (const auto& arg : key.type_args) {
      h ^= std::hash<std::string>{}(arg) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }
    return h;
  }
};

/// What one monomorphization run accumulates: the specializations it
/// has already made, and the spellings their names give type arguments.
struct SpecializationState {
  std::unordered_map<SpecKey, MirFunction*, SpecKeyHash> cache;
  MangledTypeNames names;
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
        // The instruction's own type is what the construction produces —
        // `Box(made)` inside `fn mapped<U>` is a `Box<U>`, while the
        // payload carries the class as the declaration spells it.  Where
        // they differ the instruction's is the instantiation to build.
        if (dst_inst->type != nullptr && dst_inst->type->kind() == TypeKind::Struct) {
          ctor->struct_type = static_cast<const TypeStruct*>(dst_inst->type);
        } else if (ctor->struct_type != nullptr) {
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
  // Keyed by name, but a name is not unique across a program: two modules
  // may each extend `i32` with `local`, and one entry per name let a later
  // module's method answer for an earlier one's call.  Every candidate is
  // kept and the visible one is chosen per call site (§5).
  std::unordered_map<std::string, std::vector<FnEntry>> fn_by_name;
  for (const auto* mod_fn : module.functions) {
    if (mod_fn->symbol != nullptr) {
      fn_by_name[std::string(mod_fn->symbol->name)].push_back({mod_fn->symbol, mod_fn});
    }
  }
  // The module whose body is being rewritten: an `extend` of another
  // module is not in scope for it.
  const auto* from_module = fn->symbol == nullptr ? nullptr : fn->symbol->module;

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
          print_type_name(obj_type) + "." + std::string(field->field);
      auto sym_it = fn_by_name.find(method_name);
      if (sym_it == fn_by_name.end()) {
        continue;
      }
      // Innermost-first, as lookup is everywhere else
      // (CONTRACT_MODULE_SYSTEM.md §7.4): the module's own `extend`
      // shadows the prelude's.  HIR lists prelude functions before
      // module functions, so "first visible" chose the prelude's method
      // over a local one of the same name.
      const FnEntry* visible = nullptr;
      for (const auto& candidate : sym_it->second) {
        if (!extend_visible_from(candidate.symbol->module, from_module)) {
          continue;
        }
        if (candidate.symbol->module == from_module) {
          visible = &candidate;
          break;
        }
        if (visible == nullptr) {
          visible = &candidate;
        }
      }
      if (visible == nullptr) {
        continue;
      }

      // Save the object value before replacing the payload.
      auto object_val = field->object;
      const auto& entry = *visible;

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
/// Pairs the inference walk is already matching.  Both sides may reach
/// themselves (`class Node<T>: next: Ptr<Node<T>>` instantiated), so a
/// pair that comes back around has nothing further to say.
using Matching = std::unordered_set<TypePair, TypePairHash>;

void infer_bindings_recursive(const Type* pattern, const Type* concrete, TypeSubst& subst,
                              Matching& matching);

void infer_bindings_recursive(const Type* pattern, const Type* concrete, TypeSubst& subst) {
  Matching matching;
  infer_bindings_recursive(pattern, concrete, subst, matching);
}

void infer_bindings_recursive(const Type* pattern, const Type* concrete, TypeSubst& subst,
                              Matching& matching) {
  if (pattern == nullptr || concrete == nullptr) {
    return;
  }
  if (!matching.insert(TypePair{pattern, concrete}).second) {
    return;
  }

  if (pattern->kind() == TypeKind::GenericParam) {
    const auto* gp = static_cast<const TypeGenericParam*>(pattern);
    subst[param_key(gp)] = concrete; // last-write-wins at MIR level
    return;
  }

  if (pattern->kind() == TypeKind::Pointer &&
      concrete->kind() == TypeKind::Pointer) {
    infer_bindings_recursive(
        static_cast<const TypePointer*>(pattern)->pointee(),
        static_cast<const TypePointer*>(concrete)->pointee(), subst, matching);
  } else if (pattern->kind() == TypeKind::Generator &&
             concrete->kind() == TypeKind::Generator) {
    infer_bindings_recursive(
        static_cast<const TypeGenerator*>(pattern)->yield_type(),
        static_cast<const TypeGenerator*>(concrete)->yield_type(), subst, matching);
  } else if (pattern->kind() == TypeKind::Function &&
             concrete->kind() == TypeKind::Function) {
    const auto* fp = static_cast<const TypeFunction*>(pattern);
    const auto* fc = static_cast<const TypeFunction*>(concrete);
    if (fp->param_types().size() == fc->param_types().size()) {
      for (size_t i = 0; i < fp->param_types().size(); ++i) {
        infer_bindings_recursive(fp->param_types()[i],
                                 fc->param_types()[i], subst, matching);
      }
      infer_bindings_recursive(fp->return_type(), fc->return_type(), subst, matching);
    }
  } else if (pattern->kind() == TypeKind::Struct &&
             concrete->kind() == TypeKind::Struct) {
    const auto* sp = static_cast<const TypeStruct*>(pattern);
    const auto* sc = static_cast<const TypeStruct*>(concrete);
    // The instantiation first: a parameter no field mentions is bound
    // from there and nowhere else.
    if (sp->type_args().size() == sc->type_args().size()) {
      for (size_t i = 0; i < sp->type_args().size(); ++i) {
        infer_bindings_recursive(sp->type_args()[i], sc->type_args()[i], subst, matching);
      }
    }
    if (sp->fields().size() == sc->fields().size()) {
      for (size_t i = 0; i < sp->fields().size(); ++i) {
        infer_bindings_recursive(sp->fields()[i].type,
                                 sc->fields()[i].type, subst, matching);
      }
    }
  } else if (pattern->kind() == TypeKind::Enum &&
             concrete->kind() == TypeKind::Enum) {
    const auto* ep = static_cast<const TypeEnum*>(pattern);
    const auto* ec = static_cast<const TypeEnum*>(concrete);
    if (ep->type_args().size() == ec->type_args().size()) {
      for (size_t i = 0; i < ep->type_args().size(); ++i) {
        infer_bindings_recursive(ep->type_args()[i], ec->type_args()[i], subst, matching);
      }
    }
    if (ep->variants().size() == ec->variants().size()) {
      for (size_t vi = 0; vi < ep->variants().size(); ++vi) {
        const auto& vp = ep->variants()[vi];
        const auto& vc = ec->variants()[vi];
        if (vp.payload_types.size() == vc.payload_types.size()) {
          for (size_t pi = 0; pi < vp.payload_types.size(); ++pi) {
            infer_bindings_recursive(vp.payload_types[pi],
                                     vc.payload_types[pi], subst, matching);
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
/// Where a declaration sits in the source, which orders two parameters
/// at the same position — a class's before the method's inside it.  An
/// address would order them differently on every run.
auto declared_at(const Decl* decl) -> uint32_t {
  return decl == nullptr ? 0U : decl->span.offset;
}

/// The bindings in a settled order: by position, then by where the
/// declaring declaration sits, so a specialization is named and keyed
/// the same on every run — a hash map's order is not the program's.
auto ordered_bindings(const TypeSubst& subst)
    -> std::vector<std::pair<ParamKey, const Type*>> {
  std::vector<std::pair<ParamKey, const Type*>> bound(subst.begin(), subst.end());
  std::ranges::sort(bound, [](const auto& lhs, const auto& rhs) {
    if (lhs.first.index != rhs.first.index) {
      return lhs.first.index < rhs.first.index;
    }
    return declared_at(lhs.first.binder) < declared_at(rhs.first.binder);
  });
  return bound;
}

auto subst_to_type_args(const TypeSubst& subst) -> std::vector<const Type*> {
  std::vector<const Type*> args;
  args.reserve(subst.size());
  for (const auto& [key, type] : ordered_bindings(subst)) {
    args.push_back(type);
  }
  return args;
}

/// What a specialization IS: each parameter, by the declaration that
/// declares it, and the type it stands for.  Two calls that swap the
/// arguments of a class's and a method's parameters are two
/// specializations, though their argument lists read alike.
auto subst_identity(const TypeSubst& subst) -> std::vector<std::string> {
  std::vector<std::string> identity;
  identity.reserve(subst.size());
  for (const auto& [key, type] : ordered_bindings(subst)) {
    identity.push_back(
        std::format("{}:{}={}", declared_at(key.binder), key.index, type_identity_key(type)));
  }
  return identity;
}

/// The parameters a signature is written with, by position: what a
/// call's written-out type arguments fill.  A position a signature
/// mentions once is unambiguous; where a class's and a method's
/// parameters share one, the declaration named by `preferred` wins,
/// since those arguments were written for it.
void collect_params(const Type* type, const Decl* preferred,
                    std::unordered_map<uint32_t, const TypeGenericParam*>& found,
                    std::unordered_set<const Type*>& seen) {
  if (type == nullptr || !seen.insert(type).second) {
    return;
  }
  switch (type->kind()) {
  case TypeKind::GenericParam: {
    const auto* param = static_cast<const TypeGenericParam*>(type);
    auto [it, fresh] = found.try_emplace(param->index(), param);
    if (!fresh && param->binder() == preferred) {
      it->second = param;
    }
    break;
  }
  case TypeKind::Pointer:
    collect_params(static_cast<const TypePointer*>(type)->pointee(), preferred, found, seen);
    break;
  case TypeKind::Generator:
    collect_params(static_cast<const TypeGenerator*>(type)->yield_type(), preferred, found, seen);
    break;
  case TypeKind::Function: {
    const auto* fn = static_cast<const TypeFunction*>(type);
    for (const auto* param : fn->param_types()) {
      collect_params(param, preferred, found, seen);
    }
    collect_params(fn->return_type(), preferred, found, seen);
    break;
  }
  case TypeKind::Struct: {
    const auto* st = static_cast<const TypeStruct*>(type);
    for (const auto* arg : st->type_args()) {
      collect_params(arg, preferred, found, seen);
    }
    for (const auto& field : st->fields()) {
      collect_params(field.type, preferred, found, seen);
    }
    break;
  }
  case TypeKind::Enum: {
    const auto* en = static_cast<const TypeEnum*>(type);
    for (const auto* arg : en->type_args()) {
      collect_params(arg, preferred, found, seen);
    }
    for (const auto& variant : en->variants()) {
      for (const auto* payload : variant.payload_types) {
        collect_params(payload, preferred, found, seen);
      }
    }
    break;
  }
  default:
    break;
  }
}

/// The written-out type arguments of a call, bound to the parameters
/// they were written for.  A declaration that declares its own
/// parameters takes them by position, whether or not its signature
/// mentions them — `fn width<T>(): i64 -> size_of<T>()` uses `T` in its
/// body alone.  A method written with its class's parameters has none of
/// its own, and the signature says which they are.
auto explicit_substitution(const Type* signature, const Decl* declared_by,
                           const std::vector<const Type*>& type_args) -> TypeSubst {
  TypeSubst subst;
  if (declared_by != nullptr) {
    for (size_t i = 0; i < type_args.size(); ++i) {
      subst[ParamKey{declared_by, static_cast<uint32_t>(i)}] = type_args[i];
    }
    return subst;
  }
  std::unordered_map<uint32_t, const TypeGenericParam*> params;
  std::unordered_set<const Type*> seen;
  collect_params(signature, declared_by, params, seen);
  for (size_t i = 0; i < type_args.size(); ++i) {
    auto it = params.find(static_cast<uint32_t>(i));
    if (it != params.end()) {
      subst[param_key(it->second)] = type_args[i];
    }
  }
  return subst;
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

// The call each callee value feeds, function-wide: a reference and its
// call need not share a block (an argument containing `?` puts the
// call in the merge block that follows).
auto build_call_index(const MirFunction* fn) -> std::unordered_map<uint32_t, const MirCall*> {
  std::unordered_map<uint32_t, const MirCall*> calls_by_callee;
  for (const auto* blk : fn->blocks) {
    for (const auto* inst : blk->insts) {
      if (const auto* call = std::get_if<MirCall>(&inst->payload)) {
        calls_by_callee[call->callee.id] = call;
      }
    }
  }
  return calls_by_callee;
}

// ---------------------------------------------------------------------------
// Process a single call site that references a generic function.
// Returns true if a new specialization was created.
// ---------------------------------------------------------------------------

/// The declaration a call's written-out type arguments were written
/// for: the callee's own, when it declares type parameters.  A method
/// that declares none is written with its class's, and
/// `Vector<u8>::new()` fills those — so there is no declaration to
/// prefer and the signature's positions decide.
auto explicit_arg_binder(const MirFnRef* fn_ref, const MirCall* call) -> const Decl* {
  if (call != nullptr && call->type_args_binder != nullptr) {
    return call->type_args_binder; // as the checker resolved it
  }
  if (fn_ref == nullptr || fn_ref->symbol == nullptr || fn_ref->symbol->decl == nullptr) {
    return nullptr;
  }
  const auto* decl = fn_ref->symbol->decl_as_decl();
  if (!decl->is<FunctionDecl>() || decl->as<FunctionDecl>().type_params.empty()) {
    return nullptr;
  }
  return decl;
}

auto specialize_call_site(MirInst* inst,
                          MirFnRef* fn_ref,
                          const std::unordered_map<uint32_t, const MirCall*>& calls_by_callee,
                          const std::unordered_map<uint32_t, const Type*>& value_types,
                          const std::unordered_map<const Symbol*, MirFunction*>& generic_fns,
                          SpecializationState& state,
                          MirModule& module,
                          MirContext& ctx,
                          TypeContext& types) -> bool {

  // Only a template's reference, or one whose type still carries a
  // generic parameter (a compiler builtin's), has anything to do
  // here; an ordinary call is left alone without a scan for its site.
  auto git = generic_fns.find(fn_ref->symbol);
  const bool is_template = git != generic_fns.end();
  if (!is_template && !type_mentions_generic_param(inst->type)) {
    return false;
  }

  // The call this reference feeds, wherever in the function it sits.
  const MirCall* call_payload = nullptr;
  if (auto found = calls_by_callee.find(inst->result.id); found != calls_by_callee.end()) {
    call_payload = found->second;
  }

  if (!is_template) {
    // Not a template: a compiler intrinsic (`size_of<T>`, `align_of<T>`)
    // has no body to specialize, but its reference is typed with the
    // builtin's generic signature.  The call's explicit type arguments
    // make that type concrete; the backend keys the builtin on its name
    // and reads the types from the call, so nothing else is needed.
    if (call_payload != nullptr && call_payload->explicit_type_args != nullptr &&
        !call_payload->explicit_type_args->empty()) {
      auto subst = explicit_substitution(inst->type, explicit_arg_binder(fn_ref, call_payload),
                                         *call_payload->explicit_type_args);
      inst->type = substitute_type(inst->type, subst, types);
    }
    return false;
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
  if (call_payload->explicit_type_args != nullptr &&
      !call_payload->explicit_type_args->empty()) {
    // Written-out arguments decide their positions, whatever inference
    // made of the values.
    for (const auto& [key, type] :
         explicit_substitution(git->second->return_type, explicit_arg_binder(fn_ref, call_payload),
                               *call_payload->explicit_type_args)) {
      subst[key] = type;
    }
    for (const auto& local : git->second->locals) {
      if (!local.is_param) {
        break;
      }
      for (const auto& [key, type] :
           explicit_substitution(local.type, explicit_arg_binder(fn_ref, call_payload),
                                 *call_payload->explicit_type_args)) {
        subst.emplace(key, type);
      }
    }
  }

  if (subst.empty()) {
    return false;
  }

  auto type_args = subst_to_type_args(subst);
  SpecKey key{git->second, subst_identity(subst)};

  // Check cache.
  auto cache_it = state.cache.find(key);
  if (cache_it != state.cache.end()) {
    // Rewrite the FnRef to point at the cached specialization.
    fn_ref->symbol = cache_it->second->symbol;
    inst->type = substitute_type(inst->type, subst, types);
    return false; // no new specialization
  }

  // Create mangled symbol.
  // Allocate name string on arena so it outlives this scope.
  auto* name_str = ctx.alloc<std::string>(state.names.mangle(git->second->symbol->name, type_args));

  auto* new_sym = ctx.alloc<Symbol>();
  new_sym->kind = SymbolKind::Function;
  new_sym->name = std::string_view(*name_str);
  new_sym->decl_span = git->second->symbol->decl_span;
  new_sym->decl = git->second->symbol->decl;
  new_sym->module =
      git->second->symbol->module; // an instantiation belongs to its template's module

  // Clone the generic function with type substitution.
  auto* specialized =
      clone_function(git->second, subst, new_sym, ctx, types);

  // Resolve method calls on newly-concrete types.
  fixup_method_calls(specialized, module, ctx, types);

  // Register in cache and add to module.
  state.cache[key] = specialized;
  module.functions.push_back(specialized);

  // Rewrite the FnRef.
  fn_ref->symbol = new_sym;
  inst->type = substitute_type(inst->type, subst, types);

  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// copy_out expansion
// ---------------------------------------------------------------------------
//
// The prelude's `copy_out<T>(x)` is the identity.  A call to it whose T
// is concrete is expanded, before the call could be specialized, into
// what copies a T out of a `resource memory` domain into the enclosing
// one:
//   string                        -> copy_out_string(x), the prelude's
//   class with `copy_out(self)`   -> x.copy_out(), the class's own
//   any other class               -> the class constructed from copy_out<F>(x.f) per field
//   enum                          -> per variant, constructed from copy_out<P>(payload)
//   anything owning no heap memory -> left to specialize into the identity
// The calls an expansion introduces (a field's copy_out, a container's
// method) are expanded in the same sweep or specialized right after it,
// so the pass runs inside the fixpoint loop ahead of each function's
// specialization and reports whether it changed anything.

struct CopyOutHooks {
  const Decl* copy_out_decl = nullptr; // the prelude's copy_out; its specializations share it
  const Symbol* copy_out_template = nullptr;
  const MirFunction* copy_out_fn = nullptr; // the template, for its (generic) function type
  const Symbol* copy_out_string = nullptr;  // the prelude's copy_out_string
  const MirFunction* copy_out_string_fn = nullptr;
};

auto is_prelude_symbol(const Symbol* sym) -> bool {
  return sym != nullptr && sym->module != nullptr && sym->module->is_prelude;
}

auto find_copy_out_hooks(const MirModule& module,
                         const std::unordered_map<const Symbol*, MirFunction*>& generic_templates)
    -> CopyOutHooks {
  CopyOutHooks hooks;
  for (const auto& [sym, fn] : generic_templates) {
    if (sym->name == "copy_out" && is_prelude_symbol(sym)) {
      hooks.copy_out_template = sym;
      hooks.copy_out_decl = sym->decl_as_decl();
      hooks.copy_out_fn = fn;
    }
  }
  for (const auto* fn : module.functions) {
    if (fn->symbol != nullptr && fn->symbol->name == "copy_out_string" &&
        is_prelude_symbol(fn->symbol)) {
      hooks.copy_out_string = fn->symbol;
      hooks.copy_out_string_fn = fn;
    }
  }
  return hooks;
}

auto function_type_of(const MirFunction* fn, TypeContext& types) -> const Type* {
  std::vector<const Type*> params;
  for (const auto& local : fn->locals) {
    if (!local.is_param) {
      break;
    }
    params.push_back(local.type);
  }
  return types.function_type(std::move(params), fn->return_type);
}

// A class's own copier, decided from its declaration: the method named
// `copy_out`, with no type parameters of its own, that takes `self` and
// nothing else and returns the class's own type spelled as the class
// names itself, type parameters included (`Wrap<T>` for `class Wrap<T>`).
// Decided there rather than from MIR shapes: a static
// `copy_out(value: Label): Label` has one parameter but no receiver, and
// a phantom type parameter leaves `Tag<i32>` and `Tag<string>` with the
// same fields.
struct MethodRef {
  const Symbol* symbol = nullptr;
  const MirFunction* fn = nullptr;
};

auto names_only(const QualifiedPath& path, std::string_view name) -> bool {
  return path.segments.size() == 1 && path.segments[0] == name;
}

auto is_copier_shaped(const FunctionDecl& fn, const ClassDecl& cls) -> bool {
  if (fn.name != "copy_out" || !fn.type_params.empty() || fn.params.size() != 1 ||
      fn.params[0].name != "self" || fn.params[0].type != nullptr || fn.return_type == nullptr ||
      !fn.return_type->is<NamedType>()) {
    return false;
  }
  const auto& returned = fn.return_type->as<NamedType>();
  if (!names_only(returned.name, cls.name) || returned.type_args.size() != cls.type_params.size()) {
    return false;
  }
  for (size_t i = 0; i < returned.type_args.size(); ++i) {
    const auto* arg = returned.type_args[i];
    if (!arg->is<NamedType>() || !arg->as<NamedType>().type_args.empty() ||
        !names_only(arg->as<NamedType>().name, cls.type_params[i].name)) {
      return false;
    }
  }
  return true;
}

auto copier_among(const std::vector<Decl*>& methods, const ClassDecl& cls) -> const Decl* {
  for (const auto* method : methods) {
    if (is_copier_shaped(method->as<FunctionDecl>(), cls)) {
      return method;
    }
  }
  return nullptr;
}

auto find_copy_out_method(const TypeStruct* receiver,
                          const MirModule& module,
                          const std::unordered_map<const Symbol*, MirFunction*>& generic_templates,
                          Span span,
                          std::vector<Diagnostic>& diagnostics) -> MethodRef {
  const auto* class_decl = receiver->decl_id();
  if (class_decl == nullptr || class_decl->kind() != NodeKind::ClassDecl) {
    return {};
  }
  const auto& cls = class_decl->as<ClassDecl>();
  const auto* copier = copier_among(cls.methods, cls);
  if (copier == nullptr) {
    // A conformance block's methods are lowered without a symbol (not
    // lowerable to native code yet), so a copier declared only there
    // cannot be called; say so rather than copy field by field as if
    // the class had declared none.
    for (const auto& conformance : cls.conformances) {
      if (copier_among(conformance.methods, cls) != nullptr) {
        diagnostics.push_back(Diagnostic::error(
            span,
            "'" + std::string(cls.name) +
                "' declares its copy_out inside a conformance block, which cannot be "
                "called yet: declare it directly in the class"));
        return {};
      }
    }
    return {};
  }
  // The template first: a specialization already made for another
  // receiver (`Vector.copy_out$string` when `Vector<i64>` is being
  // copied) shares the declaration and must not answer for it; the
  // template is specialized for this receiver by the call's own type
  // arguments.
  for (const auto& [sym, fn] : generic_templates) {
    if (sym != nullptr && sym->decl_as_decl() == copier) {
      return {sym, fn};
    }
  }
  for (const auto* fn : module.functions) {
    if (fn->symbol != nullptr && fn->symbol->decl_as_decl() == copier) {
      return {fn->symbol, fn};
    }
  }
  return {};
}

// Fresh value and block ids for instructions the expansion adds.
struct FunctionIds {
  uint32_t next_value = 0;
  uint32_t next_block = 0;
  explicit FunctionIds(const MirFunction* fn) {
    for (const auto* blk : fn->blocks) {
      next_block = std::max(next_block, blk->id.id + 1);
      for (const auto* inst : blk->insts) {
        if (inst->result.valid()) {
          next_value = std::max(next_value, inst->result.id + 1);
        }
      }
    }
  }
  auto value() -> MirValueId {
    return MirValueId{next_value++};
  }
  auto block() -> BlockId {
    return BlockId{next_block++};
  }
};

auto make_inst(MirContext& ctx, MirValueId result, const Type* type, Span span, MirPayload payload)
    -> MirInst* {
  auto* inst = ctx.alloc<MirInst>();
  inst->result = result;
  inst->type = type;
  inst->span = span;
  inst->payload = std::move(payload);
  return inst;
}

// `copy_out<T>(value)` as fresh instructions, or `value` itself when its
// type owns nothing.  Appended to `out`.
auto emit_copy_of(MirValueId value,
                  const Type* type,
                  Span span,
                  const CopyOutHooks& hooks,
                  FunctionIds& ids,
                  MirContext& ctx,
                  TypeContext& types,
                  std::vector<MirInst*>& out) -> MirValueId {
  if (type == nullptr || !owns_heap_memory(type)) {
    return value;
  }
  auto callee = ids.value();
  out.push_back(make_inst(ctx,
                          callee,
                          function_type_of(hooks.copy_out_fn, types),
                          span,
                          MirFnRef{hooks.copy_out_template}));
  auto* args = ctx.alloc<std::vector<MirValueId>>();
  args->push_back(value);
  auto* type_args = ctx.alloc<std::vector<const Type*>>();
  type_args->push_back(type);
  auto result = ids.value();
  out.push_back(make_inst(ctx, result, type, span, MirCall{callee, args, type_args}));
  return result;
}

// Drop the `fn_ref` that fed a call whose payload no longer calls
// anything, so the expansion leaves no dead instruction behind.  The
// builder and the expansion place the `fn_ref` just before its call.
auto erase_callee_ref(MirBlock* block, size_t call_index, const MirInst* callee_ref) -> bool {
  for (size_t i = call_index; i > 0; --i) {
    if (block->insts[i - 1] == callee_ref) {
      block->insts.erase(block->insts.begin() + static_cast<std::ptrdiff_t>(i - 1));
      return true;
    }
  }
  return false;
}

// Expand every `copy_out` call in `fn` whose T is concrete and owns heap
// memory.  Returns true if anything was rewritten.
auto expand_copy_out_calls(MirFunction* fn,
                           const MirModule& module,
                           const std::unordered_map<const Symbol*, MirFunction*>& generic_templates,
                           const CopyOutHooks& hooks,
                           MirContext& ctx,
                           TypeContext& types,
                           std::vector<Diagnostic>& diagnostics) -> bool {
  if (hooks.copy_out_decl == nullptr) {
    return false;
  }
  // The fn_ref each value id was defined by, for finding a call's callee.
  std::unordered_map<uint32_t, MirInst*> defining;
  for (auto* blk : fn->blocks) {
    for (auto* inst : blk->insts) {
      if (inst->result.valid()) {
        defining[inst->result.id] = inst;
      }
    }
  }
  FunctionIds ids(fn);
  bool changed = false;

  for (size_t bi = 0; bi < fn->blocks.size(); ++bi) {
    auto* block = fn->blocks[bi];
    for (size_t k = 0; k < block->insts.size(); ++k) {
      auto* inst = block->insts[k];
      auto* call = std::get_if<MirCall>(&inst->payload);
      if (call == nullptr || call->args == nullptr || call->args->size() != 1) {
        continue;
      }
      auto def = defining.find(call->callee.id);
      if (def == defining.end()) {
        continue;
      }
      auto* callee_ref = std::get_if<MirFnRef>(&def->second->payload);
      if (callee_ref == nullptr || callee_ref->symbol != hooks.copy_out_template) {
        continue;
      }
      const Type* type = inst->type;
      if (type == nullptr || type_mentions_generic_param(type) || !owns_heap_memory(type)) {
        continue;
      }
      // A generator has no copier (its frame layout is the generator's
      // own); one reached here came through a container's element type,
      // past the checker's rule, and is rejected rather than aliased.
      if (holds_generator(type)) {
        diagnostics.push_back(
            Diagnostic::error(inst->span,
                              "a generator cannot be copied out of a resource block: a '" +
                                  print_type(type) + "' leaves one through a container"));
        continue;
      }
      MirValueId value = (*call->args)[0];
      Span span = inst->span;

      if (type->kind() == TypeKind::Named) {
        // A string: the prelude copies it into the enclosing domain.
        if (hooks.copy_out_string == nullptr) {
          continue; // reported once, by monomorphize()
        }
        def->second->payload = MirFnRef{hooks.copy_out_string};
        def->second->type = function_type_of(hooks.copy_out_string_fn, types);
        call->explicit_type_args = nullptr;
        changed = true;
        continue;
      }

      if (type->kind() == TypeKind::Struct) {
        const auto* st = static_cast<const TypeStruct*>(type);
        auto method = find_copy_out_method(st, module, generic_templates, span, diagnostics);
        if (method.symbol != nullptr) {
          // The class copies itself; the receiver is the one argument.
          def->second->payload = MirFnRef{method.symbol};
          def->second->type = function_type_of(method.fn, types);
          call->explicit_type_args = nullptr;
          changed = true;
          continue;
        }
        // Field by field: each owning field through its own copy_out.
        std::vector<MirInst*> added;
        auto* field_values = ctx.alloc<std::vector<MirValueId>>();
        uint32_t index = 0;
        for (const auto& field : st->fields()) {
          auto fv = ids.value();
          added.push_back(
              make_inst(ctx, fv, field.type, span, MirFieldAccess{value, field.name, index}));
          field_values->push_back(
              emit_copy_of(fv, field.type, span, hooks, ids, ctx, types, added));
          ++index;
        }
        inst->payload = MirConstruct{st, field_values};
        if (erase_callee_ref(block, k, def->second)) {
          --k;
        }
        // Inserted ahead of the construct; the sweep resumes at the
        // second inserted instruction (the first is a field access, never
        // a call), so a field's own copy_out call is expanded when reached.
        block->insts.insert(
            block->insts.begin() + static_cast<std::ptrdiff_t>(k), added.begin(), added.end());
        for (auto* a : added) {
          defining[a->result.id] = a;
        }
        changed = true;
        continue;
      }

      if (type->kind() == TypeKind::Enum) {
        // Per variant: the block splits at the call; each variant's block
        // rebuilds the value from copied payloads into a temporary, and
        // the merge block reads it back under the call's own result id.
        const auto* en = static_cast<const TypeEnum*>(type);
        if (erase_callee_ref(block, k, def->second)) {
          --k;
        }
        LocalId temp{static_cast<uint32_t>(fn->locals.size())};
        fn->locals.push_back(
            {.id = temp, .symbol = nullptr, .type = type, .span = span, .is_param = false});
        std::vector<MirInst*> post(block->insts.begin() + static_cast<std::ptrdiff_t>(k) + 1,
                                   block->insts.end());
        block->insts.resize(k);
        auto* merge = ctx.alloc<MirBlock>();
        merge->id = ids.block();
        auto* load_place = ctx.alloc<MirPlace>();
        load_place->local = temp;
        inst->payload = MirLoad{load_place};
        merge->insts.push_back(inst);
        merge->insts.insert(merge->insts.end(), post.begin(), post.end());

        const auto* i32 = types.builtin(BuiltinKind::I32);
        auto discr = ids.value();
        block->insts.push_back(make_inst(ctx, discr, i32, span, MirEnumDiscriminant{value}));
        MirBlock* test_block = block;
        const auto& variants = en->variants();
        std::vector<MirBlock*> variant_blocks;
        for (uint32_t v = 0; v < variants.size(); ++v) {
          auto* var_bb = ctx.alloc<MirBlock>();
          var_bb->id = ids.block();
          variant_blocks.push_back(var_bb);
          if (v + 1 < variants.size()) {
            auto* next_test = ctx.alloc<MirBlock>();
            next_test->id = ids.block();
            auto tag = ids.value();
            test_block->insts.push_back(
                make_inst(ctx, tag, i32, span, MirConstInt{static_cast<int64_t>(v)}));
            auto is_v = ids.value();
            test_block->insts.push_back(make_inst(
                ctx, is_v, types.bool_type(), span, MirBinary{BinaryOp::EqEq, discr, tag}));
            test_block->insts.push_back(make_inst(
                ctx, MirValueId{}, nullptr, span, MirCondBr{is_v, var_bb->id, next_test->id}));
            fn->blocks.push_back(next_test);
            test_block = next_test;
          } else {
            test_block->insts.push_back(
                make_inst(ctx, MirValueId{}, nullptr, span, MirBr{var_bb->id}));
          }
          // The variant's block: copy each payload, rebuild, store, merge.
          auto* payloads = ctx.alloc<std::vector<MirValueId>>();
          for (uint32_t j = 0; j < variants[v].payload_types.size(); ++j) {
            const Type* pt = variants[v].payload_types[j];
            auto pv = ids.value();
            var_bb->insts.push_back(make_inst(ctx, pv, pt, span, MirEnumPayload{value, v, j}));
            payloads->push_back(emit_copy_of(pv, pt, span, hooks, ids, ctx, types, var_bb->insts));
          }
          auto rebuilt = ids.value();
          var_bb->insts.push_back(
              make_inst(ctx, rebuilt, type, span, MirEnumConstruct{en, v, payloads}));
          auto* store_place = ctx.alloc<MirPlace>();
          store_place->local = temp;
          var_bb->insts.push_back(
              make_inst(ctx, MirValueId{}, nullptr, span, MirStore{store_place, rebuilt}));
          var_bb->insts.push_back(make_inst(ctx, MirValueId{}, nullptr, span, MirBr{merge->id}));
          fn->blocks.push_back(var_bb);
        }
        fn->blocks.push_back(merge);
        for (auto* blk : fn->blocks) {
          for (auto* a : blk->insts) {
            if (a->result.valid()) {
              defining[a->result.id] = a;
            }
          }
        }
        changed = true;
        break; // this block ends here; the loop moves on to the next block
      }
    }
  }
  return changed;
}

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
  // The call-site pass runs at least once even when there are no
  // templates: a reference to a compiler intrinsic (`size_of<T>`) is
  // made concrete there, template or not.  The concreteness check
  // below runs regardless, to catch synthetic residue.

  // Specialization cache: (generic fn, type args) → specialized fn.
  SpecializationState state;
  const auto copy_out_hooks = find_copy_out_hooks(module, generic_templates);
  if (copy_out_hooks.copy_out_template != nullptr && copy_out_hooks.copy_out_string == nullptr) {
    // A string could not be copied out; say so rather than leave the
    // identity in place and let the value dangle past its block.
    result.diagnostics.push_back(
        Diagnostic::error(copy_out_hooks.copy_out_fn->span,
                          "internal: the prelude declares `copy_out` but not `copy_out_string`"));
  }

  // Phase 2+3+4: iterate until no new specializations are produced.
  // Use index-based iteration because specialize_call_site() may
  // push_back new functions, invalidating range-for iterators.
  bool changed = true;
  while (changed) {
    changed = false;

    const size_t fn_count = module.functions.size();
    for (size_t fn_idx = 0; fn_idx < fn_count; ++fn_idx) {
      auto* fn = module.functions[fn_idx];

      // Copies out of a domain first, per concrete type, so that what a
      // copy_out call becomes is what gets specialized below.
      if (expand_copy_out_calls(
              fn, module, generic_templates, copy_out_hooks, ctx, types, result.diagnostics)) {
        changed = true;
      }

      // Per-function indexes: value types for O(1) argument lookups,
      // and the call each callee value feeds.
      auto value_types = build_value_types(fn);
      auto calls_by_callee = build_call_index(fn);

      for (auto* block : fn->blocks) {
        for (size_t inst_idx = 0; inst_idx < block->insts.size();
             ++inst_idx) {
          auto* inst = block->insts[inst_idx];
          auto* fn_ref = std::get_if<MirFnRef>(&inst->payload);
          if (fn_ref == nullptr) {
            continue;
          }

          if (specialize_call_site(inst,
                                   fn_ref,
                                   calls_by_callee,
                                   value_types,
                                   generic_templates,
                                   state,
                                   module,
                                   ctx,
                                   types)) {
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
        if (type_mentions_generic_param(inst->type)) {
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
