#ifndef DAO_FRONTEND_RESOLVE_RESOLVE_H
#define DAO_FRONTEND_RESOLVE_RESOLVE_H

#include "frontend/ast/ast.h"
#include "frontend/diagnostics/diagnostic.h"
#include "frontend/module/program.h"
#include "frontend/module/source_map.h"
#include "frontend/resolve/resolve_context.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace dao {

struct ResolveResult {
  ResolveContext context;
  std::unordered_map<uint32_t, Symbol*> uses; // token span offset -> resolved Symbol*
  std::vector<Diagnostic> diagnostics;
};

// Run name resolution over a program: builtins, then the prelude
// group as one namespace, then one scope per module (spec §7.6), in
// topological order.  Imports bind module names; `b::name` resolves
// through the bound module's export table (CONTRACT_MODULE_SYSTEM.md
// §3, §4, §6).  Records each module's scope on its ModuleInfo.  The
// Program must outlive the result: symbol names are string_views into
// its source buffers.
/// The generic intrinsic family: declared by the prelude
/// (`stdlib/core/builtins.dao`) and answered by the backend with inline
/// IR rather than a call.  Recognition is by ownership — a prelude
/// module may declare these, no other module may — so this says only
/// which names belong to the family, mangled specializations
/// (`size_of$i32`) included.
auto is_prelude_intrinsic(std::string_view name) -> bool;

auto resolve(Program& program) -> ResolveResult;

// Resolution without a program: every file is its own module and the
// source map decides prelude membership.  Imports bind names that
// resolve no further.
auto resolve(std::span<const FileNode* const> files, const SourceMap* source_map)
    -> ResolveResult;

// Single-file convenience for tests: one file, no prelude group.
auto resolve(const FileNode& file) -> ResolveResult;

} // namespace dao

#endif // DAO_FRONTEND_RESOLVE_RESOLVE_H
