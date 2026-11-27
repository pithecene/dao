#include "backend/msl/msl_backend.h"

#include "frontend/module/program.h"
#include "frontend/resolve/symbol.h"
#include "frontend/types/type_printer.h"
#include "support/sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dao {

namespace {

auto is_i32_type(const Type* type) -> bool {
  if (type == nullptr || type->kind() != TypeKind::Builtin) {
    return false;
  }
  return static_cast<const TypeBuiltin*>(type)->builtin() == BuiltinKind::I32;
}

auto is_bool_type(const Type* type) -> bool {
  if (type == nullptr || type->kind() != TypeKind::Builtin) {
    return false;
  }
  return static_cast<const TypeBuiltin*>(type)->builtin() == BuiltinKind::Bool;
}

auto type_label(const Type* type) -> std::string {
  if (type == nullptr) {
    return "<untyped>";
  }
  return print_type(type);
}

auto in_prelude(const MirFunction& fn, const SourceMap* source_map) -> bool {
  if (fn.symbol != nullptr && fn.symbol->module != nullptr &&
      fn.symbol->module->is_prelude) {
    return true;
  }
  if (source_map != nullptr && fn.span.length > 0 &&
      source_map->is_prelude(fn.span.offset)) {
    return true;
  }
  return false;
}

auto qualified_fn_name(const MirFunction& fn) -> std::string {
  if (fn.symbol == nullptr) {
    return "<anonymous>";
  }
  if (fn.symbol->module != nullptr && !fn.symbol->module->display.empty()) {
    return fn.symbol->module->display + "::" + std::string(fn.symbol->name);
  }
  return std::string(fn.symbol->name);
}

auto is_valid_msl_kernel_identifier(std::string_view name) -> bool {
  if (name.empty()) {
    return false;
  }
  const unsigned char first = static_cast<unsigned char>(name.front());
  if (std::isalpha(first) == 0 && name.front() != '_') {
    return false;
  }
  for (const char ch : name) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) == 0 && ch != '_') {
      return false;
    }
  }
  static const std::set<std::string_view> kReserved = {
      "alignas",  "alignof",  "atomic_int", "atomic_uint", "bool",     "break",
      "case",     "char",     "class",      "const",       "constant", "continue",
      "default",  "device",   "do",         "double",      "else",     "enum",
      "false",    "float",    "for",        "fragment",    "goto",     "half",
      "if",       "inline",   "int",        "kernel",      "long",     "metal",
      "namespace","private",  " public",    "register",    "return",   "sampler",
      "short",    "signed",   "sizeof",     "static",      "struct",   "switch",
      "template", "texture",  "thread",     "threadgroup", "true",     "typedef",
      "uint",     "ulong",    "union",      "unsigned",    "ushort",   "using",
      "vertex",   "void",     "volatile",   "while",
  };
  return !kReserved.contains(name);
}

auto checked_msl_preamble() -> std::string {
  return R"(#include <metal_stdlib>
using namespace metal;

inline int dao_add_i32(int a, int b, thread bool& trap) {
  long r = long(a) + long(b);
  if (r < -2147483648L || r > 2147483647L) {
    trap = true;
    return 0;
  }
  return int(r);
}

inline int dao_sub_i32(int a, int b, thread bool& trap) {
  long r = long(a) - long(b);
  if (r < -2147483648L || r > 2147483647L) {
    trap = true;
    return 0;
  }
  return int(r);
}

inline int dao_mul_i32(int a, int b, thread bool& trap) {
  long r = long(a) * long(b);
  if (r < -2147483648L || r > 2147483647L) {
    trap = true;
    return 0;
  }
  return int(r);
}

inline int dao_div_i32(int a, int b, thread bool& trap) {
  if (b == 0 || (a == int(-2147483648L) && b == -1)) {
    trap = true;
    return 0;
  }
  return a / b;
}

inline int dao_rem_i32(int a, int b, thread bool& trap) {
  if (b == 0 || (a == int(-2147483648L) && b == -1)) {
    trap = true;
    return 0;
  }
  return a % b;
}

inline int dao_neg_i32(int a, thread bool& trap) {
  if (a == int(-2147483648L)) {
    trap = true;
    return 0;
  }
  return -a;
}
)";
}

