cmake_minimum_required(VERSION 3.25)

project(SavanXP
    VERSION 0.4.0
    DESCRIPTION "SavanXP operating system"
    LANGUAGES C CXX ASM
)

include(CTest)

option(SAVANXP_INCLUDE_TEST_APPS "Build diagnostic and test userland applications" ON)
set(SAVANXP_ACCEL "tcg" CACHE STRING "QEMU accelerator: tcg or kvm")
if(NOT SAVANXP_ACCEL STREQUAL "tcg" AND NOT SAVANXP_ACCEL STREQUAL "kvm")
    message(FATAL_ERROR "SAVANXP_ACCEL must be tcg or kvm")
endif()
set(SAVANXP_SMP 1 CACHE STRING "Number of virtual CPUs")
option(SAVANXP_HEADLESS "Run QEMU without a graphical display" OFF)
# This branch ports SavanXP to i386; the x86_64 system lives on master.
set(SAVANXP_ARCH "i386" CACHE STRING "Target architecture (this branch: i386)")
set_property(CACHE SAVANXP_ARCH PROPERTY STRINGS i386)
if(NOT SAVANXP_ARCH STREQUAL "i386")
    message(FATAL_ERROR "This branch supports i386 only. Use master for x86_64 and a separate build directory.")
endif()
if(NOT SAVANXP_SMP EQUAL 1)
    message(FATAL_ERROR "The i386 kernel currently supports one CPU only.")
endif()
set(SAVANXP_OUTPUT_ROOT "${CMAKE_SOURCE_DIR}/build" CACHE PATH
    "Output root (staging, images and persistent disk)")
set(SAVANXP_SXFS_SECTORS 2097152 CACHE STRING "Size of the persistent SxFS image in sectors")
set(SAVANXP_GENERATED_ROOT "${SAVANXP_OUTPUT_ROOT}/generated")
set(SAVANXP_DISKFS_ROOT "${SAVANXP_OUTPUT_ROOT}/diskfs")
set(SAVANXP_ISO_ROOT "${SAVANXP_OUTPUT_ROOT}/iso-root")
set(SAVANXP_DISK_IMAGE "${SAVANXP_OUTPUT_ROOT}/disk.img")
set(SAVANXP_KERNEL_ELF "${SAVANXP_OUTPUT_ROOT}/kernel.elf")
set(SAVANXP_ISO_IMAGE "${SAVANXP_OUTPUT_ROOT}/SavanXP.iso")
set(SAVANXP_TOOLS_ROOT "${SAVANXP_OUTPUT_ROOT}/tools")
set(SAVANXP_SXFS_CLI "${SAVANXP_TOOLS_ROOT}/sxfs-cli")

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
find_program(SAVANXP_HOST_CC NAMES clang cc REQUIRED)
find_program(SAVANXP_LD NAMES ld.lld REQUIRED)
find_program(SAVANXP_OBJCOPY NAMES llvm-objcopy REQUIRED)
find_program(SAVANXP_READELF NAMES llvm-readelf REQUIRED)
find_program(SAVANXP_XORRISO NAMES xorriso)
if(NOT SAVANXP_XORRISO)
    set(SAVANXP_XORRISO xorriso)
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import PIL"
    RESULT_VARIABLE SAVANXP_PIL_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
)
if(NOT SAVANXP_PIL_RESULT EQUAL 0)
    message(FATAL_ERROR "Python Pillow is required to generate SavanXP assets.")
endif()

execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE SAVANXP_BUILD_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE SAVANXP_GIT_RESULT
)
if(NOT SAVANXP_GIT_RESULT EQUAL 0 OR NOT SAVANXP_BUILD_ID)
    set(SAVANXP_BUILD_ID unknown)
endif()

