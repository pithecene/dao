#ifndef DAO_BACKEND_LLVM_LLVM_NAMES_H
#define DAO_BACKEND_LLVM_LLVM_NAMES_H

#include "frontend/resolve/symbol.h"

#include <string>

namespace dao {

struct ModuleInfo;

// ---------------------------------------------------------------------------
// Symbol naming (task spec §12).  One function names every function the
// backend declares or looks up, and special symbols are recognised by
// what they are, never by what they are called.
// ---------------------------------------------------------------------------

/// True for an `extern fn` declaration.
auto symbol_is_extern(const Symbol& sym) -> bool;

/// LLVM name of a function symbol: `extern fn` and symbols without an
/// owning module (builtins; single-file lowering) keep their name; `main`
/// of the entry module is `main`; every other function is
/// `<module>::<name>`, which keeps method (`T.m`) and instantiation
/// (`f$i32`) mangling intact and lets two modules declare one name.
auto llvm_function_name(const Symbol& sym, const ModuleInfo* entry) -> std::string;

/// Compiler intrinsics lowered as inline IR rather than calls: the
/// predeclared builtin functions and the prelude's `size_of` /
/// `align_of` / `ptr_offset` family.  A symbol owned by a non-prelude
/// module is never an intrinsic, whatever its name.
auto is_builtin_intrinsic(const Symbol& sym) -> bool;

} // namespace dao

#endif // DAO_BACKEND_LLVM_LLVM_NAMES_H
