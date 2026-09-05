// Prints the TypeScript module derived from the tooling surface table.
// `task gen-tooling-surface` redirects it into the playground frontend's
// generated/ directory.

#include "analysis/tooling_surface.h"

#include <iostream>

auto main() -> int {
  std::cout << dao::tooling::render_typescript();
  return 0;
}
