cmake_minimum_required(VERSION 3.12)
project(example_echo_server CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build and install dependency locally
get_filename_component(SERVER_DIR "${CMAKE_CURRENT_LIST_DIR}/../server" ABSOLUTE)
execute_process(COMMAND ${CMAKE_COMMAND} -S "${SERVER_DIR}" -B build_dep -DCMAKE_INSTALL_PREFIX=install_dep -DCMAKE_CXX_STANDARD=20)
execute_process(COMMAND ${CMAKE_COMMAND} --build build_dep --target install)

set(CMAKE_PREFIX_PATH ${CMAKE_CURRENT_BINARY_DIR}/install_dep)
find_package(coro_http_server REQUIRED)

add_executable(echo_server main.cpp)
target_link_libraries(echo_server PRIVATE coro_http_server::coro_http_server)
