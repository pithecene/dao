add_executable(daoc main.cpp pipeline.cpp)
target_link_libraries(daoc PRIVATE dao_analysis dao_backend_llvm dao_ir dao_frontend)

# Bake install-relative directories plus build/source fallbacks so an
# installed `daoc` resolves its packaged runtime and stdlib outside the
# build tree while build-tree invocations keep working unchanged.
target_compile_definitions(daoc PRIVATE
  DAO_INSTALL_BINDIR="${CMAKE_INSTALL_BINDIR}"
  DAO_INSTALL_LIBDIR="${CMAKE_INSTALL_LIBDIR}"
  DAO_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}"
  DAO_RUNTIME_LIB="$<TARGET_FILE:dao_runtime>"
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# `daoc build` links against the runtime; building daoc alone must build it.
add_dependencies(daoc dao_runtime)

# Process-level tests: run daoc as users do (concurrent builds, outputs).
find_package(ut REQUIRED)
add_executable(driver_test driver_test.cpp)
target_link_libraries(driver_test PRIVATE dao_backend_llvm dao_frontend Boost::ut)
target_compile_definitions(driver_test PRIVATE
  DAO_DAOC="$<TARGET_FILE:daoc>"
  DAO_INSTALL_BINDIR="${CMAKE_INSTALL_BINDIR}"
  DAO_INSTALL_LIBDIR="${CMAKE_INSTALL_LIBDIR}"
  DAO_INSTALL_DATADIR="${CMAKE_INSTALL_DATADIR}"
  DAO_RUNTIME_LIB="$<TARGET_FILE:dao_runtime>"
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
add_dependencies(driver_test daoc)

include(CTest)
add_test(NAME driver_test COMMAND driver_test)

# End-to-end multi-file builds: discovery mode, explicit mode, and
# input-order determinism (compiler/driver/multifile_build_test.sh).
add_test(NAME multifile_build_test
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/multifile_build_test.sh $<TARGET_FILE:daoc> ${CMAKE_SOURCE_DIR})