set(SAVANXP_TARGET_TRIPLE i386-unknown-none-elf)
set(SAVANXP_LINK_EMULATION elf_i386)
set(SAVANXP_KERNEL_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/linker32.ld")
set(SAVANXP_USER_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/linker32.ld")

set(SAVANXP_KERNEL_COMPILE_OPTIONS
    "-target;${SAVANXP_TARGET_TRIPLE}"
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fstack-protector-strong
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mgeneral-regs-only
    -Wall
    -Wextra
    -Wpedantic
)
set(SAVANXP_USER_COMPILE_OPTIONS
    "-target;${SAVANXP_TARGET_TRIPLE}"
    -ffreestanding
    -fstack-protector-strong
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mno-mmx
    -msse
    -msse2
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Wpedantic
    -Wno-language-extension-token
    -Wno-c23-extensions
)
set(SAVANXP_USER_LINK_OPTIONS_STATIC
    -nostdlib
    -static
    -fuse-ld=lld
    "-Wl,-m,${SAVANXP_LINK_EMULATION}"
    "-Wl,-T,${SAVANXP_USER_LINKER_SCRIPT}"
    "-Wl,-z,max-page-size=0x1000"
    -Wl,--gc-sections
    -Wl,--build-id=none
)
set(SAVANXP_KERNEL_ARCH_SOURCES
    arch/x86/entry32.S
    arch/x86/process.cpp
    arch/x86/context.cpp
    arch/x86/interrupts32.S
    arch/x86/serial.cpp
    arch/x86/egatext.cpp
    arch/x86/fb32.cpp
    arch/x86/pit.cpp
    arch/x86/paging.cpp
    arch/x86/cpu.cpp
    arch/x86/syscall.cpp
    arch/x86/user32.cpp
    arch/x86/ps2_32.cpp
    arch/x86/main32.cpp
)
# These paths execute on a bounded per-process kernel stack. Reject large
# Debug aggregate temporaries before they can corrupt another task.
set_source_files_properties(
    arch/x86/context.cpp arch/x86/process.cpp arch/x86/user32.cpp arch/x86/syscall.cpp
    PROPERTIES COMPILE_OPTIONS "-Wframe-larger-than=4096;-Werror=frame-larger-than"
)
# Reused storage modules; other subsystems enter the graph as they are ported.
set(SAVANXP_KERNEL_SOURCES ${SAVANXP_KERNEL_ARCH_SOURCES}
    kernel/ata.cpp kernel/block.cpp kernel/partition.cpp kernel/fs.cpp
    kernel/sxfs.cpp kernel/vfs.cpp kernel/runtime.cpp)

add_library(savanxp_kernel_objects OBJECT ${SAVANXP_KERNEL_SOURCES})
target_include_directories(savanxp_kernel_objects PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/vendor"
    "${CMAKE_SOURCE_DIR}/vendor/uacpi/include"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp_kernel_objects PRIVATE ${SAVANXP_KERNEL_COMPILE_OPTIONS})
set_target_properties(savanxp_kernel_objects PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

# The SDK runtime is compiled as ELF32; unused units are discarded at link.
set(SAVANXP_USER_RUNTIME_SOURCES
    subsystems/posix/sdk/v1/runtime/crt0.S
    subsystems/posix/sdk/v1/runtime/libc.c
    subsystems/posix/sdk/v1/runtime/posix.c
    subsystems/posix/sdk/v1/runtime/math.c
    subsystems/posix/sdk/v1/runtime/gfx.c
    subsystems/posix/sdk/v1/runtime/gfx2d.c
    subsystems/posix/sdk/v1/runtime/sxchrome.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c
    subsystems/posix/sdk/v1/runtime/sxe.c
    subsystems/posix/sdk/v1/runtime/audio.c
    subsystems/posix/sdk/v1/runtime/sxmidi.c
)
# Desktop icon PNGs are checked-in, hand-editable source assets. Keep the
# generator as an explicit target, but never let a normal build redraw or
# overwrite artwork that an app author has replaced.
add_custom_target(savanxp_desktop_source_art
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_desktop_source_art.py"
            --project-root "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Generating optional desktop source art"
    VERBATIM
)
set(SAVANXP_BOOT_LOGO_HEADER "${SAVANXP_GENERATED_ROOT}/boot_logo.h")
set(SAVANXP_BRAND_LOGO_HEADER "${SAVANXP_GENERATED_ROOT}/brand_logo.h")
set(SAVANXP_CURSOR_HEADER "${SAVANXP_GENERATED_ROOT}/cursor_asset.h")
set(SAVANXP_DESKTOP_ICONS_HEADER "${SAVANXP_GENERATED_ROOT}/desktop_icon_assets.h")
set(SAVANXP_START_LOGO_HEADER "${SAVANXP_GENERATED_ROOT}/start_logo.h")
set(SAVANXP_DESKTOP_ICON_SOURCES
    "${CMAKE_SOURCE_DIR}/assets/desktop/icons/16x16/desktop.png"
    "${CMAKE_SOURCE_DIR}/assets/desktop/icons/32x32/desktop.png")
file(GLOB SAVANXP_CURSOR_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/desktop/cursors/*.png")
add_custom_command(OUTPUT "${SAVANXP_BOOT_LOGO_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_GENERATED_ROOT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_boot_logo.py"
            --project-root "${CMAKE_SOURCE_DIR}" --output "${SAVANXP_BOOT_LOGO_HEADER}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/gen_boot_logo.py"
        "${CMAKE_SOURCE_DIR}/assets/brand/logo.png"
        "${CMAKE_SOURCE_DIR}/assets/desktop/fonts/NotoSans-Regular.ttf"
        "${CMAKE_SOURCE_DIR}/include/shared/version.h"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM
)
add_custom_command(OUTPUT "${SAVANXP_BRAND_LOGO_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_GENERATED_ROOT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_brand_logo.py"
            --project-root "${CMAKE_SOURCE_DIR}" --output "${SAVANXP_BRAND_LOGO_HEADER}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/gen_brand_logo.py"
        "${CMAKE_SOURCE_DIR}/assets/brand/logo.png"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM
)
add_custom_command(OUTPUT "${SAVANXP_CURSOR_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_GENERATED_ROOT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_cursor_asset.py"
            --project-root "${CMAKE_SOURCE_DIR}" --output "${SAVANXP_CURSOR_HEADER}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/gen_cursor_asset.py"
        ${SAVANXP_CURSOR_ASSETS}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM
)
add_custom_command(OUTPUT "${SAVANXP_DESKTOP_ICONS_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_GENERATED_ROOT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_desktop_icon_assets.py"
            --project-root "${CMAKE_SOURCE_DIR}" --output "${SAVANXP_DESKTOP_ICONS_HEADER}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/gen_desktop_icon_assets.py"
        ${SAVANXP_DESKTOP_ICON_SOURCES}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM
)
add_custom_command(OUTPUT "${SAVANXP_START_LOGO_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_GENERATED_ROOT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_start_logo.py"
            --project-root "${CMAKE_SOURCE_DIR}" --output "${SAVANXP_START_LOGO_HEADER}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/gen_start_logo.py"
        "${CMAKE_SOURCE_DIR}/assets/brand/start_logo.png"
        "${CMAKE_SOURCE_DIR}/assets/desktop/icons/16x16/shutdown.png"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland/mediaplayer/icons/audio-volume-high.png"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland/mediaplayer/icons/audio-volume-medium.png"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland/mediaplayer/icons/audio-volume-low.png"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland/mediaplayer/icons/audio-volume-muted.png"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM
)
set(SAVANXP_GENERATED_HEADER_FILES
    "${SAVANXP_BOOT_LOGO_HEADER}"
    "${SAVANXP_BRAND_LOGO_HEADER}"
    "${SAVANXP_CURSOR_HEADER}"
    "${SAVANXP_DESKTOP_ICONS_HEADER}"
    "${SAVANXP_START_LOGO_HEADER}"
)
set_source_files_properties(${SAVANXP_GENERATED_HEADER_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
add_custom_target(savanxp_generated_headers DEPENDS ${SAVANXP_GENERATED_HEADER_FILES})
target_sources(savanxp_kernel_objects PRIVATE ${SAVANXP_GENERATED_HEADER_FILES})

add_custom_target(savanxp_kernel
    COMMAND "${SAVANXP_LD}"
            -m "${SAVANXP_LINK_EMULATION}"
            -T "${SAVANXP_KERNEL_LINKER_SCRIPT}"
            -z max-page-size=0x1000
            --build-id=none
            -o "${SAVANXP_KERNEL_ELF}"
            $<TARGET_OBJECTS:savanxp_kernel_objects>
    COMMAND_EXPAND_LISTS
    DEPENDS savanxp_kernel_objects
    COMMENT "Linking SavanXP kernel"
    VERBATIM
)
add_dependencies(savanxp_kernel_objects savanxp_generated_headers)

# Host tests deliberately use the normal Linux compiler and hosted linker;
# they are not part of the freestanding target graph.
add_executable(savanxp-gfx2d-test
    subsystems/posix/sdk/v1/runtime/gfx2d.c
    tests/host/gfx2d_test.cpp)
target_include_directories(savanxp-gfx2d-test PRIVATE
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
set_target_properties(savanxp-gfx2d-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

add_executable(savanxp-audio-test
    subsystems/posix/sdk/v1/runtime/audio.c
    tests/host/audio_mixer_test.cpp)
target_include_directories(savanxp-audio-test PRIVATE
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
set_target_properties(savanxp-audio-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

add_executable(savanxp-kernel-audio-gain-test
    kernel/audio.cpp
    tests/host/kernel_audio_gain_test.cpp)
target_include_directories(savanxp-kernel-audio-gain-test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
target_compile_options(savanxp-kernel-audio-gain-test PRIVATE -O1 -Wall -Wextra -Wno-deprecated)
set_target_properties(savanxp-kernel-audio-gain-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

add_executable(savanxp-ccleste-wav-test
    ports/ccleste/overlay/ccleste_savanxp_assets.c
    tests/host/ccleste_wav_test.cpp)
target_include_directories(savanxp-ccleste-wav-test PRIVATE
    "${CMAKE_SOURCE_DIR}/ports/ccleste/overlay"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
target_compile_options(savanxp-ccleste-wav-test PRIVATE -O1 -Wall -Wextra -Wno-deprecated)
set_target_properties(savanxp-ccleste-wav-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

# El sintetizador se compila con el compilador de host y se ejerce de verdad:
# arma SMF, renderiza y afirma afinacion, duracion y silencio. Es la unica
# verificacion del motor; un smoke en el invitado solo probaria que carga.
add_executable(savanxp-sxmidi-test
    subsystems/posix/sdk/v1/runtime/sxmidi.c
    tests/host/sxmidi_test.cpp)
target_include_directories(savanxp-sxmidi-test PRIVATE
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
target_compile_options(savanxp-sxmidi-test PRIVATE -O1 -Wall -Wextra)
set_target_properties(savanxp-sxmidi-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

# La musica del port doomgeneric se prueba fuera de QEMU: el mus2mid de
# upstream convierte un MUS y libsxmidi lo renderiza. Usa el WAD del repo para
# cubrir la musica real que suena al jugar.
add_executable(savanxp-doom-mus-test
    ports/doomgeneric/source/doomgeneric/mus2mid.c
    ports/doomgeneric/source/doomgeneric/memio.c
    subsystems/posix/sdk/v1/runtime/sxmidi.c
    tests/host/doom_mus_test.cpp)
target_include_directories(savanxp-doom-mus-test PRIVATE
    "${CMAKE_SOURCE_DIR}/ports/doomgeneric/source/doomgeneric"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
# El WAD no esta en el repo: la prueba mira el directorio del port y respeta
# SAVANXP_DOOM_WAD, sin atarse a un nombre de archivo concreto.
target_compile_definitions(savanxp-doom-mus-test PRIVATE
    SAVANXP_DOOM_WAD_DIR="${CMAKE_SOURCE_DIR}/ports/doomgeneric/wad")
target_compile_options(savanxp-doom-mus-test PRIVATE -O1 -Wall -Wextra)
set_target_properties(savanxp-doom-mus-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

add_executable(savanxp-partition-test
    kernel/partition.cpp
    tests/host/kernel_string_stubs.cpp
    tests/host/partition_test.cpp)
target_include_directories(savanxp-partition-test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tests/host")
target_compile_options(savanxp-partition-test PRIVATE -O1 -Wall -Wextra -Wno-deprecated)
set_target_properties(savanxp-partition-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

add_executable(savanxp-sxfs-volume-test
    kernel/sxfs.cpp
    tests/host/kernel_string_stubs.cpp
    tests/host/sxfs_host_stubs.cpp
    tests/host/sxfs_volume_test.cpp
    libsxfs/sxfs_core.c)
target_include_directories(savanxp-sxfs-volume-test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/libsxfs"
    "${CMAKE_SOURCE_DIR}/tests/host")
target_compile_options(savanxp-sxfs-volume-test PRIVATE -O1 -Wall -Wextra -Wno-deprecated)
set_target_properties(savanxp-sxfs-volume-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

add_executable(savanxp-div64-test
    subsystems/posix/sdk/v1/runtime/div64.c tests/host/div64_test.cpp)
target_compile_options(savanxp-div64-test PRIVATE -UNDEBUG)
add_test(NAME div64-test COMMAND savanxp-div64-test)

add_custom_target(savanxp_tests
    DEPENDS savanxp-div64-test savanxp-gfx2d-test savanxp-audio-test savanxp-partition-test savanxp-sxfs-volume-test savanxp-kernel-audio-gain-test savanxp-ccleste-wav-test savanxp-sxmidi-test savanxp-doom-mus-test
    COMMENT "Building SavanXP host tests")
add_test(NAME gfx2d-test COMMAND savanxp-gfx2d-test)
add_test(NAME audio-test COMMAND savanxp-audio-test)
add_test(NAME kernel-audio-gain-test COMMAND savanxp-kernel-audio-gain-test)
add_test(NAME ccleste-wav-test COMMAND savanxp-ccleste-wav-test)
add_test(NAME sxmidi-test COMMAND savanxp-sxmidi-test)
add_test(NAME doom-mus-test COMMAND savanxp-doom-mus-test)
add_test(NAME partition-test COMMAND savanxp-partition-test)
add_test(NAME sxfs-volume-test COMMAND savanxp-sxfs-volume-test)

add_custom_command(OUTPUT "${SAVANXP_SXFS_CLI}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_TOOLS_ROOT}"
    COMMAND "${SAVANXP_HOST_CC}"
            -std=c11 -O2 -Wall -Wextra
            -I "${CMAKE_SOURCE_DIR}/include"
            -I "${CMAKE_SOURCE_DIR}/libsxfs"
            "${CMAKE_SOURCE_DIR}/libsxfs/sxfs_core.c"
            "${CMAKE_SOURCE_DIR}/libsxfs/sxfs_cli.c"
            -o "${SAVANXP_SXFS_CLI}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/libsxfs/sxfs_core.c"
        "${CMAKE_SOURCE_DIR}/libsxfs/sxfs_cli.c"
        "${CMAKE_SOURCE_DIR}/libsxfs/sxfs_core.h"
        "${CMAKE_SOURCE_DIR}/include/sxfs/sxfs_format.h"
    COMMENT "Building sxfs-cli"
    VERBATIM
)
add_custom_target(savanxp_sxfs_cli DEPENDS "${SAVANXP_SXFS_CLI}")
add_test(NAME sxfs-compaction-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/sxfs_compaction_test.py"
            --cli "${SAVANXP_SXFS_CLI}"
            --sync "${CMAKE_SOURCE_DIR}/tools/sxfs_sync.py")
add_test(NAME external-builder-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/external_builder_test.py"
            --root "${CMAKE_SOURCE_DIR}"
            --clang "${CMAKE_C_COMPILER}"
            --ld "ld.lld")
add_test(NAME doom-port-layout-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/doom_port_layout_test.py"
            --port "${CMAKE_SOURCE_DIR}/ports/doomgeneric")
add_test(NAME ffmpeg-port-layout-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/ffmpeg_port_layout_test.py"
            --port "${CMAKE_SOURCE_DIR}/ports/ffmpeg")
add_test(NAME ccleste-port-layout-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/ccleste_port_layout_test.py"
            --port "${CMAKE_SOURCE_DIR}/ports/ccleste")
add_test(NAME native-port-layout-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/native_port_layout_test.py"
            --root "${CMAKE_SOURCE_DIR}")
add_test(NAME i386-workflow-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/i386_workflow_test.py"
            --root "${CMAKE_SOURCE_DIR}")
add_test(NAME tcp-echo-server-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/tcp_echo_server_test.py"
            --server "${CMAKE_SOURCE_DIR}/tools/tcp_echo_server.py")
add_dependencies(savanxp_tests savanxp_sxfs_cli)

include("${CMAKE_SOURCE_DIR}/cmake/userland-i386.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/iso-grub.cmake")
message(STATUS "SavanXP architecture: ${SAVANXP_ARCH}")
message(STATUS "SavanXP output root: ${SAVANXP_OUTPUT_ROOT}")
