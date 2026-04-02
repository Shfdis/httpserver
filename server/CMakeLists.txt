cmake_minimum_required(VERSION 3.12)
project(coro_http_server CXX)
set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

file(GLOB SRCS *.cpp)
file(GLOB HDRS *.h)

add_library(coro_http_server ${SRCS})
target_include_directories(coro_http_server PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
    ${LIBURING_INCLUDE_DIRS}
)
target_link_libraries(coro_http_server PUBLIC ${LIBURING_LIBRARIES})

include(GNUInstallDirs)
install(TARGETS coro_http_server EXPORT MyServerConfig DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT MyServerConfig FILE coro_http_serverConfig.cmake NAMESPACE coro_http_server:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coro_http_server)
install(FILES ${HDRS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
