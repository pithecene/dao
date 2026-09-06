add_library(dao_analysis STATIC
  completion.cpp
  document_symbols.cpp
  goto_definition.cpp
  hover.cpp
  references.cpp
  semantic_tokens.cpp
  tooling_surface.cpp
)

target_include_directories(dao_analysis PUBLIC
  ${CMAKE_SOURCE_DIR}/compiler
)

target_link_libraries(dao_analysis PUBLIC dao_frontend)

# Tests
find_package(ut REQUIRED)

add_executable(semantic_tokens_test
  semantic_tokens_test.cpp
)

target_link_libraries(semantic_tokens_test PRIVATE dao_analysis Boost::ut)

target_compile_definitions(semantic_tokens_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_executable(analysis_test
  analysis_test.cpp
)

target_link_libraries(analysis_test PRIVATE dao_analysis Boost::ut)

add_executable(tooling_surface_test
  tooling_surface_test.cpp
)

target_link_libraries(tooling_surface_test PRIVATE dao_analysis Boost::ut)

target_compile_definitions(tooling_surface_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

include(CTest)
add_test(NAME semantic_tokens_test COMMAND semantic_tokens_test)
add_test(NAME analysis_test COMMAND analysis_test)
add_test(NAME tooling_surface_test COMMAND tooling_surface_test)
