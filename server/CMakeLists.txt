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
  src/http_parser.cpp
  src/read_iterator.cpp
  src/http_error.cpp
  src/trie.cpp
  src/server.cpp
)
add_library(coro_http_server::coro_http_server ALIAS coro_http_server)
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

if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(CORO_HTTP_SERVER_BUILD_TESTS_DEFAULT ON)
else()
  set(CORO_HTTP_SERVER_BUILD_TESTS_DEFAULT OFF)
endif()
option(CORO_HTTP_SERVER_BUILD_TESTS "Build coro_http_server tests" ${CORO_HTTP_SERVER_BUILD_TESTS_DEFAULT})

if (CORO_HTTP_SERVER_BUILD_TESTS)
  include(CTest)
  if (BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(http_parser_test tests/http_parser_test.cpp)
    target_link_libraries(http_parser_test PRIVATE coro_http_server::coro_http_server)
    set_target_properties(http_parser_test PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
    )
    add_test(NAME http_parser_test COMMAND http_parser_test)

    add_executable(stack_future_test tests/stack_future_test.cpp)
    target_link_libraries(stack_future_test PRIVATE coro_http_server::coro_http_server)
    set_target_properties(stack_future_test PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
    )
    add_test(NAME stack_future_test COMMAND stack_future_test)

    add_executable(http_integration_test tests/http_integration_test.cpp)
    target_link_libraries(http_integration_test PRIVATE coro_http_server::coro_http_server Threads::Threads)
    set_target_properties(http_integration_test PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/tests
    )
    add_test(NAME http_integration_test COMMAND http_integration_test)
  endif()
endif()
