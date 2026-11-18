# CMake toolchain file for SavanXP programs, built against the SDK sysroot
# (docs/roadmaps/done/SDK_V2_ROADMAP.md, phase 5). It lives in the sysroot's
# share/sdk/ and compiles through sxcc next to it:
#
#   cmake -S myapp -B build -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=<sysroot>/share/sdk/savanxp-toolchain.cmake \
#       [-DSAVANXP_SDK_PROFILE=nosse|sse|pie] [-DSAVANXP_SDK_NATIVE=ON]
#
# A target links the runtime on its own; the other groups are libraries it
# names: target_link_libraries(app PRIVATE sxgui m). The executable is a bare
# ELF image: SXE resources and the native OSABI are stamped by the tools in
# share/sdk/, as build-user.sh does.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT SAVANXP_SDK_PROFILE)
    set(SAVANXP_SDK_PROFILE sse)
endif()
# try_compile runs in a project of its own; these reach it.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SAVANXP_SDK_PROFILE SAVANXP_SDK_NATIVE)

set(CMAKE_C_COMPILER "${CMAKE_CURRENT_LIST_DIR}/sxcc")
set(CMAKE_ASM_COMPILER "${CMAKE_CURRENT_LIST_DIR}/sxcc")
set(savanxp_sxcc_flags "--sx-profile=${SAVANXP_SDK_PROFILE}")
if(SAVANXP_SDK_NATIVE)
    string(APPEND savanxp_sxcc_flags " --sx-native")
endif()
# CMAKE_C_FLAGS reaches the link line too, so the profile applies to both.
set(CMAKE_C_FLAGS_INIT "${savanxp_sxcc_flags}")
set(CMAKE_ASM_FLAGS_INIT "${savanxp_sxcc_flags}")

get_filename_component(SAVANXP_SDK_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_FIND_ROOT_PATH "${SAVANXP_SDK_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
