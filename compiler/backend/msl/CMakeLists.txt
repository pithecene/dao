add_library(dao_backend_msl STATIC
  msl_backend.cpp
)

target_include_directories(dao_backend_msl PUBLIC
  ${CMAKE_SOURCE_DIR}/compiler
)

target_link_libraries(dao_backend_msl
  PUBLIC
    dao_ir
    dao_frontend
)

add_executable(dao_msl_emit
  msl_emit_main.cpp
)

target_link_libraries(dao_msl_emit
  PRIVATE
    dao_backend_msl
)

target_compile_definitions(dao_msl_emit PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

find_package(ut REQUIRED)

add_executable(msl_backend_test
  msl_backend_test.cpp
)

target_link_libraries(msl_backend_test
  PRIVATE
    dao_backend_msl
    Boost::ut
)

target_compile_definitions(msl_backend_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

include(CTest)
add_test(NAME msl_backend_test COMMAND msl_backend_test)
