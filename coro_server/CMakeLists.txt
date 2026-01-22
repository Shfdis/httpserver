cmake_minimum_required(VERSION 3.12)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(coro_http_server CXX)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
add_library(coro_http_server
  io_uring.cpp 
  read_iterator.cpp 
  http_error.cpp 
  trie.cpp 
  server.cpp
)
target_compile_options(coro_http_server PUBLIC -fmodules)
target_include_directories(coro_http_server PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBURING_INCLUDE_DIRS}
)
target_compile_options(coro_http_server PRIVATE -O3 -g -fno-omit-frame-pointer)
if (ENABLE_ASAN)
  target_compile_options(coro_http_server PRIVATE -fsanitize=address)
  target_link_options(coro_http_server PUBLIC -fsanitize=address)
endif()
target_link_libraries(coro_http_server PUBLIC ${LIBURING_LIBRARIES})
