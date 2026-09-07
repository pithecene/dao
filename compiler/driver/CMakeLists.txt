add_executable(daoc main.cpp pipeline.cpp)
target_link_libraries(daoc PRIVATE dao_analysis dao_backend_llvm dao_ir dao_frontend)

# Bake the path to the runtime static library so `daoc build` can link it.
# Bake the source root so the driver can find stdlib prelude files.
target_compile_definitions(daoc PRIVATE
  DAO_RUNTIME_LIB="$<TARGET_FILE:dao_runtime>"
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# `daoc build` links against the runtime; building daoc alone must build it.
add_dependencies(daoc dao_runtime)

# Process-level tests: run daoc as users do (concurrent builds, outputs).
find_package(ut REQUIRED)
add_executable(driver_test driver_test.cpp)
target_link_libraries(driver_test PRIVATE dao_backend_llvm dao_frontend Boost::ut)
target_compile_definitions(driver_test PRIVATE DAO_DAOC="$<TARGET_FILE:daoc>")
add_dependencies(driver_test daoc)

include(CTest)
add_test(NAME driver_test COMMAND driver_test)
