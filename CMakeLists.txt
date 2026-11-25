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

include(GNUInstallDirs)

install(TARGETS daoc
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(TARGETS dao_runtime
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY stdlib/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/dao/stdlib
  FILES_MATCHING PATTERN "*.dao"
)
install(FILES LICENSE THIRD_PARTY_NOTICES.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)

set(CPACK_PACKAGE_NAME "dao")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
