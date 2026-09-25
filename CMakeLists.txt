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
option(SAVANXP_VIRTIO "Use QEMU virtio hardware" OFF)
option(SAVANXP_HEADLESS "Run QEMU without a graphical display" OFF)
set(SAVANXP_SMOKE_COMMAND "" CACHE STRING
    "Optional init automation command staged in the smoke initramfs")
set(SAVANXP_OUTPUT_ROOT "${CMAKE_SOURCE_DIR}/build" CACHE PATH
    "Shared SavanXP output root (staging, images and persistent disk)")
set(SAVANXP_SXFS_SECTORS 131072 CACHE STRING "Size of the persistent SxFS image in sectors")

set(SAVANXP_GENERATED_ROOT "${SAVANXP_OUTPUT_ROOT}/generated")
set(SAVANXP_SXE_ROOT "${SAVANXP_GENERATED_ROOT}/sxe")
set(SAVANXP_ROOTFS_ROOT "${SAVANXP_OUTPUT_ROOT}/rootfs")
set(SAVANXP_ROOTFS_STAMP "${SAVANXP_OUTPUT_ROOT}/rootfs.stamp")
set(SAVANXP_DISKFS_ROOT "${SAVANXP_OUTPUT_ROOT}/diskfs")
set(SAVANXP_IMAGE_ROOT "${SAVANXP_OUTPUT_ROOT}/image")
set(SAVANXP_INITRAMFS "${SAVANXP_OUTPUT_ROOT}/initramfs.cpio")
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

set(SAVANXP_TARGET_TRIPLE x86_64-unknown-none-elf)

