#include "backend/llvm/llvm_names.h"

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
  if (sym.module != nullptr && !sym.module->is_prelude) {
    return false;
  }
  // Instantiations carry the template name plus `$<type args>`.
  for (std::string_view base : {"size_of", "align_of", "null_ptr", "ptr_offset", "ptr_cast"}) {
    if (sym.name == base || (sym.name.starts_with(base) && sym.name.substr(base.size()).starts_with('$'))) {
      return true;
    }
  }
  return false;
}

} // namespace dao
