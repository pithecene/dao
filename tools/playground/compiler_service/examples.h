#ifndef DAO_PLAYGROUND_EXAMPLES_H
#define DAO_PLAYGROUND_EXAMPLES_H

#include "service.h"

namespace dao::playground {

/// The `.dao` files directly under the examples directory, sorted by name.
auto examples_list(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

/// Source of one example by file name (`request["name"]`).
auto example_source(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_EXAMPLES_H
