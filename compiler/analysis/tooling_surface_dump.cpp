// Prints what the tooling surface table generates: the TypeScript module
// the playground frontend compiles against (default) or, with
// `--matrix`, the capability matrix for docs/tooling_capabilities.md.
// `task gen-tooling-surface` writes both.

#include "analysis/tooling_surface.h"

#include <iostream>
#include <string_view>

auto main(int argc, char* argv[]) -> int {
  const bool matrix = argc > 1 && std::string_view(argv[1]) == "--matrix";
  std::cout << (matrix ? dao::tooling::render_capability_matrix()
                       : dao::tooling::render_typescript());
  return 0;
}
