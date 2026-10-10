# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(demo-ce)

set(CEVO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cevo" CACHE PATH "Path to the CEVO command engine source tree")

if(NOT EXISTS "${CEVO_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing CEVO source at ${CEVO_PATH}. "
        "Clone cevo next to demo-ce or set CEVO_PATH.")
endif()

add_subdirectory("${CEVO_PATH}" "${CMAKE_BINARY_DIR}/cevo_build")

target_link_libraries(cevo PRIVATE zephyr_interface)
target_include_directories(cevo PRIVATE inc)
target_include_directories(app PRIVATE inc)
target_link_libraries(app PRIVATE cevo)

target_sources(app PRIVATE
    src/main.c
    src/uart_line.c
    src/demo.c
)
