#ifndef DAO_FRONTEND_MODULE_MODULE_GRAPH_H
#define DAO_FRONTEND_MODULE_MODULE_GRAPH_H

#include "frontend/module/program.h"

#include <optional>
#include <string>
#include <vector>

namespace dao {

// ---------------------------------------------------------------------------
// Module graph construction over a lexed and parsed Program
// (CONTRACT_MODULE_SYSTEM.md §2, §3, §8; task spec §8.4–§8.5).
//
// Registers one ModuleInfo per file with a module declaration, resolves
// import edges within the program, orders modules so that imports come
// first, and selects the entry module.  Every problem is a diagnostic
// on the program — the graph is still built as far as it goes, so
// analysis over a broken program keeps working.
// ---------------------------------------------------------------------------

/// Which file discovery loaded for which import identity (root-file
/// mode), so a file that declares a different module than the import
/// that located it is diagnosed as a mismatch rather than "not found".
struct LocatedImport {
  std::string identity;
  std::string display_path;
};

struct GraphInputs {
  std::optional<std::string> entry;       // explicit entry module (explicit / in-memory mode)
  std::string root_display;               // root-file mode: the root file; its module is the entry
  std::vector<LocatedImport> located;     // root-file mode
  std::vector<std::string> searched_roots; // root-file mode: named in "not found" diagnostics
};

/// Fill `program.modules`, `program.topo_order`, `program.entry`, and
/// each file's `module` back-pointer; append graph diagnostics.
void build_module_graph(Program& program, const GraphInputs& inputs);

/// `a::b::c` for a qualified path's segments.
auto module_display(const std::vector<std::string_view>& segments) -> std::string;

} // namespace dao

#endif // DAO_FRONTEND_MODULE_MODULE_GRAPH_H
