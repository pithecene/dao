find_package(httplib REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(ut REQUIRED)

# Transport-free service: every route as a JSON function.  Shared by the
# HTTP server and the service test.
add_library(dao_playground_service STATIC
  analyze.cpp
  completions.cpp
  examples.cpp
  navigation.cpp
  pipeline.cpp
  run.cpp
  service.cpp
  service_surface.cpp
)

target_include_directories(dao_playground_service PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(dao_playground_service PUBLIC
  dao_analysis
  dao_backend_llvm
  dao_ir
  dao_frontend
  nlohmann_json::nlohmann_json
)

target_compile_definitions(dao_playground_service PRIVATE
  DAO_RUNTIME_LIB="$<TARGET_FILE:dao_runtime>"
)

# The run route links produced objects against the runtime at request
# time; building the service alone must build the runtime too.
add_dependencies(dao_playground_service dao_runtime)

# Emits the TypeScript the playground frontend compiles against.
add_executable(service_surface_dump
  service_surface_dump.cpp
)

target_link_libraries(service_surface_dump PRIVATE dao_playground_service)

add_executable(dao_playground
  main.cpp
)

target_link_libraries(dao_playground PRIVATE
  dao_playground_service
  httplib::httplib
)

target_compile_definitions(dao_playground PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# Drives every route over every example in-process: response shapes,
# full token classification, navigation answers, and run goldens.
add_executable(service_test
  service_test.cpp
)

target_link_libraries(service_test PRIVATE dao_playground_service Boost::ut)

target_compile_definitions(service_test PRIVATE
  DAO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

include(CTest)
add_test(NAME playground_service_test COMMAND service_test)
set_tests_properties(playground_service_test PROPERTIES TIMEOUT 900)
