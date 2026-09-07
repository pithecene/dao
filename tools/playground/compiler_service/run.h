#ifndef DAO_PLAYGROUND_RUN_H
#define DAO_PLAYGROUND_RUN_H

#include "service.h"

namespace dao::playground {

// Initialize LLVM targets. Must be called once before run().
void init_run_support();

/// Compile the editor buffer to a native executable, run it with a
/// timeout, and report its output.  A failed compile replies with
/// `exit_code` -1 and the diagnostics.
auto run(const nlohmann::json& request, const ServiceContext& ctx) -> Reply;

} // namespace dao::playground

#endif // DAO_PLAYGROUND_RUN_H
