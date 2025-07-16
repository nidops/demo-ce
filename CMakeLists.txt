# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(demo-ce)

# --------------------------------------------------------------------
# Add CEVO as a subproject - must come BEFORE referencing 'cevo'
# --------------------------------------------------------------------
set(CEVO_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../cevo)

if(NOT EXISTS ${CEVO_PATH}/CMakeLists.txt)
    message(FATAL_ERROR "Missing CEVO source at ${CEVO_PATH}. 
Did you forget to clone it or run 'git submodule update --init'?")
endif()

add_subdirectory(${CEVO_PATH} ${CMAKE_BINARY_DIR}/cevo_build)

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
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc)

# --------------------------------------------------------------------
# Link CEVO to this app
# --------------------------------------------------------------------
target_link_libraries(app PRIVATE cevo)

# --------------------------------------------------------------------
# App source files
# --------------------------------------------------------------------
target_sources(app PRIVATE
    src/main.c
    src/demo.c
)
