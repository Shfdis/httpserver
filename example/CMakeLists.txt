cmake_minimum_required(VERSION 3.12)

set(CMAKE_CXX_STANDARD 20)
project(example_echo_server CXX)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../coro_server coro_server_build)
add_executable(echo_server main.cpp)
target_compile_options(echo_server PRIVATE -O3 -g -fno-omit-frame-pointer)
if (ENABLE_ASAN)
  target_compile_options(echo_server PRIVATE -fsanitize=address)
  target_link_options(echo_server PRIVATE -fsanitize=address)
endif()
target_link_libraries(echo_server PRIVATE coro_http_server)