auto format_i32_literal(int64_t value) -> std::string {
  if (value == std::numeric_limits<int32_t>::min()) {
    return "int(-2147483648L)";
  }
  return std::to_string(static_cast<int32_t>(value));
}

auto resolve_origin(const MirFunction& fn, const SourceMap* source_map) -> MslSourceOrigin {
  MslSourceOrigin origin;
  if (fn.symbol != nullptr && fn.symbol->module != nullptr) {
    origin.module_name = fn.symbol->module->display;
  }
  const Span span = (fn.symbol != nullptr && fn.symbol->decl_span.length > 0)
                        ? fn.symbol->decl_span
                        : fn.span;
  if (source_map != nullptr && span.length > 0) {
    const auto* file = source_map->file_for(span.offset);
    if (file != nullptr) {
      const auto loc = source_map->locate(span.offset);
      origin.file = file->display_path;
      origin.line = loc.line;
      origin.col = loc.col;
    }
  }
  return origin;
}

struct AcyclicSchedule {
  std::vector<const MirBlock*> blocks;
  bool ok = false;
};

// Verify that reachable control flow from `blocks[0]` is a finite DAG and
// return blocks in reverse-postorder (topological order).
auto schedule_acyclic_blocks(const MirFunction& fn,
                             std::vector<MslRejection>& rejections) -> AcyclicSchedule {
  const std::string fn_label = qualified_fn_name(fn);
  if (fn.blocks.empty() || fn.blocks.front() == nullptr) {
    rejections.push_back(
        {.span = fn.span, .function_name = fn_label, .message = "function has no MIR entry block"});
    return {};
  }

  std::unordered_map<uint32_t, const MirBlock*> by_id;
  for (const auto* block : fn.blocks) {
    if (block != nullptr) {
      by_id[block->id.id] = block;
    }
  }

  enum class VisitState : uint8_t { Unvisited, Visiting, Visited };
  std::unordered_map<uint32_t, VisitState> state;
  std::vector<const MirBlock*> postorder;
  bool valid = true;

  auto dfs = [&](auto& self, const MirBlock* block) -> void {
    state[block->id.id] = VisitState::Visiting;
    if (block->insts.empty() || block->insts.back() == nullptr) {
      rejections.push_back({.span = fn.span,
                            .function_name = fn_label,
                            .message = "MIR basic block is missing a terminator"});
      valid = false;
      state[block->id.id] = VisitState::Visited;
      return;
    }

    const MirInst& term = *block->insts.back();
    auto visit_edge = [&](BlockId target_id) {
      auto it = by_id.find(target_id.id);
      if (it == by_id.end()) {
        rejections.push_back({.span = term.span,
                              .function_name = fn_label,
                              .message = "branch targets missing MIR basic block"});
        valid = false;
        return;
      }
      const VisitState target_state = state[target_id.id];
      if (target_state == VisitState::Visiting) {
        rejections.push_back(
            {.span = term.span.length > 0 ? term.span : fn.span,
             .function_name = fn_label,
             .message = "cyclic control flow (loop) is unsupported in the Metal kernel slice"});
        valid = false;
        return;
      }
      if (target_state == VisitState::Unvisited) {
        self(self, it->second);
      }
    };

    if (const auto* br = std::get_if<MirBr>(&term.payload)) {
      visit_edge(br->target);
    } else if (const auto* cond_br = std::get_if<MirCondBr>(&term.payload)) {
      visit_edge(cond_br->then_block);
      visit_edge(cond_br->else_block);
    } else if (const auto* ret = std::get_if<MirReturn>(&term.payload)) {
      if (!ret->has_value || !ret->value.valid()) {
        rejections.push_back({.span = term.span,
                              .function_name = fn_label,
                              .message = "Metal kernel return must produce an i32 value"});
        valid = false;
      }
    } else {
      rejections.push_back({.span = term.span,
                            .function_name = fn_label,
                            .message = "unsupported MIR block terminator in Metal kernel slice"});
      valid = false;
    }

    state[block->id.id] = VisitState::Visited;
    postorder.push_back(block);
  };

  dfs(dfs, fn.blocks.front());
  if (!valid) {
    return {};
  }
  std::ranges::reverse(postorder);
  return {.blocks = std::move(postorder), .ok = true};
}

