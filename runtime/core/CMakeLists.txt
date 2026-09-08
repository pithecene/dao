add_library(dao_runtime STATIC
  io.c
  equality.c
  convert.c
  overflow.c
  generator.c
  resource.c
  string.c
  panic.c
  ${CMAKE_SOURCE_DIR}/runtime/memory/alloc.c
)

# This is C code, not C++.
set_target_properties(dao_runtime PROPERTIES
  LINKER_LANGUAGE C
)

# Allow runtime sources to include dao_abi.h via "dao_abi.h".
target_include_directories(dao_runtime PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Tests
find_package(ut REQUIRED)

add_executable(overflow_test
  overflow_test.cpp
)

set_target_properties(overflow_test PROPERTIES
  LINKER_LANGUAGE CXX
)

target_link_libraries(overflow_test
  PRIVATE
    dao_runtime
    Boost::ut
)

target_include_directories(overflow_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(convert_test
  convert_test.cpp
)

set_target_properties(convert_test PROPERTIES
  LINKER_LANGUAGE CXX
)

target_link_libraries(convert_test
  PRIVATE
    dao_runtime
    Boost::ut
)

target_include_directories(convert_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(alloc_test
  ${CMAKE_SOURCE_DIR}/runtime/memory/alloc_test.cpp
)

set_target_properties(alloc_test PROPERTIES
  LINKER_LANGUAGE CXX
)

target_link_libraries(alloc_test
  PRIVATE
    dao_runtime
    Boost::ut
)

target_include_directories(alloc_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

include(CTest)
add_test(NAME overflow_test COMMAND overflow_test)
add_test(NAME convert_test COMMAND convert_test)
add_test(NAME alloc_test COMMAND alloc_test)
