cmake_minimum_required(VERSION 3.25)

# Build freestanding i386 products with host Clang and hosted test programs.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i386)
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
