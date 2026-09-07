#ifndef DAO_PLAYGROUND_NAVIGATION_H
#define DAO_PLAYGROUND_NAVIGATION_H

#include "service.h"

namespace dao::playground {

// Navigation queries at an editor-buffer offset.  Each runs the frontend
// pipeline over the prelude group plus the buffer and reports only
// positions inside the buffer; a query that lands nowhere replies
// `null` (single result) or `[]` (list result).

auto hover(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;
auto goto_definition(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;
auto document_symbols(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;
auto references(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_NAVIGATION_H
