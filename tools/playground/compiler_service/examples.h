#ifndef DAO_PLAYGROUND_EXAMPLES_H
#define DAO_PLAYGROUND_EXAMPLES_H

#include "service.h"

namespace dao::playground {

/// Single-file `.dao` examples and single-entry multi-file example
/// directories under `examples/`, sorted by name.
auto examples_list(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

/// Program files and entry document of one example by name (`request["name"]`).
auto example_source(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_EXAMPLES_H
