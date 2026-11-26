#ifndef DAO_PLAYGROUND_FORMAT_H
#define DAO_PLAYGROUND_FORMAT_H

#include "service.h"

namespace dao::playground {

/// Format the active document via `dao::format_source`, handling scratch
/// buffers that omit a leading `module` declaration and refusing to rewrite
/// when lexical or syntax errors exist.
auto format_document(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_FORMAT_H
