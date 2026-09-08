add_library(dao_frontend STATIC
  ast/ast.cpp
  ast/ast_printer.cpp
  lexer/lexer.cpp
  module/module_graph.cpp
  module/program.cpp
  module/source_map.cpp
  parser/parser.cpp
  resolve/resolve.cpp
  types/type.cpp
  types/type_context.cpp
  types/type_identity.cpp
  types/type_printer.cpp
  typecheck/type_checker.cpp
  typecheck/type_conversion.cpp
)

target_include_directories(dao_frontend PUBLIC
  ${CMAKE_SOURCE_DIR}/compiler
)

# Tests
find_package(ut REQUIRED)

add_executable(lexer_test
  lexer/lexer_test.cpp
)

target_link_libraries(lexer_test PRIVATE dao_frontend Boost::ut)

target_compile_definitions(lexer_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_executable(parser_test
  parser/parser_test.cpp
)

target_link_libraries(parser_test PRIVATE dao_frontend Boost::ut)

target_compile_definitions(parser_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_executable(ast_printer_test
  ast/ast_printer_test.cpp
)

target_link_libraries(ast_printer_test PRIVATE dao_frontend Boost::ut)

target_compile_definitions(ast_printer_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_executable(resolve_test
  resolve/resolve_test.cpp
)

target_link_libraries(resolve_test PRIVATE dao_frontend Boost::ut)

target_compile_definitions(resolve_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_executable(type_test
  types/type_test.cpp
)

target_link_libraries(type_test PRIVATE dao_frontend Boost::ut)

target_compile_definitions(type_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

include(CTest)
add_test(NAME lexer_test COMMAND lexer_test)
add_test(NAME parser_test COMMAND parser_test)
add_test(NAME ast_printer_test COMMAND ast_printer_test)
add_test(NAME resolve_test COMMAND resolve_test)
add_test(NAME type_test COMMAND type_test)

add_executable(typecheck_test
  typecheck/typecheck_test.cpp
)

target_link_libraries(typecheck_test PRIVATE dao_frontend Boost::ut)

target_compile_definitions(typecheck_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME typecheck_test COMMAND typecheck_test)

add_executable(source_map_test
  module/source_map_test.cpp
)

target_link_libraries(source_map_test PRIVATE dao_frontend Boost::ut)

target_compile_definitions(source_map_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME source_map_test COMMAND source_map_test)

add_executable(module_graph_test
  module/module_graph_test.cpp
)

target_link_libraries(module_graph_test PRIVATE dao_frontend Boost::ut)

target_compile_definitions(module_graph_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME module_graph_test COMMAND module_graph_test)
