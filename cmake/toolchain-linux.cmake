cmake_minimum_required(VERSION 3.25)

# The target is freestanding, but the project itself is configured and run on
# a Linux host. CMake must not try to link a hosted probe executable. The
# processor follows the graph's arch axis; SAVANXP_ARCH is a cache variable, so
# it already exists when this file is read.
set(CMAKE_SYSTEM_NAME Linux)
if(DEFINED CACHE{SAVANXP_ARCH} AND SAVANXP_ARCH STREQUAL "i386")
    set(CMAKE_SYSTEM_PROCESSOR i386)
else()
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang CACHE FILEPATH "C compiler")
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++ CACHE FILEPATH "C++ compiler")
endif()
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER clang CACHE FILEPATH "assembler compiler")
endif()
