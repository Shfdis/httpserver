cmake_minimum_required(VERSION 3.12)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(coro_http_server CXX)
set(BIN_OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/../bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BIN_OUTPUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${BIN_OUTPUT_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${BIN_OUTPUT_DIR})
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER ${cfg} cfg_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} ${BIN_OUTPUT_DIR})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfg_upper} ${BIN_OUTPUT_DIR})
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfg_upper} ${BIN_OUTPUT_DIR})
endforeach()
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
add_library(coro_http_server
  src/io_uring.cpp
  src/read_iterator.cpp
  src/http_error.cpp
  src/trie.cpp
  src/server.cpp
)
target_compile_options(coro_http_server PUBLIC -fmodules)
target_include_directories(coro_http_server PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${LIBURING_INCLUDE_DIRS}
)
target_compile_options(coro_http_server PRIVATE -O3 -g -fno-omit-frame-pointer)
if (ENABLE_ASAN)
  target_compile_options(coro_http_server PRIVATE -fsanitize=address)
  target_link_options(coro_http_server PUBLIC -fsanitize=address)
endif()
target_link_libraries(coro_http_server PUBLIC ${LIBURING_LIBRARIES})

include(GNUInstallDirs)
install(TARGETS coro_http_server EXPORT MyServerConfig DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT MyServerConfig FILE coro_http_serverConfig.cmake NAMESPACE coro_http_server:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coro_http_server)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
