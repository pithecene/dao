#include "frontend/types/type_printer.h"

#include "frontend/types/type.h"

#include <sstream>
#include <string_view>

namespace dao {

namespace {

/// How much of a type is spelled out before what is left of it is
/// written `…`.  A type holds its arguments, which hold theirs:
/// `Pair<L29, L29>` where each `L` is a `Pair` of the one below has as
/// many leaves as there are paths through it — a billion of them, for
/// text a person is meant to read.  Depth alone still leaves a width of
/// two to the depth, so what is spelled out is counted as well.
constexpr int kDepthSpelledOut = 6;
constexpr int kRoomToSpellOut = 160;

/// What is left of the room: the printer stops spelling out once a type
/// has taken it, wherever in the type that happens.
struct Room {
  int left = kRoomToSpellOut;
};

void print_type_impl(std::ostream& out, const Type* type, bool with_args, int depth, Room& room);

/// A name, counted against the room left.
void print_name(std::ostream& out, std::string_view name, Room& room) {
  out << name;
  room.left -= static_cast<int>(name.size());
}

/// `<a, b>` after a nominal type's name, nothing when it takes no
/// arguments.
void print_type_args(std::ostream& out, const std::vector<const Type*>& args, bool with_args,
                     int depth, Room& room) {
  if (args.empty()) {
    return;
  }
  print_name(out, "<", room);
  bool first = true;
  for (const auto* arg : args) {
    if (!first) {
      print_name(out, ", ", room);
    }
    first = false;
    print_type_impl(out, arg, with_args, depth, room);
  }
  print_name(out, ">", room);
}

void print_type_impl(std::ostream& out, const Type* type, bool with_args, int depth, Room& room) {
  if (type == nullptr) {
    out << "<null>";
    return;
  }
  if (depth >= kDepthSpelledOut || room.left <= 0) {
    out << "…";
    return;
  }
  const int inside = depth + 1;

  switch (type->kind()) {
  case TypeKind::Builtin: {
    const auto* b = static_cast<const TypeBuiltin*>(type);
    print_name(out, builtin_kind_name(b->builtin()), room);
    break;
  }
  case TypeKind::Void:
    out << "void";
    break;
  case TypeKind::Pointer: {
    const auto* p = static_cast<const TypePointer*>(type);
    print_name(out, "Ptr<", room);
    print_type_impl(out, p->pointee(), with_args, inside, room);
    print_name(out, ">", room);
    break;
  }
  case TypeKind::Function: {
    const auto* f = static_cast<const TypeFunction*>(type);
    out << "fn(";
    bool first = true;
    for (const auto* param : f->param_types()) {
      if (!first)
        out << ", ";
      first = false;
      print_type_impl(out, param, with_args, inside, room);
    }
    out << "): ";
    print_type_impl(out, f->return_type(), with_args, inside, room);
    break;
  }
  case TypeKind::Named: {
    const auto* n = static_cast<const TypeNamed*>(type);
    print_name(out, n->name(), room);
    if (with_args) {
      print_type_args(out, n->type_args(), with_args, inside, room);
    }
    break;
  }
  case TypeKind::GenericParam: {
    const auto* g = static_cast<const TypeGenericParam*>(type);
    print_name(out, g->name(), room);
    break;
  }
  case TypeKind::Struct: {
    const auto* s = static_cast<const TypeStruct*>(type);
    print_name(out, s->name(), room);
    if (with_args) {
      print_type_args(out, s->type_args(), with_args, inside, room);
    }
    break;
  }
  case TypeKind::Enum: {
    const auto* e = static_cast<const TypeEnum*>(type);
    print_name(out, e->name(), room);
    if (with_args) {
      print_type_args(out, e->type_args(), with_args, inside, room);
    }
    break;
  }
  case TypeKind::Generator: {
    const auto* gen = static_cast<const TypeGenerator*>(type);
    print_name(out, "Generator<", room);
    print_type_impl(out, gen->yield_type(), with_args, inside, room);
    print_name(out, ">", room);
    break;
  }
  }
}

} // namespace

void print_type(std::ostream& out, const Type* type) {
  Room room;
  print_type_impl(out, type, true, 0, room);
}

auto print_type(const Type* type) -> std::string {
  std::ostringstream out;
  Room room;
  print_type_impl(out, type, true, 0, room);
  return out.str();
}

auto print_type_name(const Type* type) -> std::string {
  std::ostringstream out;
  Room room;
  print_type_impl(out, type, false, 0, room);
  return out.str();
}

} // namespace dao
