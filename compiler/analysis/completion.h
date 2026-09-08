#ifndef DAO_ANALYSIS_COMPLETION_H
#define DAO_ANALYSIS_COMPLETION_H

#include "frontend/resolve/resolve.h"
#include "frontend/typecheck/type_checker.h"

#include <string>
#include <vector>

namespace dao {

struct ModuleInfo;

struct CompletionItem {
  std::string label;
  std::string kind;  // "function", "variable", "type", "parameter", "field", "method"
  std::string type;  // printed type signature
};

/// Query identifier completions at a byte offset in the source.
/// Returns all symbols visible at that position, with type info.
auto query_completions(uint32_t offset,
                        const ResolveResult& resolve,
                        const TypeCheckResult& typed)
    -> std::vector<CompletionItem>;

/// Query dot completions for a receiver type, as seen from a module.
/// Returns fields (if struct) and methods (from concept extends) that
/// are in `from_module`'s method set: its own `extend` methods and the
/// prelude's (CONTRACT_MODULE_SYSTEM.md §5, §7.2), never a sibling
/// module's.  Null `from_module` is a query outside a program.
/// The caller is responsible for determining the receiver type.
/// The playground resolves both simple identifier receivers and
/// general expression receivers (calls, field chains, indexing)
/// via the typed expression map.
auto query_dot_completions(const Type* receiver_type,
                           const TypeCheckResult& typed,
                           const ModuleInfo* from_module) -> std::vector<CompletionItem>;

} // namespace dao

#endif // DAO_ANALYSIS_COMPLETION_H