set(SAVANXP_KERNEL_COMPILE_OPTIONS
    "-target;${SAVANXP_TARGET_TRIPLE}"
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fstack-protector-strong
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mcmodel=kernel
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mgeneral-regs-only
    -Wall
    -Wextra
    -Wpedantic
)
set(SAVANXP_UACPI_COMPILE_OPTIONS
    "-target;${SAVANXP_TARGET_TRIPLE}"
    -ffreestanding
    -fstack-protector-strong
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mcmodel=kernel
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mgeneral-regs-only
    -Wall
    -DUACPI_USE_BUILTIN_STRING
)
set(SAVANXP_USER_COMPILE_OPTIONS
    "-target;${SAVANXP_TARGET_TRIPLE}"
    -ffreestanding
    -fstack-protector-strong
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mcmodel=small
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
set(SAVANXP_USER_LINK_OPTIONS
    -nostdlib
    -static
    -fuse-ld=lld
    "-Wl,-T,${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/linker.ld"
    "-Wl,-z,max-page-size=0x1000"
    -Wl,--gc-sections
    -Wl,--build-id=none
)

set(SAVANXP_KERNEL_SOURCES
    arch/x86_64/context.S
    arch/x86_64/entry.cpp
    arch/x86_64/cpu_init.cpp
    arch/x86_64/timer.cpp
    arch/x86_64/smp.cpp
    kernel/kernel_main.cpp
    kernel/boot_screen.cpp
    kernel/console.cpp
    kernel/device.cpp
    kernel/serial_dev.cpp
    kernel/display.cpp
    kernel/ata.cpp
    kernel/virtio_blk.cpp
    kernel/partition.cpp
    kernel/fs.cpp
    kernel/ramdisk.cpp
    kernel/nic.cpp
    kernel/rtl8139.cpp
    kernel/virtio_net.cpp
    kernel/pci.cpp
    kernel/input.cpp
    kernel/tty.cpp
    kernel/ui.cpp
    kernel/virtio_pci.cpp
    kernel/virtio_gpu.cpp
    kernel/fb_gpu.cpp
    kernel/gpu_device.cpp
    kernel/virtio_input.cpp
    kernel/virtio_sound.cpp
    kernel/ac97.cpp
    kernel/audio.cpp
    kernel/audio_device.cpp
    kernel/ps2.cpp
    kernel/pcspeaker.cpp
    kernel/power.cpp
    kernel/clipboard.cpp
    kernel/acpi.cpp
    kernel/ioapic.cpp
    kernel/uacpi_glue.cpp
    kernel/rtc.cpp
    kernel/heap.cpp
    kernel/net.cpp
    kernel/object.cpp
    kernel/physical_memory.cpp
    kernel/vmm.cpp
    kernel/vfs.cpp
    kernel/block.cpp
    kernel/sxfs.cpp
    kernel/elf.cpp
    kernel/process.cpp
    kernel/subsystem.cpp
    kernel/panic.cpp
    kernel/runtime.cpp
)
file(GLOB SAVANXP_POSIX_KERNEL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/subsystems/posix/kernel/*.cpp")
list(SORT SAVANXP_POSIX_KERNEL_SOURCES)
list(APPEND SAVANXP_KERNEL_SOURCES ${SAVANXP_POSIX_KERNEL_SOURCES})

include("${CMAKE_SOURCE_DIR}/vendor/uacpi/uacpi.cmake")
set(SAVANXP_UACPI_SOURCES ${UACPI_SOURCES})

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

add_library(savanxp_uacpi_objects OBJECT ${SAVANXP_UACPI_SOURCES})
target_include_directories(savanxp_uacpi_objects PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/uacpi/include"
)
target_compile_options(savanxp_uacpi_objects PRIVATE
    ${SAVANXP_UACPI_COMPILE_OPTIONS}
    -std=gnu11
)
set_target_properties(savanxp_uacpi_objects PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
)

# The freestanding userland runtime is shared by every in-tree executable. The
# final link still uses --gc-sections, so each program only retains the runtime
# pieces it references.
add_library(savanxp_user_runtime OBJECT
    subsystems/posix/sdk/v1/runtime/crt0.S
    subsystems/posix/sdk/v1/runtime/libc.c
    subsystems/posix/sdk/v1/runtime/posix.c
    subsystems/posix/sdk/v1/runtime/math.c
    subsystems/posix/sdk/v1/runtime/gfx.c
    subsystems/posix/sdk/v1/runtime/gfx2d.c
    subsystems/posix/sdk/v1/runtime/sxchrome.c
)
target_include_directories(savanxp_user_runtime PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_definitions(savanxp_user_runtime PRIVATE
    DESKTOP_INCLUDE_TEST_APPS=$<BOOL:${SAVANXP_INCLUDE_TEST_APPS}>)
target_compile_options(savanxp_user_runtime PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
set_target_properties(savanxp_user_runtime PROPERTIES
    C_EXTENSIONS YES
)

# BusyBox follows the external SDK profile: no SSE, and it needs the setjmp
# trampoline that the in-tree userland runtime does not use.
set(SAVANXP_EXTERNAL_COMPILE_OPTIONS
    "-target;${SAVANXP_TARGET_TRIPLE}"
    -ffreestanding
    -fstack-protector-strong
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mcmodel=small
    -mno-mmx
    -mno-sse
    -mno-sse2
    -mgeneral-regs-only
    -Wall
    -Wextra
    -Wpedantic
)
set(SAVANXP_EXTERNAL_LINK_OPTIONS
    -nostdlib
    -static
    -fuse-ld=lld
    "-Wl,-T,${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/linker.ld"
    "-Wl,-z,max-page-size=0x1000"
    -Wl,--build-id=none
)
add_library(savanxp_external_runtime OBJECT
    subsystems/posix/sdk/v1/runtime/crt0.S
    subsystems/posix/sdk/v1/runtime/libc.c
    subsystems/posix/sdk/v1/runtime/posix.c
    subsystems/posix/sdk/v1/runtime/gfx.c
    subsystems/posix/sdk/v1/runtime/gfx2d.c
    subsystems/posix/sdk/v1/runtime/setjmp.S
)
target_include_directories(savanxp_external_runtime PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp_external_runtime PRIVATE ${SAVANXP_EXTERNAL_COMPILE_OPTIONS})
set_target_properties(savanxp_external_runtime PROPERTIES
    C_EXTENSIONS YES
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
set(SAVANXP_GENERATED_HEADER_FILES
    "${SAVANXP_BOOT_LOGO_HEADER}"
    "${SAVANXP_BRAND_LOGO_HEADER}"
    "${SAVANXP_CURSOR_HEADER}"
    "${SAVANXP_DESKTOP_ICONS_HEADER}"
)
set_source_files_properties(${SAVANXP_GENERATED_HEADER_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
add_custom_target(savanxp_generated_headers DEPENDS ${SAVANXP_GENERATED_HEADER_FILES})
target_sources(savanxp_kernel_objects PRIVATE ${SAVANXP_GENERATED_HEADER_FILES})

include("${CMAKE_SOURCE_DIR}/cmake/UserPrograms.cmake")

# BusyBox is an external-style port compiled against the same freestanding
# runtime, but it is part of the base rootfs and therefore a normal target here.
file(GLOB SAVANXP_BUSYBOX_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/vendor/busybox-port/*.c")
list(SORT SAVANXP_BUSYBOX_SOURCES)
add_executable(savanxp-busybox ${SAVANXP_BUSYBOX_SOURCES})
target_include_directories(savanxp-busybox PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/busybox-port"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/include"
    "${SAVANXP_GENERATED_ROOT}"
)
target_link_libraries(savanxp-busybox PRIVATE savanxp_external_runtime)
target_compile_definitions(savanxp-busybox PRIVATE
    DESKTOP_INCLUDE_TEST_APPS=$<BOOL:${SAVANXP_INCLUDE_TEST_APPS}>)
target_compile_options(savanxp-busybox PRIVATE ${SAVANXP_EXTERNAL_COMPILE_OPTIONS})
target_link_options(savanxp-busybox PRIVATE ${SAVANXP_EXTERNAL_LINK_OPTIONS})
set_target_properties(savanxp-busybox PROPERTIES OUTPUT_NAME busybox SUFFIX "")

set(SAVANXP_USER_TARGETS)
get_property(SAVANXP_USER_TARGETS GLOBAL PROPERTY SAVANXP_USER_TARGETS)
set(SAVANXP_USER_PROGRAM_NAMES)
get_property(SAVANXP_USER_PROGRAM_NAMES GLOBAL PROPERTY SAVANXP_USER_PROGRAM_NAMES)

set(SAVANXP_SXE_ARGUMENTS)
foreach(program IN LISTS SAVANXP_USER_PROGRAM_NAMES)
    list(APPEND SAVANXP_SXE_ARGUMENTS --program "${program}")
endforeach()
set(SAVANXP_SXE_STAMP "${SAVANXP_GENERATED_ROOT}/sxe-resources.stamp")
set(SAVANXP_SXE_CONFIG_STAMP "${CMAKE_BINARY_DIR}/sxe-programs.stamp")
set(SAVANXP_SXE_CONFIG_CONTENT
    "include_tests=${SAVANXP_INCLUDE_TEST_APPS}\nbuild_id=${SAVANXP_BUILD_ID}\nprograms=${SAVANXP_USER_PROGRAM_NAMES}\n")
set(SAVANXP_SXE_CONFIG_OLD "")
if(EXISTS "${SAVANXP_SXE_CONFIG_STAMP}")
    file(READ "${SAVANXP_SXE_CONFIG_STAMP}" SAVANXP_SXE_CONFIG_OLD)
endif()
if(NOT SAVANXP_SXE_CONFIG_OLD STREQUAL SAVANXP_SXE_CONFIG_CONTENT)
    file(WRITE "${SAVANXP_SXE_CONFIG_STAMP}" "${SAVANXP_SXE_CONFIG_CONTENT}")
endif()
file(GLOB_RECURSE SAVANXP_SXE_MANIFESTS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland/*.sxres")
file(GLOB_RECURSE SAVANXP_SXE_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/desktop/icons/*.png")
add_custom_command(OUTPUT "${SAVANXP_SXE_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_SXE_ROOT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_sxe_resources.py"
            --project-root "${CMAKE_SOURCE_DIR}"
            --output-dir "${SAVANXP_SXE_ROOT}"
            --build-id "${SAVANXP_BUILD_ID}"
            --manifest-dir "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
            ${SAVANXP_SXE_ARGUMENTS}
    COMMAND "${CMAKE_COMMAND}" -E touch "${SAVANXP_SXE_STAMP}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/gen_sxe_resources.py"
        "${CMAKE_SOURCE_DIR}/include/sxe/sxe_format.h"
        "${CMAKE_SOURCE_DIR}/include/abi/savanxp_native_abi.h"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include/savanxp/syscall.h"
        "${CMAKE_SOURCE_DIR}/include/shared/version.h"
        "${SAVANXP_SXE_CONFIG_STAMP}"
        ${SAVANXP_GENERATED_HEADER_FILES}
        ${SAVANXP_SXE_MANIFESTS}
        ${SAVANXP_SXE_ASSETS}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Generating SXE resources"
    VERBATIM
)
add_custom_target(savanxp_sxe_resources DEPENDS "${SAVANXP_SXE_STAMP}")

foreach(target IN LISTS SAVANXP_USER_TARGETS)
    add_dependencies(${target} savanxp_sxe_resources)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${SAVANXP_SXE_STAMP}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/stamp_sxe.py"
                --binary "$<TARGET_FILE:${target}>"
                --name "${target}"
                --resource-dir "${SAVANXP_SXE_ROOT}"
                --objcopy "${SAVANXP_OBJCOPY}"
                --readelf "${SAVANXP_READELF}"
        VERBATIM
    )
endforeach()
add_dependencies(savanxp-busybox savanxp_sxe_resources)

add_custom_target(savanxp_kernel
    COMMAND "${SAVANXP_LD}"
            -m elf_x86_64
            -T "${CMAKE_SOURCE_DIR}/linker.ld"
            -z max-page-size=0x1000
            --build-id=none
            -o "${SAVANXP_KERNEL_ELF}"
            $<TARGET_OBJECTS:savanxp_kernel_objects>
            $<TARGET_OBJECTS:savanxp_uacpi_objects>
    COMMAND_EXPAND_LISTS
    DEPENDS savanxp_kernel_objects savanxp_uacpi_objects
    COMMENT "Linking SavanXP kernel"
    VERBATIM
)
add_dependencies(savanxp_kernel_objects savanxp_generated_headers)

add_custom_target(savanxp_userland
    DEPENDS ${SAVANXP_USER_TARGETS} savanxp-busybox
    COMMENT "Linking SavanXP userland"
)

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

add_executable(savanxp-partition-test
    kernel/partition.cpp
    tests/host/kernel_string_stubs.cpp
    tests/host/partition_test.cpp)
target_include_directories(savanxp-partition-test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tests/host")
target_compile_options(savanxp-partition-test PRIVATE -O1 -Wall -Wextra -Wno-deprecated)
set_target_properties(savanxp-partition-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

# The SxMedia engine and its registry. Deliberately with no savanxp_* stubs: the
# engine opens no file, because opening a source is a backend's job, so the
# whole registry is exercised on the host in seconds rather than in a QEMU boot.
# See docs/SXMEDIA.md.
add_executable(savanxp-sxmedia-test
    subsystems/posix/sdk/v1/runtime/sxmedia.c
    tests/host/sxmedia_test.cpp)
target_include_directories(savanxp-sxmedia-test PRIVATE
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
target_compile_options(savanxp-sxmedia-test PRIVATE -O1 -Wall -Wextra)
set_target_properties(savanxp-sxmedia-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

# SxCodecs' first codec, against a real Ogg file. Separate from the engine test
# because it is the only test here that needs a codec and a fixture, and because
# it is the one that says whether the PCM coming out is right rather than whether
# the plumbing moved it. See docs/SXMEDIA.md.
add_executable(savanxp-sxmedia-vorbis-test
    subsystems/posix/sdk/v1/runtime/sxmedia.c
    subsystems/posix/sdk/v1/runtime/sxmedia_vorbis.c
    tests/host/sxmedia_vorbis_test.cpp)
target_include_directories(savanxp-sxmedia-vorbis-test PRIVATE
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/vendor/stb")
# stb_vorbis is third-party and is not held to this tree's warning settings: it
# has one tautological comparison of its own, and patching a pinned vendored file
# to silence a warning is how a pin stops being reproducible.
target_compile_options(savanxp-sxmedia-vorbis-test PRIVATE -O1 -Wall -Wextra)
set_source_files_properties(subsystems/posix/sdk/v1/runtime/sxmedia_vorbis.c
    TARGET_DIRECTORY savanxp-sxmedia-vorbis-test PROPERTIES COMPILE_OPTIONS "-w")
set_target_properties(savanxp-sxmedia-vorbis-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

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

add_custom_target(savanxp_tests
    DEPENDS savanxp-gfx2d-test savanxp-audio-test savanxp-partition-test
            savanxp-sxmedia-test savanxp-sxmedia-vorbis-test savanxp-sxfs-volume-test
    COMMENT "Building SavanXP host tests"
)
add_test(NAME gfx2d-test COMMAND savanxp-gfx2d-test)
add_test(NAME audio-test COMMAND savanxp-audio-test)
add_test(NAME partition-test COMMAND savanxp-partition-test)
add_test(NAME sxmedia-test COMMAND savanxp-sxmedia-test)
# The fixtures are named by relative path, so the test runs from the repository
# root rather than from wherever ctest was invoked.
add_test(NAME sxmedia-vorbis-test COMMAND savanxp-sxmedia-vorbis-test
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
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
add_test(NAME smoke-runner-layout-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/smoke_runner_layout_test.py"
            --root "${CMAKE_SOURCE_DIR}")
add_test(NAME tcp-echo-server-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/tcp_echo_server_test.py"
            --server "${CMAKE_SOURCE_DIR}/tools/tcp_echo_server.py")
add_test(NAME system-mediaplayer-layout-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/system_mediaplayer_layout_test.py"
            --root "${CMAKE_SOURCE_DIR}")
add_dependencies(savanxp_tests savanxp_sxfs_cli)

set(SAVANXP_APPLETS busybox ls cat echo mkdir rm mv cp ps true false sleep)
set(SAVANXP_STAGE_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_ROOTFS_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_ROOTFS_ROOT}/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/rootfs/README"
            "${SAVANXP_ROOTFS_ROOT}/README"
)
foreach(target IN LISTS SAVANXP_USER_TARGETS)
    list(APPEND SAVANXP_STAGE_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E copy
                "$<TARGET_FILE:${target}>"
                "${SAVANXP_ROOTFS_ROOT}/bin/${target}"
    )
endforeach()
list(APPEND SAVANXP_STAGE_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:savanxp-busybox>"
            "${SAVANXP_ROOTFS_ROOT}/bin/busybox"
)
foreach(applet IN LISTS SAVANXP_APPLETS)
    if(NOT applet STREQUAL "busybox")
        list(APPEND SAVANXP_STAGE_COMMANDS
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "$<TARGET_FILE:savanxp-busybox>"
                    "${SAVANXP_ROOTFS_ROOT}/bin/${applet}"
        )
    endif()
endforeach()
if(SAVANXP_SMOKE_COMMAND)
    list(APPEND SAVANXP_STAGE_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${SAVANXP_ROOTFS_ROOT}/SMOKE"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/write_smoke_spec.py"
                --output "${SAVANXP_ROOTFS_ROOT}/SMOKE"
                --command "${SAVANXP_SMOKE_COMMAND}"
    )
else()
    list(APPEND SAVANXP_STAGE_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${SAVANXP_ROOTFS_ROOT}/SMOKE"
    )
endif()
list(APPEND SAVANXP_STAGE_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E touch "${SAVANXP_ROOTFS_STAMP}"
)
set(SAVANXP_SMOKE_CONFIG_STAMP "${CMAKE_BINARY_DIR}/smoke-config.stamp")
set(SAVANXP_SMOKE_CONFIG_CONTENT "command=${SAVANXP_SMOKE_COMMAND}\n")
set(SAVANXP_SMOKE_CONFIG_OLD "")
if(EXISTS "${SAVANXP_SMOKE_CONFIG_STAMP}")
    file(READ "${SAVANXP_SMOKE_CONFIG_STAMP}" SAVANXP_SMOKE_CONFIG_OLD)
endif()
if(NOT SAVANXP_SMOKE_CONFIG_OLD STREQUAL SAVANXP_SMOKE_CONFIG_CONTENT)
    file(WRITE "${SAVANXP_SMOKE_CONFIG_STAMP}" "${SAVANXP_SMOKE_CONFIG_CONTENT}")
endif()

add_custom_command(OUTPUT "${SAVANXP_ROOTFS_STAMP}"
    ${SAVANXP_STAGE_COMMANDS}
    DEPENDS savanxp_userland "${CMAKE_SOURCE_DIR}/rootfs/README"
            "${CMAKE_SOURCE_DIR}/tools/write_smoke_spec.py"
            "${SAVANXP_SMOKE_CONFIG_STAMP}"
    COMMENT "Staging SavanXP initramfs tree"
    VERBATIM
)
add_custom_target(savanxp_rootfs DEPENDS "${SAVANXP_ROOTFS_STAMP}")

add_custom_command(OUTPUT "${SAVANXP_INITRAMFS}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/mkinitramfs.py"
            --source "${SAVANXP_ROOTFS_ROOT}"
            --output "${SAVANXP_INITRAMFS}"
    DEPENDS "${SAVANXP_ROOTFS_STAMP}" "${CMAKE_SOURCE_DIR}/tools/mkinitramfs.py"
    COMMENT "Writing initramfs.cpio"
    VERBATIM
)
add_custom_target(savanxp_initramfs DEPENDS "${SAVANXP_INITRAMFS}")

add_custom_target(savanxp_diskfs
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_DISKFS_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/bin"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/icons"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/diskfs" "${SAVANXP_DISKFS_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${SAVANXP_ROOTFS_ROOT}/bin" "${SAVANXP_DISKFS_ROOT}/bin"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_mime_icons.py"
            --project-root "${CMAKE_SOURCE_DIR}"
            --output-dir "${SAVANXP_DISKFS_ROOT}/icons"
    DEPENDS savanxp_rootfs
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Staging persistent disk tree"
    VERBATIM
)

add_custom_target(savanxp_disk
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/sxfs_sync.py"
            --image "${SAVANXP_DISK_IMAGE}"
            --source "${SAVANXP_DISKFS_ROOT}"
            --cli "${SAVANXP_SXFS_CLI}"
            --sectors "${SAVANXP_SXFS_SECTORS}"
    DEPENDS savanxp_diskfs savanxp_sxfs_cli
    COMMENT "Synchronizing persistent SxFS image"
    VERBATIM
)

set(SAVANXP_LIMINE_ROOT "${CMAKE_SOURCE_DIR}/tools/limine")
set(SAVANXP_LIMINE_INSTALLER "${SAVANXP_LIMINE_ROOT}/limine" CACHE FILEPATH
    "Limine BIOS deployer used by the ISO target")
foreach(limine_file
    BOOTX64.EFI
    limine-bios-cd.bin
    limine-bios.sys
    limine-uefi-cd.bin
)
    if(NOT EXISTS "${SAVANXP_LIMINE_ROOT}/${limine_file}")
        message(FATAL_ERROR
            "Missing tools/limine/${limine_file}. Run ./build.sh once to fetch Limine.")
    endif()
endforeach()
if(NOT EXISTS "${SAVANXP_LIMINE_INSTALLER}")
    message(STATUS "Limine ISO deployer is not built; the savanxp_iso target is unavailable.")
endif()

add_custom_target(savanxp_image
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_IMAGE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_IMAGE_ROOT}/boot/limine"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_IMAGE_ROOT}/EFI/BOOT"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/boot/limine.conf" "${SAVANXP_IMAGE_ROOT}/limine.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/boot/limine.conf" "${SAVANXP_IMAGE_ROOT}/boot/limine/limine.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_KERNEL_ELF}" "${SAVANXP_IMAGE_ROOT}/boot/kernel.elf"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_INITRAMFS}" "${SAVANXP_IMAGE_ROOT}/boot/initramfs.cpio"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_DISK_IMAGE}" "${SAVANXP_IMAGE_ROOT}/boot/disk.img"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_LIMINE_ROOT}/BOOTX64.EFI" "${SAVANXP_IMAGE_ROOT}/EFI/BOOT/BOOTX64.EFI"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_LIMINE_ROOT}/limine-bios-cd.bin" "${SAVANXP_IMAGE_ROOT}/boot/limine/limine-bios-cd.bin"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_LIMINE_ROOT}/limine-bios.sys" "${SAVANXP_IMAGE_ROOT}/boot/limine/limine-bios.sys"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_LIMINE_ROOT}/limine-uefi-cd.bin" "${SAVANXP_IMAGE_ROOT}/boot/limine/limine-uefi-cd.bin"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/boot/startup.nsh" "${SAVANXP_IMAGE_ROOT}/startup.nsh"
    DEPENDS savanxp_kernel savanxp_initramfs savanxp_disk
    COMMENT "Staging EFI boot image"
    VERBATIM
)

set(SAVANXP_QEMU_ARGS
    --image-root "${SAVANXP_IMAGE_ROOT}"
    --disk-image "${SAVANXP_DISK_IMAGE}"
    --accel "${SAVANXP_ACCEL}"
    --smp "${SAVANXP_SMP}"
)
if(SAVANXP_VIRTIO)
    list(APPEND SAVANXP_QEMU_ARGS --virtio)
endif()
if(SAVANXP_HEADLESS)
    list(APPEND SAVANXP_QEMU_ARGS --headless)
endif()

add_custom_target(savanxp_run
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/run_qemu.py"
            ${SAVANXP_QEMU_ARGS}
    DEPENDS savanxp_image
    COMMENT "Running SavanXP in QEMU"
    VERBATIM
)
add_custom_target(savanxp_debug
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/run_qemu.py"
            ${SAVANXP_QEMU_ARGS} --debug
    DEPENDS savanxp_image
    COMMENT "Running SavanXP in QEMU with gdb"
    VERBATIM
)

if(EXISTS "${SAVANXP_LIMINE_INSTALLER}")
    add_custom_target(savanxp_iso
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${SAVANXP_ISO_IMAGE}"
        COMMAND "${SAVANXP_XORRISO}"
                -as mkisofs
                -b boot/limine/limine-bios-cd.bin
                -no-emul-boot
                -boot-load-size 4
                -boot-info-table
                -eltorito-alt-boot
                -e boot/limine/limine-uefi-cd.bin
                -no-emul-boot
                -efi-boot-part
                --efi-boot-image
                --protective-msdos-label
                -iso-level 3
                -V SAVANXP
                -o "${SAVANXP_ISO_IMAGE}"
                "${SAVANXP_IMAGE_ROOT}"
        COMMAND "${SAVANXP_LIMINE_INSTALLER}" bios-install "${SAVANXP_ISO_IMAGE}"
        DEPENDS savanxp_image
        WORKING_DIRECTORY "${SAVANXP_IMAGE_ROOT}"
        COMMENT "Creating SavanXP.iso"
        VERBATIM
    )
endif()

add_custom_target(savanxp
    DEPENDS savanxp_image
    COMMENT "Building SavanXP base system"
)

message(STATUS "SavanXP output root: ${SAVANXP_OUTPUT_ROOT}")
message(STATUS "SavanXP test applications: ${SAVANXP_INCLUDE_TEST_APPS}")
message(STATUS "SavanXP build id: ${SAVANXP_BUILD_ID}")
