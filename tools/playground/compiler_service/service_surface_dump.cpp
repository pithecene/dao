// Prints the TypeScript module the playground frontend compiles against.
// `task gen-tooling-surface` redirects it into frontend/src/generated/.

#include "service_surface.h"

#include <iostream>

auto main() -> int {
  std::cout << dao::playground::render_service_typescript();
  return 0;
}
