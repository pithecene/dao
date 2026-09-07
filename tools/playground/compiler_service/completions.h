#ifndef DAO_PLAYGROUND_COMPLETIONS_H
#define DAO_PLAYGROUND_COMPLETIONS_H

#include "service.h"

namespace dao::playground {

/// Completion candidates at an editor-buffer offset: members of the
/// receiver when the offset follows `.`, otherwise the symbols in scope.
auto completions(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_COMPLETIONS_H