auto lower_single_kernel(const MirFunction& fn,
                         const SourceMap* source_map,
                         std::vector<MslRejection>& rejections) -> std::optional<MslKernel> {
  const std::string fn_label = qualified_fn_name(fn);
  const Span fn_span = (fn.symbol != nullptr && fn.symbol->decl_span.length > 0)
                           ? fn.symbol->decl_span
                           : fn.span;

  if (fn.symbol == nullptr) {
    rejections.push_back({.span = fn_span,
                          .function_name = fn_label,
                          .message = "anonymous function cannot be lowered to Metal"});
    return std::nullopt;
  }
  const std::string op_name(fn.symbol->name);
  if (!is_valid_msl_kernel_identifier(op_name)) {
    rejections.push_back(
        {.span = fn_span,
         .function_name = fn_label,
         .message = "function name '" + op_name + "' is not a valid Metal kernel identifier"});
    return std::nullopt;
  }
  if (fn.is_extern) {
    rejections.push_back({.span = fn_span,
                          .function_name = fn_label,
                          .message = "extern function '" + op_name +
                                     "' cannot be lowered to a Metal kernel"});
    return std::nullopt;
  }
  if (!is_i32_type(fn.return_type)) {
    rejections.push_back({.span = fn_span,
                          .function_name = fn_label,
                          .message = "unsupported kernel return type '" +
                                     type_label(fn.return_type) +
                                     "': Metal kernel slice requires i32"});
    return std::nullopt;
  }

  std::vector<const MirLocal*> params;
  std::unordered_map<uint32_t, const MirLocal*> local_by_id;
  for (const auto& local : fn.locals) {
    local_by_id[local.id.id] = &local;
    if (local.is_param) {
      if (!is_i32_type(local.type)) {
        rejections.push_back({.span = local.span.length > 0 ? local.span : fn_span,
                              .function_name = fn_label,
                              .message = "unsupported kernel parameter type '" +
                                         type_label(local.type) +
                                         "': Metal kernel slice requires i32"});
        return std::nullopt;
      }
      params.push_back(&local);
    } else {
      if (!is_i32_type(local.type) && !is_bool_type(local.type)) {
        rejections.push_back(
            {.span = local.span.length > 0 ? local.span : fn_span,
             .function_name = fn_label,
             .message = "unsupported local variable type '" + type_label(local.type) +
                        "': Metal kernel slice supports only i32 and bool locals"});
        return std::nullopt;
      }
    }
  }

  if (params.empty()) {
    rejections.push_back({.span = fn_span,
                          .function_name = fn_label,
                          .message = "Metal kernel '" + op_name +
                                     "' requires at least one scalar i32 input parameter"});
    return std::nullopt;
  }

  const auto schedule = schedule_acyclic_blocks(fn, rejections);
  if (!schedule.ok) {
    return std::nullopt;
  }

  // Track value types across blocks so multi-block kernels can declare SSA
  // value slots up front before the bounded switch dispatch.
  std::unordered_map<uint32_t, const Type*> value_types;
  std::vector<std::pair<uint32_t, const Type*>> ordered_values;

  auto reject_inst = [&](const MirInst& inst, const std::string& msg) {
    rejections.push_back({.span = inst.span.length > 0 ? inst.span : fn_span,
                          .function_name = fn_label,
                          .message = msg});
  };

  // Validate instructions and collect SSA value declarations in topological order.
  for (const auto* block : schedule.blocks) {
    for (size_t idx = 0; idx < block->insts.size(); ++idx) {
      const MirInst& inst = *block->insts[idx];
      const bool is_last = (idx + 1 == block->insts.size());
      const bool is_term = std::holds_alternative<MirBr>(inst.payload) ||
                           std::holds_alternative<MirCondBr>(inst.payload) ||
                           std::holds_alternative<MirReturn>(inst.payload);
      if (is_term != is_last) {
        reject_inst(inst, "MIR terminator must appear only at the end of a basic block");
        return std::nullopt;
      }
      if (is_term) {
        if (const auto* cond_br = std::get_if<MirCondBr>(&inst.payload)) {
          auto cond_it = value_types.find(cond_br->cond.id);
          if (cond_it == value_types.end() || !is_bool_type(cond_it->second)) {
            reject_inst(inst, "conditional branch requires a defined bool condition");
            return std::nullopt;
          }
        } else if (const auto* ret = std::get_if<MirReturn>(&inst.payload)) {
          auto ret_it = value_types.find(ret->value.id);
          if (ret_it == value_types.end() || !is_i32_type(ret_it->second)) {
            reject_inst(inst, "return value must be a defined i32 value");
            return std::nullopt;
          }
        }
        continue;
      }

      auto record_result = [&](const Type* expected_Check) -> bool {
        if (!inst.result.valid() || inst.type == nullptr || expected_Check == nullptr) {
          reject_inst(inst, "MIR value instruction is missing a typed result");
          return false;
        }
        if (!is_i32_type(inst.type) && !is_bool_type(inst.type)) {
          reject_inst(inst,
                      "unsupported expression type '" + type_label(inst.type) +
                          "' in Metal kernel slice");
          return false;
        }
        value_types[inst.result.id] = inst.type;
        ordered_values.emplace_back(inst.result.id, inst.type);
        return true;
      };

      if (const auto* ci = std::get_if<MirConstInt>(&inst.payload)) {
        if (!is_i32_type(inst.type) || ci->value < std::numeric_limits<int32_t>::min() ||
            ci->value > std::numeric_limits<int32_t>::max()) {
          reject_inst(inst, "integer constant is outside i32 range in Metal kernel slice");
          return std::nullopt;
        }
        if (!record_result(inst.type)) {
          return std::nullopt;
        }
      } else if (std::holds_alternative<MirConstBool>(inst.payload)) {
        if (!is_bool_type(inst.type) || !record_result(inst.type)) {
          reject_inst(inst, "boolean constant has non-bool type");
          return std::nullopt;
        }
      } else if (const auto* load = std::get_if<MirLoad>(&inst.payload)) {
        if (load->place == nullptr || !load->place->projections.empty()) {
          reject_inst(
              inst,
              "projected memory load (field/index/deref) is unsupported in Metal kernel slice");
          return std::nullopt;
        }
        if (!local_by_id.contains(load->place->local.id) || !record_result(inst.type)) {
          return std::nullopt;
        }
      } else if (const auto* store = std::get_if<MirStore>(&inst.payload)) {
        if (store->place == nullptr || !store->place->projections.empty()) {
          reject_inst(
              inst,
              "projected memory store (field/index/deref) is unsupported in Metal kernel slice");
          return std::nullopt;
        }
        auto loc_it = local_by_id.find(store->place->local.id);
        auto val_it = value_types.find(store->value.id);
        if (loc_it == local_by_id.end() || val_it == value_types.end()) {
          reject_inst(inst, "store references undefined local or value");
          return std::nullopt;
        }
      } else if (const auto* unary = std::get_if<MirUnary>(&inst.payload)) {
        auto op_it = value_types.find(unary->operand.id);
        if (op_it == value_types.end()) {
          reject_inst(inst, "unary operation references undefined operand");
          return std::nullopt;
        }
        if (unary->op == UnaryOp::Negate && !is_i32_type(op_it->second)) {
          reject_inst(inst, "unary '-' in Metal kernel slice supports only i32");
          return std::nullopt;
        }
        if (unary->op == UnaryOp::Not && !is_bool_type(op_it->second)) {
          reject_inst(inst, "unary '!' in Metal kernel slice supports only bool");
          return std::nullopt;
        }
        if (!record_result(inst.type)) {
          return std::nullopt;
        }
      } else if (const auto* binary = std::get_if<MirBinary>(&inst.payload)) {
        auto lhs_it = value_types.find(binary->lhs.id);
        auto rhs_it = value_types.find(binary->rhs.id);
        if (lhs_it == value_types.end() || rhs_it == value_types.end()) {
          reject_inst(inst, "binary operation references undefined operand");
          return std::nullopt;
        }
        const bool lhs_i32 = is_i32_type(lhs_it->second);
        const bool rhs_i32 = is_i32_type(rhs_it->second);
        const bool lhs_bool = is_bool_type(lhs_it->second);
        const bool rhs_bool = is_bool_type(rhs_it->second);
        switch (binary->op) {
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
        case BinaryOp::Lt:
        case BinaryOp::LtEq:
        case BinaryOp::Gt:
        case BinaryOp::GtEq:
          if (!lhs_i32 || !rhs_i32) {
            reject_inst(inst,
                        "numeric binary operator in Metal kernel slice supports only i32 operands");
            return std::nullopt;
          }
          break;
        case BinaryOp::EqEq:
        case BinaryOp::BangEq:
          if (!((lhs_i32 && rhs_i32) || (lhs_bool && rhs_bool))) {
            reject_inst(
                inst,
                "equality operator in Metal kernel slice supports only i32 or bool operands");
            return std::nullopt;
          }
          break;
        // MIR lowers `and` and `or` into branches (CONTRACT_MIR_BOUNDARY.md),
        // so neither reaches a kernel as an instruction.
        case BinaryOp::And:
        case BinaryOp::Or:
          reject_inst(inst, "a logical operator reached the Metal backend as a binary instruction");
          return std::nullopt;
        }
        if (!record_result(inst.type)) {
          return std::nullopt;
        }
      } else if (std::holds_alternative<MirCall>(inst.payload) ||
                 std::holds_alternative<MirFnRef>(inst.payload)) {
        reject_inst(inst, "function calls are unsupported in the pure scalar Metal kernel slice");
        return std::nullopt;
      } else if (std::holds_alternative<MirPtrOp>(inst.payload)) {
        reject_inst(inst, "raw pointer operations are unsupported in the Metal kernel slice");
        return std::nullopt;
      } else if (std::holds_alternative<MirModeEnter>(inst.payload) ||
                 std::holds_alternative<MirModeExit>(inst.payload) ||
                 std::holds_alternative<MirResourceEnter>(inst.payload) ||
                 std::holds_alternative<MirResourceExit>(inst.payload)) {
        reject_inst(inst, "mode and resource blocks are unsupported in the Metal kernel slice");
        return std::nullopt;
      } else {
        reject_inst(inst,
                    "unsupported MIR construct '" +
                        std::string(mir_inst_kind_name(inst.kind())) +
                        "' in the Metal kernel slice");
        return std::nullopt;
      }
    }
  }

  // Emit Metal kernel source.
  std::ostringstream out;
  out << "kernel void " << op_name << "(";
  for (size_t p_idx = 0; p_idx < params.size(); ++p_idx) {
    out << "device const int* dao_in_" << p_idx << " [[buffer(" << p_idx << ")]], ";
  }
  out << "device int* dao_out [[buffer(" << params.size() << ")]], "
      << "device atomic_uint* dao_status [[buffer(" << (params.size() + 1) << ")]], "
      << "uint dao_tid [[thread_position_in_grid]]) {\n";
  out << "  bool dao_trap = false;\n";

  for (size_t p_idx = 0; p_idx < params.size(); ++p_idx) {
    out << "  int l" << params[p_idx]->id.id << " = dao_in_" << p_idx << "[dao_tid];\n";
  }
  for (const auto& local : fn.locals) {
    if (!local.is_param) {
      if (is_i32_type(local.type)) {
        out << "  int l" << local.id.id << " = 0;\n";
      } else {
        out << "  bool l" << local.id.id << " = false;\n";
      }
    }
  }

  const bool single_block = (schedule.blocks.size() == 1);
  if (!single_block) {
    for (const auto& [val_id, val_type] : ordered_values) {
      if (is_i32_type(val_type)) {
        out << "  int v" << val_id << " = 0;\n";
      } else {
        out << "  bool v" << val_id << " = false;\n";
      }
    }
  }

  auto emit_non_term = [&](const MirInst& inst, std::string_view indent) {
    const char* decl_prefix = is_i32_type(inst.type) ? "const int v" : "const bool v";
    const std::string id_text = std::to_string(inst.result.id);
    const std::string lhs = single_block ? (std::string(decl_prefix) + id_text) : ("v" + id_text);
    if (const auto* ci = std::get_if<MirConstInt>(&inst.payload)) {
      out << indent << lhs << " = " << format_i32_literal(ci->value) << ";\n";
    } else if (const auto* cb = std::get_if<MirConstBool>(&inst.payload)) {
      out << indent << lhs << " = " << (cb->value ? "true" : "false") << ";\n";
    } else if (const auto* load = std::get_if<MirLoad>(&inst.payload)) {
      out << indent << lhs << " = l" << load->place->local.id << ";\n";
    } else if (const auto* store = std::get_if<MirStore>(&inst.payload)) {
      out << indent << "l" << store->place->local.id << " = v" << store->value.id << ";\n";
    } else if (const auto* unary = std::get_if<MirUnary>(&inst.payload)) {
      if (unary->op == UnaryOp::Negate) {
        out << indent << lhs << " = dao_neg_i32(v" << unary->operand.id << ", dao_trap);\n";
      } else {
        out << indent << lhs << " = !v" << unary->operand.id << ";\n";
      }
    } else if (const auto* binary = std::get_if<MirBinary>(&inst.payload)) {
      const std::string a = "v" + std::to_string(binary->lhs.id);
      const std::string b = "v" + std::to_string(binary->rhs.id);
      switch (binary->op) {
      case BinaryOp::Add:
        out << indent << lhs << " = dao_add_i32(" << a << ", " << b << ", dao_trap);\n";
        break;
      case BinaryOp::Sub:
        out << indent << lhs << " = dao_sub_i32(" << a << ", " << b << ", dao_trap);\n";
        break;
      case BinaryOp::Mul:
        out << indent << lhs << " = dao_mul_i32(" << a << ", " << b << ", dao_trap);\n";
        break;
      case BinaryOp::Div:
        out << indent << lhs << " = dao_div_i32(" << a << ", " << b << ", dao_trap);\n";
        break;
      case BinaryOp::Mod:
        out << indent << lhs << " = dao_rem_i32(" << a << ", " << b << ", dao_trap);\n";
        break;
      case BinaryOp::EqEq:
        out << indent << lhs << " = (" << a << " == " << b << ");\n";
        break;
      case BinaryOp::BangEq:
        out << indent << lhs << " = (" << a << " != " << b << ");\n";
        break;
      case BinaryOp::Lt:
        out << indent << lhs << " = (" << a << " < " << b << ");\n";
        break;
      case BinaryOp::LtEq:
        out << indent << lhs << " = (" << a << " <= " << b << ");\n";
        break;
      case BinaryOp::Gt:
        out << indent << lhs << " = (" << a << " > " << b << ");\n";
        break;
      case BinaryOp::GtEq:
        out << indent << lhs << " = (" << a << " >= " << b << ");\n";
        break;
      // Rejected when the kernel was validated.
      case BinaryOp::And:
      case BinaryOp::Or:
        break;
      }
    }
  };

  if (single_block) {
    const MirBlock& entry = *schedule.blocks.front();
    for (size_t idx = 0; idx + 1 < entry.insts.size(); ++idx) {
      emit_non_term(*entry.insts[idx], "  ");
    }
    const auto& ret = std::get<MirReturn>(entry.insts.back()->payload);
    out << "  if (dao_trap) {\n"
        << "    atomic_fetch_or_explicit(dao_status, 1u, memory_order_relaxed);\n"
        << "    return;\n"
        << "  }\n"
        << "  dao_out[dao_tid] = v" << ret.value.id << ";\n";
  } else {
    out << "  uint dao_block = " << schedule.blocks.front()->id.id << "u;\n"
        << "  int dao_ret = 0;\n"
        << "  bool dao_done = false;\n"
        << "  for (uint dao_step = 0u; dao_step < " << schedule.blocks.size()
        << "u && !dao_done; ++dao_step) {\n"
        << "    switch (dao_block) {\n";
    for (const auto* block : schedule.blocks) {
      out << "    case " << block->id.id << "u: {\n";
      for (size_t idx = 0; idx + 1 < block->insts.size(); ++idx) {
        emit_non_term(*block->insts[idx], "      ");
      }
      const MirInst& term = *block->insts.back();
      if (const auto* br = std::get_if<MirBr>(&term.payload)) {
        out << "      dao_block = " << br->target.id << "u;\n";
      } else if (const auto* cond_br = std::get_if<MirCondBr>(&term.payload)) {
        out << "      dao_block = v" << cond_br->cond.id << " ? " << cond_br->then_block.id
            << "u : " << cond_br->else_block.id << "u;\n";
      } else if (const auto* ret = std::get_if<MirReturn>(&term.payload)) {
        out << "      dao_ret = v" << ret->value.id << ";\n"
            << "      dao_done = true;\n";
      }
      out << "      break;\n"
          << "    }\n";
    }
    out << "    default: {\n"
        << "      dao_trap = true;\n"
        << "      dao_done = true;\n"
        << "      break;\n"
        << "    }\n"
        << "    }\n"
        << "  }\n"
        << "  if (dao_trap || !dao_done) {\n"
        << "    atomic_fetch_or_explicit(dao_status, 1u, memory_order_relaxed);\n"
        << "    return;\n"
        << "  }\n"
        << "  dao_out[dao_tid] = dao_ret;\n";
  }

  out << "}\n";

  MslKernelSignature sig;
  sig.return_type = "i32";
  sig.effects = std::string(kMslKernelEffectProfile);
  for (const auto* param : params) {
    sig.param_names.emplace_back(param->symbol != nullptr ? param->symbol->name : "");
    sig.param_types.emplace_back("i32");
  }

  return MslKernel{
      .operation_name = op_name,
      .target = std::string(kMslTargetId),
      .origin = resolve_origin(fn, source_map),
      .signature = std::move(sig),
      .msl_source = out.str(),
  };
}

} // namespace

