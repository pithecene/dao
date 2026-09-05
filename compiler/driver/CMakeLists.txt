add_executable(daoc main.cpp pipeline.cpp)
target_link_libraries(daoc PRIVATE dao_analysis dao_backend_llvm dao_ir dao_frontend)

# Bake the path to the runtime static library so `daoc build` can link it.
# Bake the source root so the driver can find stdlib prelude files.
target_compile_definitions(daoc PRIVATE
  DAO_RUNTIME_LIB="$<TARGET_FILE:dao_runtime>"
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# End-to-end multi-file builds: discovery mode, explicit mode, and
# input-order determinism (compiler/driver/multifile_build_test.sh).
include(CTest)
add_test(NAME multifile_build_test
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/multifile_build_test.sh $<TARGET_FILE:daoc> ${CMAKE_SOURCE_DIR})
