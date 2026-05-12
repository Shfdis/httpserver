#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "coro_http_server::coro_http_server" for configuration "Release"
set_property(TARGET coro_http_server::coro_http_server APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(coro_http_server::coro_http_server PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libcoro_http_server.a"
  )

list(APPEND _cmake_import_check_targets coro_http_server::coro_http_server )
list(APPEND _cmake_import_check_files_for_coro_http_server::coro_http_server "${_IMPORT_PREFIX}/lib/libcoro_http_server.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
