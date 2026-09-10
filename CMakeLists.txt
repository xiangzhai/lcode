cmake_minimum_required(VERSION 3.16)
project(lcode C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_DEBUG "-g -O0")

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(READLINE REQUIRED IMPORTED_TARGET readline)

add_executable(lcode
  src/main.c
  src/agent.c
  src/http.c
  src/tools.c
  src/session.c
)

target_include_directories(lcode PRIVATE include)
target_compile_definitions(lcode PRIVATE _GNU_SOURCE)
target_compile_options(lcode PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(lcode PRIVATE
  PkgConfig::CURL
  PkgConfig::JSONC
  PkgConfig::READLINE
)
