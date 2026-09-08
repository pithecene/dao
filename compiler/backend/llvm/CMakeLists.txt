find_package(LLVM REQUIRED)

add_library(dao_backend_llvm STATIC
  llvm_abi.cpp
  llvm_type_lowering.cpp
  llvm_runtime_hooks.cpp
  llvm_backend.cpp
  llvm_names.cpp
)

target_include_directories(dao_backend_llvm PUBLIC
  ${CMAKE_SOURCE_DIR}/compiler
)

target_link_libraries(dao_backend_llvm
  PUBLIC
    dao_ir
    dao_frontend
    llvm-core::llvm-core
)

# Tests
find_package(ut REQUIRED)

add_executable(llvm_backend_test
  llvm_backend_test.cpp
)

target_link_libraries(llvm_backend_test
  PRIVATE
    dao_backend_llvm
    Boost::ut
)

target_compile_definitions(llvm_backend_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

include(CTest)
add_test(NAME llvm_backend_test COMMAND llvm_backend_test)