auto MslBackend::lower_kernels(const MirModule& mir_module,
                               std::span<const std::string_view> kernel_names,
                               const SourceMap* source_map) -> MslBackendOutcome {
  MslBackendOutcome outcome;
  if (kernel_names.empty()) {
    outcome.rejections.push_back(
        {.span = mir_module.span, .function_name = "", .message = "no kernel names requested"});
    return outcome;
  }

  std::unordered_map<std::string, std::vector<const MirFunction*>> by_short_name;
  std::unordered_map<std::string, const MirFunction*> by_qualified_name;
  for (const auto* fn : mir_module.functions) {
    if (fn == nullptr || fn->symbol == nullptr || in_prelude(*fn, source_map)) {
      continue;
    }
    by_short_name[std::string(fn->symbol->name)].push_back(fn);
    by_qualified_name.emplace(qualified_fn_name(*fn), fn);
  }

  std::set<std::string> seen_requests;
  std::set<const MirFunction*> seen_functions;
  std::unordered_map<std::string, std::string> emitted_entry_names;
  std::ostringstream bundle;
  bundle << checked_msl_preamble();

  for (const std::string_view requested : kernel_names) {
    const std::string req_str(requested);
    if (!seen_requests.emplace(req_str).second) {
      outcome.rejections.push_back({.span = mir_module.span,
                                    .function_name = req_str,
                                    .message = "duplicate kernel request '" + req_str + "'"});
      continue;
    }

    const MirFunction* resolved_fn = nullptr;
    if (requested.find("::") != std::string_view::npos) {
      auto q_it = by_qualified_name.find(req_str);
      if (q_it != by_qualified_name.end()) {
        resolved_fn = q_it->second;
      }
    } else {
      auto s_it = by_short_name.find(req_str);
      if (s_it != by_short_name.end()) {
        if (s_it->second.size() > 1) {
          outcome.rejections.push_back(
              {.span = s_it->second.front()->span,
               .function_name = req_str,
               .message = "requested kernel '" + req_str +
                          "' is ambiguous across multiple modules; qualify as module::name"});
          continue;
        }
        resolved_fn = s_it->second.front();
      }
    }

    if (resolved_fn == nullptr) {
      outcome.rejections.push_back({.span = mir_module.span,
                                    .function_name = req_str,
                                    .message = "requested kernel '" + req_str +
                                               "' was not found in user MIR functions"});
      continue;
    }

    const std::string q_name = qualified_fn_name(*resolved_fn);
    const Span fn_span = (resolved_fn->symbol != nullptr &&
                          resolved_fn->symbol->decl_span.length > 0)
                             ? resolved_fn->symbol->decl_span
                             : resolved_fn->span;
    if (!seen_functions.insert(resolved_fn).second) {
      outcome.rejections.push_back(
          {.span = fn_span,
           .function_name = q_name,
           .message = "duplicate kernel request resolves to the same function '" + q_name + "'"});
      continue;
    }

    const std::string entry_name(resolved_fn->symbol->name);
    auto [entry_it, inserted] = emitted_entry_names.emplace(entry_name, q_name);
    if (!inserted) {
      outcome.rejections.push_back(
          {.span = fn_span,
           .function_name = q_name,
           .message = "Metal entry-point name '" + entry_name + "' collides across '" +
                      entry_it->second + "' and '" + q_name + "'"});
      continue;
    }

    auto lowered = lower_single_kernel(*resolved_fn, source_map, outcome.rejections);
    if (lowered.has_value()) {
      bundle << "\n" << lowered->msl_source;
      outcome.kernels.push_back(std::move(*lowered));
    }
  }

  if (outcome.rejections.empty()) {
    outcome.bundle_source = bundle.str();
    outcome.bundle_sha256 = sha256_hex(outcome.bundle_source);
  }
  return outcome;
}

auto MslBackend::render_manifest_tsv(const MslBackendOutcome& outcome) -> std::string {
  std::ostringstream out;
  const std::string digest = outcome.bundle_sha256.empty()
                                 ? sha256_hex(outcome.bundle_source)
                                 : outcome.bundle_sha256;
  out << "bundle_sha256\t" << digest << "\n";
  for (const auto& kernel : outcome.kernels) {
    std::string params_csv;
    for (size_t i = 0; i < kernel.signature.param_types.size(); ++i) {
      if (i > 0) {
        params_csv += ",";
      }
      params_csv += kernel.signature.param_types[i];
    }
    out << "kernel\t" << kernel.origin.module_name << "\t" << kernel.operation_name << "\t"
        << kernel.origin.file << ":" << kernel.origin.line << ":" << kernel.origin.col << "\t"
        << kernel.target << "\t" << kernel.signature.param_types.size() << "\t" << params_csv
        << "\t" << kernel.signature.return_type << "\t" << kernel.signature.effects << "\n";
  }
  return out.str();
}

} // namespace dao
