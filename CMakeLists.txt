# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(demo-ce)

# --------------------------------------------------------------------
# Add CEVO as a subproject - must come BEFORE referencing 'cevo'
# --------------------------------------------------------------------
set(CEVO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cevo" CACHE PATH "Path to the CEVO command engine source tree")
set(BUFFER_POOL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../buffer-pool" CACHE PATH "Path to the buffer-pool source tree")

if(NOT EXISTS "${CEVO_PATH}/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing CEVO source at ${CEVO_PATH}. "
        "Did you forget to run 'west update' or clone cevo next to demo-ce?")
endif()

if(NOT EXISTS "${BUFFER_POOL_PATH}/buffer.h" OR NOT EXISTS "${BUFFER_POOL_PATH}/buffer.c")
    message(FATAL_ERROR
        "Missing buffer-pool source at ${BUFFER_POOL_PATH}. "
        "Did you forget to run 'west update' or clone buffer-pool next to demo-ce?")
endif()

add_subdirectory("${CEVO_PATH}" "${CMAKE_BINARY_DIR}/cevo_build")

# --------------------------------------------------------------------
# Inherit Zephyr toolchain flags and definitions for CEVO
# --------------------------------------------------------------------
target_include_directories(cevo
    PRIVATE
    ${ZEPHYR_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)

target_compile_options(cevo PRIVATE ${ZEPHYR_EXTRA_CFLAGS})
target_compile_definitions(cevo PRIVATE ${ZEPHYR_DEFINES})

# --------------------------------------------------------------------
# Application include paths
# --------------------------------------------------------------------
target_include_directories(app
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${BUFFER_POOL_PATH}
)

# --------------------------------------------------------------------
# Link CEVO to this app
# --------------------------------------------------------------------
target_link_libraries(app PRIVATE cevo)

# --------------------------------------------------------------------
# App source files
# --------------------------------------------------------------------
target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/uart_line.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/demo.c
    ${BUFFER_POOL_PATH}/buffer.c
)
