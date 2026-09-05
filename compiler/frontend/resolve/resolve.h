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

// Run name resolution over every parsed file of a program.  All files
// are declared into one shared file scope, in load order (Task 31 D0;
// per-module scopes arrive in D2).  The Program must outlive the
// result: symbol names are string_views into its source buffers.
// The source map decides which declarations belong to the prelude
// group and are therefore exempt from user-code naming restrictions
// (CONTRACT_MODULE_SYSTEM.md §7.7).
auto resolve(const Program& program) -> ResolveResult;
auto resolve(std::span<const FileNode* const> files, const SourceMap* source_map)
    -> ResolveResult;

// Single-file convenience for tests: one file, no prelude group.
auto resolve(const FileNode& file) -> ResolveResult;

} // namespace dao

#endif // DAO_FRONTEND_RESOLVE_RESOLVE_H
