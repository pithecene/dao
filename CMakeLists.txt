cmake_minimum_required(VERSION 3.30)
project(dao LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ISO C++ requires designated initializers in declaration order; out of
# order they are a clang extension, and the code then reads as if the
# fields were evaluated in the order written when they are not. Clang
# already warns -- promoted here so a warning-clean build stays the bar.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror=reorder-init-list>)

enable_testing()

add_subdirectory(compiler/frontend)
add_subdirectory(compiler/ir)
add_subdirectory(compiler/backend)
add_subdirectory(compiler/analysis)
add_subdirectory(compiler/driver)
add_subdirectory(runtime)
add_subdirectory(tools/playground)
