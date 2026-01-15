cmake_minimum_required(VERSION 3.12)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(http_server CXX)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)
add_library(http_server read_iterator.cpp io_uring.cpp http_server.cpp trie.cpp http_error.cpp mpsq_queue.cpp)
  target_compile_options(http_server PUBLIC -fmodules)
target_include_directories(http_server PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBURING_INCLUDE_DIRS}
)
target_link_libraries(http_server PUBLIC ${LIBURING_LIBRARIES})
