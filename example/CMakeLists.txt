cmake_minimum_required(VERSION 3.12)

set(CMAKE_CXX_STANDARD 20)
project(example_echo_server CXX)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../server server_build)
add_executable(echo_server main.cpp)
target_compile_options(echo_server PRIVATE -O3)
target_link_libraries(echo_server PRIVATE http_server)
