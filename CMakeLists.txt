# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.16)
project(chaining VERSION 0.1.0 LANGUAGES C)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(CHAIN_IS_TOP_LEVEL ON)
else()
  set(CHAIN_IS_TOP_LEVEL OFF)
endif()

option(CHAIN_BUILD_EXAMPLES "Build chaining example binaries" ${CHAIN_IS_TOP_LEVEL})
option(CHAIN_BUILD_TESTS "Build chaining unit test suites" ${CHAIN_IS_TOP_LEVEL})
option(CHAIN_ENABLE_SQLITE "Build SQLite3 Db extension when SQLite3 is available" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS ON)

# =========================================================
# Libraries
# =========================================================

add_library(core_lib
  core/boolean.c
  core/hash.c
  core/json.c
  core/null.c
  core/number.c
  core/slice.c
  core/status_or.c
  core/string.c
  core/typed_array.c
)
add_library(chaining::core ALIAS core_lib)

target_include_directories(core_lib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

if(UNIX)
  target_link_libraries(core_lib PUBLIC m)
endif()

set(CHAINING_EXT_SRCS
  ext/mcp.c
  ext/server.c
  ext/test.c
)

set(CHAIN_HAS_SQLITE FALSE)
if(CHAIN_ENABLE_SQLITE)
  find_package(SQLite3 QUIET)
  if(SQLite3_FOUND)
    set(CHAIN_HAS_SQLITE TRUE)
    list(APPEND CHAINING_EXT_SRCS ext/db.c)
  else()
    message(STATUS "SQLite3 not found: building chaining_lib without ext/db.c")
  endif()
endif()

add_library(chaining_lib ${CHAINING_EXT_SRCS})
add_library(chaining::chaining ALIAS chaining_lib)

target_include_directories(chaining_lib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(chaining_lib PUBLIC core_lib)
if(CHAIN_HAS_SQLITE)
  target_link_libraries(chaining_lib PUBLIC SQLite::SQLite3)
endif()

# =========================================================
# Single-Header Amalgamation Target (`chain_single.h`)
# =========================================================

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(CHAIN_SINGLE_HEADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/chain_single.h")
  set(CHAIN_ALL_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/chain.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/boolean.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/boolean.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/hash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/hash.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/json.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/json.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/null.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/null.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/number.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/number.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/slice.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/slice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/status_or.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/status_or.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/string.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/string.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/typed_array.c
    ${CMAKE_CURRENT_SOURCE_DIR}/core/typed_array.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/types.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/db.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/db.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/mcp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/mcp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ext/test.h
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_single_header.py
  )

  add_custom_command(
    OUTPUT "${CHAIN_SINGLE_HEADER_OUT}"
    COMMAND Python3::Interpreter
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_single_header.py"
      --root "${CMAKE_CURRENT_SOURCE_DIR}"
      --output "${CHAIN_SINGLE_HEADER_OUT}"
    DEPENDS ${CHAIN_ALL_SOURCES}
    COMMENT "Generating single-header amalgamation ${CHAIN_SINGLE_HEADER_OUT}"
    VERBATIM
  )

  add_custom_target(chain_single_header ALL DEPENDS "${CHAIN_SINGLE_HEADER_OUT}")
  add_custom_target(single_header DEPENDS "${CHAIN_SINGLE_HEADER_OUT}")

  add_library(chain_single_lib INTERFACE)
  add_dependencies(chain_single_lib chain_single_header)
  target_include_directories(chain_single_lib
    INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
      $<INSTALL_INTERFACE:include>
  )
  if(UNIX)
    target_link_libraries(chain_single_lib INTERFACE m)
  endif()
endif()

# =========================================================
# Examples
# =========================================================

if(CHAIN_BUILD_EXAMPLES)
  add_executable(chain examples/chain.c)
  target_link_libraries(chain PRIVATE core_lib)

  add_executable(chain_nomacro examples/chain_nomacro.c)
  target_link_libraries(chain_nomacro PRIVATE core_lib)

  add_executable(chain_hash examples/chain_hash.c)
  target_link_libraries(chain_hash PRIVATE core_lib)

  add_executable(chain_nest examples/chain_nest.c)
  target_link_libraries(chain_nest PRIVATE core_lib)

  add_executable(chain_typed_array examples/chain_typed_array.c)
  target_link_libraries(chain_typed_array PRIVATE core_lib)

  add_executable(chain_server examples/chain_server.c)
  target_link_libraries(chain_server PRIVATE chaining_lib core_lib)

  add_executable(chain_mcp examples/chain_mcp.c)
  target_link_libraries(chain_mcp PRIVATE chaining_lib core_lib)

  if(CHAIN_HAS_SQLITE)
    add_executable(chain_db examples/chain_db.c)
    target_link_libraries(chain_db PRIVATE chaining_lib core_lib)

    add_executable(chain_vector_db examples/chain_vector_db.c)
    target_link_libraries(chain_vector_db PRIVATE chaining_lib core_lib)

    add_executable(server_bench benchmarks/server_bench.c)
    target_link_libraries(server_bench PRIVATE chaining_lib core_lib)
  endif()
endif()

# =========================================================
# Unit Tests
# =========================================================

if(CHAIN_BUILD_TESTS)
  enable_testing()

  set(CHAIN_CORE_TEST_TARGETS
    core_primitives_test
    containers_test
    json_status_test
    server_test
  )

  foreach(test_name IN LISTS CHAIN_CORE_TEST_TARGETS)
    add_executable(${test_name} tests/${test_name}.c)
    target_link_libraries(${test_name} PRIVATE chaining_lib core_lib)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()

  if(CHAIN_HAS_SQLITE)
    add_executable(db_test tests/db_test.c)
    target_link_libraries(db_test PRIVATE chaining_lib core_lib)
    add_test(NAME db_test COMMAND db_test)

    add_executable(mcp_test tests/mcp_test.c)
    target_link_libraries(mcp_test PRIVATE chaining_lib core_lib)
    add_test(NAME mcp_test COMMAND mcp_test)
  endif()

  if(TARGET chain_single_lib)
    add_executable(single_header_test tests/single_header_test.c)
    target_link_libraries(single_header_test PRIVATE chain_single_lib)
    if(CHAIN_HAS_SQLITE)
      target_link_libraries(single_header_test PRIVATE SQLite::SQLite3)
    else()
      target_compile_definitions(single_header_test PRIVATE CHAIN_NO_DB)
    endif()
    add_test(NAME single_header_test COMMAND single_header_test)
  endif()
endif()

# =========================================================
# Install Rules
# =========================================================

include(GNUInstallDirs)

install(TARGETS core_lib chaining_lib
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES chain.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY core ext
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h"
)

if(DEFINED CHAIN_SINGLE_HEADER_OUT)
  install(FILES "${CHAIN_SINGLE_HEADER_OUT}"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
endif()
