cmake_minimum_required(VERSION 3.13)
project(lcode C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_C_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_DEBUG "-g -O0")

find_package(PkgConfig REQUIRED)
find_package(Readline REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

add_executable(lcode
  src/main.c
  src/agent.c
  src/http.c
  src/tools.c
  src/session.c
  src/skills.c
  src/policy.c
  src/strbuf.c
  src/trust.c
  src/interrupt.c
)

target_include_directories(lcode PRIVATE include)
target_compile_definitions(lcode PRIVATE _GNU_SOURCE)
target_compile_options(lcode PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(lcode PRIVATE
  PkgConfig::CURL
  PkgConfig::JSONC
  Threads::Threads
  ${READLINE_LIBRARY}
)

option(LCODE_BUILD_TESTS "Build lcode unit tests" OFF)

if(LCODE_BUILD_TESTS)
  enable_testing()

  add_executable(test_policy
    test/test_policy.c
    src/policy.c
    src/trust.c
    src/strbuf.c
  )
  target_include_directories(test_policy PRIVATE include)
  target_compile_definitions(test_policy PRIVATE _GNU_SOURCE)
  target_compile_options(test_policy PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(test_policy PRIVATE PkgConfig::JSONC)

  add_test(NAME policy COMMAND test_policy)
endif()
