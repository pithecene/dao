#ifndef DAO_PLAYGROUND_PRELUDE_H
#define DAO_PLAYGROUND_PRELUDE_H

#include "service.h"

namespace dao::playground {

/// Compiler commit, LLVM version, target triple, and prelude file paths.
auto service_version(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

/// Read-only source, semantic tokens, and symbols of one stdlib prelude file.
auto prelude_source(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_PRELUDE_H
