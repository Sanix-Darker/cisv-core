cmake_minimum_required(VERSION 3.14)
project(cisv C CXX)

include(CheckCCompilerFlag)

# =============================================================================
# Profile-Guided Optimization (PGO) Support
# Usage:
#   1. cmake -DCISV_PGO_GENERATE=ON .. && make
#   2. Run representative workload
#   3. cmake -DCISV_PGO_USE=ON .. && make
# =============================================================================
option(CISV_PGO_GENERATE "Generate PGO profile data" OFF)
option(CISV_PGO_USE "Use PGO profile data for optimization" OFF)

if(CISV_PGO_GENERATE)
    message(STATUS "PGO: Generating profile data")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-generate")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-generate")
endif()

if(CISV_PGO_USE)
    message(STATUS "PGO: Using profile data for optimization")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-use -fprofile-correction")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-use -fprofile-correction")
endif()

# Core library sources
set(CISV_SOURCES
    src/parser.c
    src/writer.c
    src/transformer.c
)

set(CISV_COMMON_COMPILE_OPTIONS
    -O3
    -pipe
    -fomit-frame-pointer
    -Wall
    -Wextra
    -ffast-math
    -funroll-loops
    -fPIC
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CISV_COMMON_COMPILE_OPTIONS -fstack-check)
endif()

set(CISV_ARCH_COMPILE_OPTIONS)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    check_c_compiler_flag("-march=native" CISV_HAS_MARCH_NATIVE)
    if(CISV_HAS_MARCH_NATIVE)
        list(APPEND CISV_ARCH_COMPILE_OPTIONS -march=native)
    endif()
    check_c_compiler_flag("-mavx2" CISV_HAS_MAVX2)
    if(CISV_HAS_MAVX2)
        list(APPEND CISV_ARCH_COMPILE_OPTIONS -mavx2)
    endif()
    check_c_compiler_flag("-mtune=native" CISV_HAS_MTUNE_NATIVE)
    if(CISV_HAS_MTUNE_NATIVE)
        list(APPEND CISV_ARCH_COMPILE_OPTIONS -mtune=native)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    if(NOT APPLE)
        check_c_compiler_flag("-mcpu=native" CISV_HAS_MCPU_NATIVE)
        if(CISV_HAS_MCPU_NATIVE)
            list(APPEND CISV_ARCH_COMPILE_OPTIONS -mcpu=native)
        else()
            check_c_compiler_flag("-march=native" CISV_HAS_ARM_MARCH_NATIVE)
            if(CISV_HAS_ARM_MARCH_NATIVE)
                list(APPEND CISV_ARCH_COMPILE_OPTIONS -march=native)
            endif()
        endif()
    endif()
endif()

# Static library
add_library(cisv_static STATIC ${CISV_SOURCES})
target_include_directories(cisv_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(cisv_static PRIVATE ${CISV_COMMON_COMPILE_OPTIONS} ${CISV_ARCH_COMPILE_OPTIONS})
set_target_properties(cisv_static PROPERTIES OUTPUT_NAME cisv)

# Shared library
add_library(cisv_shared SHARED ${CISV_SOURCES})
target_include_directories(cisv_shared PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(cisv_shared PRIVATE ${CISV_COMMON_COMPILE_OPTIONS} ${CISV_ARCH_COMPILE_OPTIONS})
set_target_properties(cisv_shared PROPERTIES OUTPUT_NAME cisv)

# Link pthread for parallel parsing support
find_package(Threads REQUIRED)
target_link_libraries(cisv_static PRIVATE Threads::Threads)
target_link_libraries(cisv_shared PRIVATE Threads::Threads)

# Platform-specific settings
if(UNIX AND NOT APPLE)
    target_compile_definitions(cisv_static PRIVATE _GNU_SOURCE)
    target_compile_definitions(cisv_shared PRIVATE _GNU_SOURCE)
endif()

if(APPLE)
    target_compile_definitions(cisv_static PRIVATE __APPLE__)
    target_compile_definitions(cisv_shared PRIVATE __APPLE__)
endif()

# Tests
if(CISV_BUILD_TESTS)
    add_executable(test_core tests/test_core.c)
    target_link_libraries(test_core PRIVATE cisv_static m pthread)
    target_include_directories(test_core PRIVATE include)

    enable_testing()
    add_test(NAME CoreTests COMMAND test_core)
endif()

# Installation
include(GNUInstallDirs)

install(TARGETS cisv_static cisv_shared
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/cisv
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
