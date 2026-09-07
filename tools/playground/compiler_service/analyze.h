#ifndef DAO_PLAYGROUND_ANALYZE_H
#define DAO_PLAYGROUND_ANALYZE_H

#include "service.h"

namespace dao::playground {

/// Full pipeline over the editor buffer: lexical and semantic tokens,
/// diagnostics, and the AST/HIR/MIR/LLVM dumps.  The IR dumps cover
/// the editor buffer's declarations only unless the request sets
/// `includePrelude`.
auto analyze(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_ANALYZE_H
