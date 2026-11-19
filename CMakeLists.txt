cmake_minimum_required(VERSION 3.25)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)
find_package(cmake-napi REQUIRED PATHS node_modules/cmake-napi)
find_package(cmake-npm REQUIRED PATHS node_modules/cmake-npm)

project(pty_native C)

add_bare_module(pty_native_bare)

target_sources(
  ${pty_native_bare}
  PRIVATE
    binding.c
)

add_napi_module(pty_native_node)

target_sources(
  ${pty_native_node}
  PRIVATE
    binding.c
)

resolve_node_module(bare-compat-napi compat)

target_include_directories(
  ${pty_native_node}
  PRIVATE
    "${compat}/include"
)

# forkpty() lives in libutil on glibc
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_libraries(${pty_native_bare} PRIVATE util)
  target_link_libraries(${pty_native_node} PRIVATE util)
endif()
