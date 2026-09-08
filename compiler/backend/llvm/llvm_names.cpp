#include "backend/llvm/llvm_names.h"
#include "frontend/resolve/resolve.h"

#include "frontend/ast/ast.h"
#include "frontend/module/program.h"

#include <string_view>

namespace dao {

auto symbol_is_extern(const Symbol& sym) -> bool {
  if (sym.kind != SymbolKind::Function || sym.decl == nullptr) {
    return false;
  }
  const auto* decl = sym.decl_as_decl();
  return decl->is<FunctionDecl>() && decl->as<FunctionDecl>().is_extern;
}

auto llvm_function_name(const Symbol& sym, const ModuleInfo* entry) -> std::string {
  if (sym.module == nullptr || symbol_is_extern(sym)) {
    return std::string(sym.name);
  }
  if (sym.name == "main" && sym.module == entry) {
    return "main";
  }
  return sym.module->display + "::" + std::string(sym.name);
}

auto is_builtin_intrinsic(const Symbol& sym) -> bool {
  // Recognition is by ownership: only a prelude module's declaration of
  // one of these names is the intrinsic.  Which names those are is the
  // resolver's to say — it is what stops any other module declaring
  // them — so the family is not written out a second time here.
  if (sym.module != nullptr && !sym.module->is_prelude) {
    return false;
  }
  return is_prelude_intrinsic(sym.name);
}

} // namespace dao
