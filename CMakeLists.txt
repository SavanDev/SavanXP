cmake_minimum_required(VERSION 3.25)

project(SavanXP
    VERSION 0.4.1
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
set(SAVANXP_SXFS_SECTORS 2097152 CACHE STRING "Size of the persistent SxFS image in sectors")
# El volumen de la ISO es mas chico a proposito: es un LiveCD, tiene que entrar
# en un medio optico y ademas se carga en RAM como modulo de Limine, asi que cada
# byte suyo es tambien un byte de memoria del invitado. La imagen de desarrollo
# no tiene por que ser chica. Ver tools/iso_volume.py.
set(SAVANXP_ISO_SECTORS 524288 CACHE STRING "Size of the LiveCD SxFS volume in sectors")

set(SAVANXP_GENERATED_ROOT "${SAVANXP_OUTPUT_ROOT}/generated")
set(SAVANXP_SXE_ROOT "${SAVANXP_GENERATED_ROOT}/sxe")
set(SAVANXP_ROOTFS_ROOT "${SAVANXP_OUTPUT_ROOT}/rootfs")
set(SAVANXP_ROOTFS_STAMP "${SAVANXP_OUTPUT_ROOT}/rootfs.stamp")
set(SAVANXP_DISKFS_ROOT "${SAVANXP_OUTPUT_ROOT}/diskfs")
set(SAVANXP_IMAGE_ROOT "${SAVANXP_OUTPUT_ROOT}/image")
# Arbol de arranque de la ISO, aparte del de desarrollo. Ver savanxp_iso_root.
set(SAVANXP_ISO_ROOT "${SAVANXP_OUTPUT_ROOT}/iso-root")
set(SAVANXP_INITRAMFS "${SAVANXP_OUTPUT_ROOT}/initramfs.cpio")
set(SAVANXP_DISK_IMAGE "${SAVANXP_OUTPUT_ROOT}/disk.img")
set(SAVANXP_KERNEL_ELF "${SAVANXP_OUTPUT_ROOT}/kernel.elf")
set(SAVANXP_ISO_IMAGE "${SAVANXP_OUTPUT_ROOT}/SavanXP.iso")
set(SAVANXP_TOOLS_ROOT "${SAVANXP_OUTPUT_ROOT}/tools")
set(SAVANXP_SXFS_CLI "${SAVANXP_TOOLS_ROOT}/sxfs-cli")

# Prefer the `python3` command on PATH, the one a developer runs. FindPython3
# alone walks PATH directory by directory looking for versioned names, so a
# stray python3.X earlier in PATH (a uv or pyenv install without Pillow) wins
# over the system python3. -DPython3_EXECUTABLE=... still overrides this.
if(NOT DEFINED Python3_EXECUTABLE)
    find_program(SAVANXP_PYTHON3 NAMES python3)
    if(SAVANXP_PYTHON3)
        set(Python3_EXECUTABLE "${SAVANXP_PYTHON3}")
    endif()
endif()
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
    message(FATAL_ERROR
        "Python Pillow is required to generate SavanXP assets, and "
        "${Python3_EXECUTABLE} does not have it. Install Pillow there, or pick "
        "another interpreter with -DPython3_EXECUTABLE=/path/to/python3.")
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
set(SAVANXP_LIBRARY_COMPILE_OPTIONS
    "-target;${SAVANXP_TARGET_TRIPLE}"
    -ffreestanding
    -fPIC
    -mno-red-zone
    -mcmodel=medium
    -mno-mmx
    -msse
    -msse2
    -fstack-protector-strong
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
    "-Wl,-T,${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/linker.ld"
    "-Wl,-z,max-page-size=0x1000"
    -Wl,--gc-sections
    -Wl,--build-id=none
)
# El bloque PIE de verdad: sin -static, sin -T fijo, -pie y --export-dynamic.
# Sin esto un ejecutable PIE no nace: -static y el -T compiten con -pie de un
# modo que no se expresa como "agregar una opcion", y sin --export-dynamic la
# salida DYN sale sin .dynsym y el cargador no tiene contra que resolver.
#
# --dynamic-linker va con la ruta real del interprete. lld, al ver -pie sin
# -static, mete por defecto /lib64/ld-linux-x86-64.so.2, que es del host que
# compila y no existe en SavanXP. El efecto no era cosmeticso: el kernel copia
# esa ruta al stack inicial, y una imagen ET_DYN la llevaba a una posicion que
# pisaba la cadena del ultimo argumento. Un programa recibia su propio argv con
# una ruta de glibc en lugar de la suya.
#
# La ruta que va es la que el interprete tendria si fuera un archivo aparte.
# Hoy el cargador va enlazado dentro del programa, asi que el kernel mapea la
# imagen principal y crt0 corre el cargador; PT_INTERP no se ejecuta todavia.
set(SAVANXP_USER_LINK_OPTIONS_PIE
    -nostdlib
    -fuse-ld=lld
    -pie
    "-Wl,-z,max-page-size=0x1000"
    "-Wl,--dynamic-linker,/lib/ld.so.0.4"
    -Wl,--build-id=none
    -Wl,--export-dynamic
    "-Wl,--gc-sections"
    -Wl,-e,_start
)
set(SAVANXP_USER_COMPILE_OPTIONS_PIE ${SAVANXP_USER_COMPILE_OPTIONS_PIC})

set(SAVANXP_KERNEL_SOURCES
    arch/x86_64/context.S
    arch/x86_64/entry.cpp
    arch/x86_64/cpu_init.cpp
    arch/x86_64/timer.cpp
    arch/x86_64/smp.cpp
    arch/x86_64/memory.cpp
    kernel/kernel_main.cpp
    kernel/boot_screen.cpp
    kernel/console.cpp
    kernel/device.cpp
    kernel/serial_dev.cpp
    kernel/display.cpp
    kernel/ahci.cpp
    kernel/ata.cpp
    kernel/virtio_blk.cpp
    kernel/partition.cpp
    kernel/fs.cpp
    kernel/fs_path.cpp
    kernel/ramdisk.cpp
    kernel/nic.cpp
    kernel/rtl8139.cpp
    kernel/virtio_net.cpp
    kernel/pci.cpp
    kernel/input.cpp
    kernel/keyboard.cpp
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
    kernel/native_dispatch.cpp
    kernel/sx.cpp
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
#
# The units come from one manifest, subsystems/posix/sdk/v1/runtime/units.txt,
# which the builds outside CMake (tools/build-user.sh and the ports) read too.
# In-tree there is ONE list for the three targets: the normal one, the PIC one
# and the PIC one without math.
#
# Each target used to have its own list and they did not match: the PIC one
# carried sxgui, sxe and audio, the normal one did not. A program that worked
# under STATIC had to list by hand the units the runtime did not bring, and as
# soon as it moved to PIE those same units became duplicate symbols. Switching
# profiles was then not a one-line change but an edit of sources, which is
# exactly what the profile should avoid.
#
# Extra units in a program that does not use them cost nothing: the compile
# options carry -ffunction-sections/-fdata-sections and the STATIC profile
# passes --gc-sections, so whatever is not referenced is dropped at link time.
# setjmp is left out: only the ports use sx_setjmp.
function(savanxp_runtime_units out_var)
    set(manifest "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/runtime/units.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
    file(STRINGS "${manifest}" lines REGEX "^[a-z]")
    set(units)
    foreach(line IN LISTS lines)
        if(NOT line MATCHES "^([a-z]+)[ \t]+([^ \t]+)$")
            message(FATAL_ERROR "${manifest}: malformed line '${line}'")
        endif()
        if(CMAKE_MATCH_1 IN_LIST ARGN)
            list(APPEND units "${CMAKE_MATCH_2}")
        endif()
    endforeach()
    set(${out_var} ${units} PARENT_SCOPE)
endfunction()
savanxp_runtime_units(SAVANXP_USER_RUNTIME_SOURCES core math gui audio midi gl)
add_library(savanxp_user_runtime OBJECT ${SAVANXP_USER_RUNTIME_SOURCES})
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

set(SAVANXP_USER_COMPILE_OPTIONS_PIC
    "-target;${SAVANXP_TARGET_TRIPLE}"
    -ffreestanding
    -fstack-protector-strong
    -fPIC
    -mno-red-zone
    -mcmodel=medium
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
# Variantes del runtime, elegidas por las librerias que el programa usa.
#
# Un runtime por COMBINACION de unidades que salen a una libreria. La razon es la
# misma para todas: si el ejecutable trae la copia, el enlazador no emite
# DT_NEEDED, la libreria no se carga y no se comparte nada. El programa tiene que
# dejar el simbolo indefinido para que el mecanismo sea real.
#
# Las combinaciones las pide el programa segun sus DEPENDS, no una bandera. Con
# math, sxgui y gfx withdrawn hay ocho subconjuntos: enumerarlos seria ocho
# bloques casi identicos, y el sexto se olvidaria. Lo que decide es "esta
# libreria reemplaza estas unidades", y `savanxp_program` lo lee de DEPENDS.
#
# El target se nombra por el conjunto que le falta, asi que dos programas con
# DEPENDS distintos reciben targets distintos sin que nadie los enumere.
set(SAVANXP_POSIX_RUNTIME subsystems/posix/sdk/v1/runtime)
set(SAVANXP_LIBRARY_REPLACES_libmath ${SAVANXP_POSIX_RUNTIME}/math.c)
set(SAVANXP_LIBRARY_REPLACES_libsxmidi ${SAVANXP_POSIX_RUNTIME}/sxmidi.c)
set(SAVANXP_LIBRARY_REPLACES_libsxgui system/gfx/sxgui.c ${SAVANXP_POSIX_RUNTIME}/sxgui_app.c)
set(SAVANXP_LIBRARY_REPLACES_libsxgfx ${SAVANXP_POSIX_RUNTIME}/gfx.c system/gfx/pixels.c)
set(SAVANXP_LIBRARY_REPLACES_libgfx2d system/gfx/gfx2d.c system/gfx/sxchrome.c ${SAVANXP_POSIX_RUNTIME}/gfx_scaled.c)

# El reproductor de medios es un programa de este arbol que depende de una libreria
# que construye ports/ffmpeg. Eso rompe una suposicion: un programa se compila con
# lo que hay en el arbol.
#
# Lo que hace falta son DOS cosas del port --la libreria, para el DT_NEEDED, y las
# CABECERAS, que `configure` genera y no estan versionadas-- y por eso el programa
# es condicional. Sin el port no hay reproductor, y el registro lo descarta igual
# que a Doom. Se dice en voz alta porque un programa que aparece y desaparece
#dependiendo de lo que haya en el disco es exactamente lo que no debe pasar sin
# decirse.
#
# La alternativa --dejar el reproductor FUERA del port-- exige que la libreria se
# pueda enlazar sin existir, y eso se resuelve con un stub de enlace que ademas
# obliga a renunciar a la comprobacion de la API de FFmpeg en tiempo de enlace. Es un
# intercambio: ahora se comprueba y el programa es condicional; con stub, el
# programa es siempre y la comprobacion pasa a ser de arranque.
# Dos directorios de cabeceras y no uno, porque FFmpeg separa lo que genera de lo
# que trae: las cabeceras publicas estan en el arbol de fuentes y `configure` solo
# pone config.h y config_components.h en el directorio de build. El port los pasa
# con -I "$SRC" -I "$BUILD" (ports/ffmpeg/link.sh), y aqui hace falta lo mismo.
set(SAVANXP_FFMPEG_LIBRARY "${SAVANXP_OUTPUT_ROOT}/external/libffmpeg.so.0.4")
set(SAVANXP_FFMPEG_SOURCE "${SAVANXP_OUTPUT_ROOT}/ports/ffmpeg/work/ffmpeg-7.1.1")
set(SAVANXP_FFMPEG_BUILD "${SAVANXP_OUTPUT_ROOT}/ports/ffmpeg/work/build")
set(SAVANXP_HAVE_FFMPEG OFF)
if(EXISTS "${SAVANXP_FFMPEG_LIBRARY}"
   AND EXISTS "${SAVANXP_FFMPEG_SOURCE}/libavcodec/avcodec.h"
   AND EXISTS "${SAVANXP_FFMPEG_BUILD}/config.h")
    set(SAVANXP_HAVE_FFMPEG ON)
    add_library(libffmpeg SHARED IMPORTED GLOBAL)
    set_target_properties(libffmpeg PROPERTIES
        IMPORTED_LOCATION "${SAVANXP_FFMPEG_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${SAVANXP_FFMPEG_SOURCE};${SAVANXP_FFMPEG_BUILD}")
    message(STATUS "mediaplayer: se construye con ${SAVANXP_FFMPEG_LIBRARY}")
else()
    message(STATUS
        "mediaplayer: NO se construye; falta ${SAVANXP_FFMPEG_LIBRARY} o el arbol de "
        "FFmpeg. Construi ports/ffmpeg primero. El registro lo descarta.")
endif()

function(savanxp_runtime_variant)
    set(options PIC)
    set(oneValueArgs NAME)
    set(multiValueArgs EXCLUDE)
    cmake_parse_arguments(R "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    set(sources ${SAVANXP_USER_RUNTIME_SOURCES})
    set(key full)
    foreach(dropped IN LISTS R_EXCLUDE)
        list(REMOVE_ITEM sources ${dropped})
        get_filename_component(base "${dropped}" NAME)
        set(key "${key}_${base}")
    endforeach()
    string(REGEX REPLACE "[.]c$" "" key "${key}")
    string(REGEX REPLACE "_" "-" key "${key}")
    add_library(${R_NAME} OBJECT ${sources})
    target_include_directories(${R_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/runtime"
        "${SAVANXP_GENERATED_ROOT}"
    )
    target_compile_definitions(${R_NAME} PRIVATE
        DESKTOP_INCLUDE_TEST_APPS=$<BOOL:${SAVANXP_INCLUDE_TEST_APPS}>)
    if(R_PIC)
        target_compile_options(${R_NAME} PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS_PIC})
    else()
        target_compile_options(${R_NAME} PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
    endif()
    set_target_properties(${R_NAME} PROPERTIES C_EXTENSIONS YES)
    set(SAVANXP_RUNTIME_TARGET_${key} ${R_NAME} PARENT_SCOPE)
endfunction()

# El runtime completo: el caso base, para un programa que no usa ninguna
# libreria compartida.
savanxp_runtime_variant(NAME savanxp_user_runtime_pic PIC)

# Las demas combinaciones las crea savanxp_program segun sus DEPENDS: cada
# programa pide las suyas y nadie mantiene una lista a mano.

add_custom_target(savanxp_user_runtime_pic_check ALL
    DEPENDS savanxp_user_runtime_pic
)

# Desktop icon PNGs are checked-in, hand-editable source assets. The art
# generator is not part of the repository; where a local copy exists it is an
# explicit target, and a normal build never redraws or overwrites the artwork.
if(EXISTS "${CMAKE_SOURCE_DIR}/tools/gen_desktop_source_art.py")
    add_custom_target(savanxp_desktop_source_art
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_desktop_source_art.py"
                --project-root "${CMAKE_SOURCE_DIR}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating optional desktop source art"
        VERBATIM
    )
endif()
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

include("${CMAKE_SOURCE_DIR}/cmake/UserPrograms.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/SystemServices.cmake")

set(SAVANXP_USER_TARGETS)
get_property(SAVANXP_USER_TARGETS GLOBAL PROPERTY SAVANXP_USER_TARGETS)
set(SAVANXP_USER_PROGRAM_NAMES)
get_property(SAVANXP_USER_PROGRAM_NAMES GLOBAL PROPERTY SAVANXP_USER_PROGRAM_NAMES)

set(SAVANXP_NATIVE_TARGETS)
get_property(SAVANXP_NATIVE_TARGETS GLOBAL PROPERTY SAVANXP_NATIVE_TARGETS)

set(SAVANXP_SXE_ARGUMENTS)
foreach(program IN LISTS SAVANXP_USER_PROGRAM_NAMES)
    list(APPEND SAVANXP_SXE_ARGUMENTS --program "${program}")
endforeach()
# A SUN program gets its resources as C too (<name>.sunres.h): it has no
# files to read its own .sxe with (docs/userland/SUN.md).
foreach(program IN LISTS SAVANXP_NATIVE_TARGETS)
    list(APPEND SAVANXP_SXE_ARGUMENTS --native-program "${program}")
    target_include_directories(${program} PRIVATE "${SAVANXP_SXE_ROOT}")
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
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland/*.sxres"
    "${CMAKE_SOURCE_DIR}/system/*.sxres")
file(GLOB_RECURSE SAVANXP_SXE_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/desktop/icons/*.png"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland/*.png")
add_custom_command(OUTPUT "${SAVANXP_SXE_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_SXE_ROOT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_sxe_resources.py"
            --project-root "${CMAKE_SOURCE_DIR}"
            --output-dir "${SAVANXP_SXE_ROOT}"
            --build-id "${SAVANXP_BUILD_ID}"
            --manifest-dir "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
            # El reproductor es el unico programa con fuentes en un subdirectorio, y
            # --manifest-dir NO es recursivo: sin esta linea su .sxres no se encuentra,
            # el binario queda sin estampar, el escaneo del lanzador no le ve categoria
            # y desaparece del catalogo sin decir nada.
            --manifest-dir "${CMAKE_SOURCE_DIR}/subsystems/posix/userland/mediaplayer"
            # The SUN programs that ship with the system, each in system/<name>.
            --manifest-dir "${CMAKE_SOURCE_DIR}/system/console"
            --manifest-dir "${CMAKE_SOURCE_DIR}/system/nightwood"
            ${SAVANXP_SXE_ARGUMENTS}
    COMMAND "${CMAKE_COMMAND}" -E touch "${SAVANXP_SXE_STAMP}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/gen_sxe_resources.py"
        "${CMAKE_SOURCE_DIR}/include/sxe/sxe_format.h"
        "${CMAKE_SOURCE_DIR}/include/abi/savanxp_native_abi.h"
        "${CMAKE_SOURCE_DIR}/include/abi/savanxp_abi.h"
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
    # After the resources: a native program is marked last, so nothing that
    # rewrites the ELF afterwards can drop the mark.
    if(target IN_LIST SAVANXP_NATIVE_TARGETS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/stamp_osabi.py"
                    --binary "$<TARGET_FILE:${target}>"
                    --abi-header "${CMAKE_SOURCE_DIR}/include/abi/savanxp_native_abi.h"
                    --installed-name "$<TARGET_FILE_NAME:${target}>"
            VERBATIM
        )
    endif()
endforeach()

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
    DEPENDS ${SAVANXP_USER_TARGETS}
    COMMENT "Linking SavanXP userland"
)

# Host tests deliberately use the normal Linux compiler and hosted linker;
# they are not part of the freestanding target graph.
add_executable(savanxp-gfx2d-test
    system/gfx/gfx2d.c
    subsystems/posix/sdk/v1/runtime/gfx_scaled.c
    tests/host/gfx2d_test.cpp)
target_include_directories(savanxp-gfx2d-test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
set_target_properties(savanxp-gfx2d-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

# SxGL on the host: the real rasterizer against a bitmap in memory, plus the
# triangles-per-second figure (docs/desktop/SXGL_ROADMAP.md#verification).
add_executable(savanxp-sxgl-test
    system/gfx/sxgl.c
    tests/host/sxgl_test.cpp)
target_include_directories(savanxp-sxgl-test PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(savanxp-sxgl-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
find_library(SAVANXP_HOST_LIBM m)
if(SAVANXP_HOST_LIBM)
    target_link_libraries(savanxp-sxgl-test PRIVATE ${SAVANXP_HOST_LIBM})
endif()

add_executable(savanxp-audio-test
    subsystems/posix/sdk/v1/runtime/audio.c
    tests/host/audio_mixer_test.cpp)
target_include_directories(savanxp-audio-test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
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
    "${CMAKE_SOURCE_DIR}/include"
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

# El decodificador PNG se compila con el compilador de host y se ejerce de
# verdad: PNG armados a mano (stored) y salidos de zlib real (fijo y dinamico),
# los cinco filtros, RGB y RGBA, mas el rechazo de lo que esta fuera del
# subconjunto. Es la unica verificacion del motor; el smoke en el invitado
# solo prueba que la libreria carga y que Paint lee un archivo.
add_executable(savanxp-sximg-test
    subsystems/posix/sdk/v1/runtime/sximg.c
    tests/host/sximg_test.cpp)
target_include_directories(savanxp-sximg-test PRIVATE
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
target_compile_options(savanxp-sximg-test PRIVATE -O1 -Wall -Wextra)
set_target_properties(savanxp-sximg-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

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

add_executable(savanxp-block-writeback-test
    kernel/block.cpp
    tests/host/kernel_string_stubs.cpp
    tests/host/block_writeback_test.cpp)
target_include_directories(savanxp-block-writeback-test PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tests/host")
target_compile_options(savanxp-block-writeback-test PRIVATE -O1 -Wall -Wextra -Wno-deprecated)
set_target_properties(savanxp-block-writeback-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)

add_executable(savanxp-sxfs-volume-test
    kernel/sxfs.cpp
    kernel/fs_path.cpp
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
    DEPENDS savanxp-gfx2d-test savanxp-sxgl-test savanxp-audio-test savanxp-partition-test savanxp-block-writeback-test savanxp-sxfs-volume-test savanxp-kernel-audio-gain-test savanxp-ccleste-wav-test savanxp-sxmidi-test savanxp-doom-mus-test savanxp-sximg-test
    COMMENT "Building SavanXP host tests")
add_test(NAME gfx2d-test COMMAND savanxp-gfx2d-test)
add_test(NAME sxgl-test COMMAND savanxp-sxgl-test)
add_test(NAME audio-test COMMAND savanxp-audio-test)
add_test(NAME kernel-audio-gain-test COMMAND savanxp-kernel-audio-gain-test)
add_test(NAME ccleste-wav-test COMMAND savanxp-ccleste-wav-test)
add_test(NAME sxmidi-test COMMAND savanxp-sxmidi-test)
add_test(NAME sximg-test COMMAND savanxp-sximg-test)
add_test(NAME doom-mus-test COMMAND savanxp-doom-mus-test)
add_test(NAME partition-test COMMAND savanxp-partition-test)
add_test(NAME block-writeback-test COMMAND savanxp-block-writeback-test)
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
add_test(NAME smoke-runner-layout-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/smoke_runner_layout_test.py"
            --root "${CMAKE_SOURCE_DIR}")
add_test(NAME tcp-echo-server-test
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tests/host/tcp_echo_server_test.py"
            --server "${CMAKE_SOURCE_DIR}/tools/tcp_echo_server.py")
add_dependencies(savanxp_tests savanxp_sxfs_cli)

# The initramfs is the rescue system, published under /rescue: what is left when
# no disk mounts (docs/kernel/FILESYSTEMS.md#the-rescue-system). A shell and the
# tools to look at and repair a volume, nothing graphical. Every program,
# these included, is staged onto the disk by savanxp_diskfs.
#
# imagetest is not a tool: it is here because it is the test of the in-memory
# executable reader, and /rescue is the only place that reader runs from.
set(SAVANXP_RESCUE_PROGRAMS
    init sh
    ls cat cp mv rm mkdir rmdir echo true false sleep ps truncate
    df fscheck sync sysinfo uname
    imagetest)
foreach(target IN LISTS SAVANXP_RESCUE_PROGRAMS)
    if(NOT target IN_LIST SAVANXP_USER_TARGETS)
        message(FATAL_ERROR "rescue program '${target}' is not a savanxp_program")
    endif()
endforeach()

set(SAVANXP_STAGE_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_ROOTFS_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_ROOTFS_ROOT}/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/rootfs/README"
            "${SAVANXP_ROOTFS_ROOT}/README"
)
foreach(target IN LISTS SAVANXP_RESCUE_PROGRAMS)
    list(APPEND SAVANXP_STAGE_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E copy
                "$<TARGET_FILE:${target}>"
                "${SAVANXP_ROOTFS_ROOT}/bin/$<TARGET_FILE_NAME:${target}>"
    )
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
    # Los binarios de verdad, no savanxp_userland. Ese es un objetivo agregado sin
    # salida propia, asi que como dependencia de un comando personalizado no aporta
    # ningun archivo: CMake no atraviesa objetivos personalizados para buscarles
    # salidas, y el stamp se declaraba al dia con el arbol de rootfs viejo mientras
    # los ejecutables ya eran nuevos.
    #
    # El efecto era que un cambio en cualquier programa no llegaba a la imagen. No
    # fallaba nada: el build pasaba, check_shared_libs pasaba, y la imagen arrancaba
    # con el binario anterior. No se nota mirando el build: si se rompe a proposito
    # el texto de una etiqueta de la barra, build y escenario dan OK los dos,
    # porque los dos proban el programa viejo. Solo se nota al buscar el binario
    # extraido de la imagen.
    DEPENDS ${SAVANXP_RESCUE_PROGRAMS}
            "${CMAKE_SOURCE_DIR}/rootfs/README"
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

set(SAVANXP_USER_LIBRARY_TARGETS)
get_property(SAVANXP_USER_LIBRARY_TARGETS GLOBAL PROPERTY SAVANXP_USER_LIBRARY_TARGETS)

# Shared libraries are staged inside savanxp_diskfs, after its rm -rf, or they
# would be wiped. Its COMMAND list is variadic and cannot host a foreach, so the
# commands are built into a list and expanded here.
set(SAVANXP_LIB_COPY_COMMANDS)
foreach(library IN LISTS SAVANXP_USER_LIBRARY_TARGETS)
    get_target_property(library_file ${library} OUTPUT_NAME)
    list(APPEND SAVANXP_LIB_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_BINARY_DIR}/${library_file}" "${SAVANXP_DISKFS_ROOT}/lib/")
endforeach()

# Every program the tree builds goes onto the disk, and /system/programs.txt
# names them: it is how Add or Remove Programs tells what shipped with the
# system from what someone installed (appwiz_catalog.h). One file name per line.
set(SAVANXP_PROGRAM_COPY_COMMANDS)
foreach(target IN LISTS SAVANXP_USER_TARGETS)
    list(APPEND SAVANXP_PROGRAM_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${target}>"
                "${SAVANXP_DISKFS_ROOT}/bin/$<TARGET_FILE_NAME:${target}>")
    string(APPEND SAVANXP_SYSTEM_PROGRAMS_CONTENT "$<TARGET_FILE_NAME:${target}>\n")
endforeach()
set(SAVANXP_SYSTEM_PROGRAMS "${CMAKE_BINARY_DIR}/system-programs.txt")
file(GENERATE OUTPUT "${SAVANXP_SYSTEM_PROGRAMS}" CONTENT "${SAVANXP_SYSTEM_PROGRAMS_CONTENT}")

add_custom_target(savanxp_diskfs
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_DISKFS_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/bin"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/lib"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/icons"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/system"
    ${SAVANXP_LIB_COPY_COMMANDS}
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/diskfs" "${SAVANXP_DISKFS_ROOT}"
    ${SAVANXP_PROGRAM_COPY_COMMANDS}
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_SYSTEM_PROGRAMS}" "${SAVANXP_DISKFS_ROOT}/system/programs.txt"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_mime_icons.py"
            --project-root "${CMAKE_SOURCE_DIR}"
            --output-dir "${SAVANXP_DISKFS_ROOT}/icons"
    DEPENDS ${SAVANXP_USER_TARGETS} ${SAVANXP_USER_LIBRARY_TARGETS}
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
            "${CMAKE_SOURCE_DIR}/boot/limine-dev.conf" "${SAVANXP_IMAGE_ROOT}/limine.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/boot/limine-dev.conf" "${SAVANXP_IMAGE_ROOT}/boot/limine/limine.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_KERNEL_ELF}" "${SAVANXP_IMAGE_ROOT}/boot/kernel.elf"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_INITRAMFS}" "${SAVANXP_IMAGE_ROOT}/boot/initramfs.cpio"
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
    DEPENDS savanxp_kernel savanxp_initramfs
    COMMENT "Staging the development boot tree"
    VERBATIM
)

# El arbol de la ISO es aparte y NO es el mismo: alla el volumen persistente
# viaja como modulo de Limine porque no hay disco adjunto, y el kernel lo monta
# como ramdisk "livecd". Un solo arbol no puede servir a los dos caminos, porque
# el dev no quiere el modulo (se carga en RAM y no se usa) y la ISO no puede
# prescindir de el. Ademas el dev no quiere disk.img dentro del arbol EFI: QEMU
# monta ese arbol como FAT16, que no llega a 512 MiB.
add_custom_target(savanxp_iso_root
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_ISO_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_ISO_ROOT}/boot/limine"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_ISO_ROOT}/EFI/BOOT"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/boot/limine.conf" "${SAVANXP_ISO_ROOT}/limine.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/boot/limine.conf" "${SAVANXP_ISO_ROOT}/boot/limine/limine.conf"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_KERNEL_ELF}" "${SAVANXP_ISO_ROOT}/boot/kernel.elf"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_INITRAMFS}" "${SAVANXP_ISO_ROOT}/boot/initramfs.cpio"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/iso_volume.py"
            --image "${SAVANXP_DISK_IMAGE}"
            --output "${SAVANXP_ISO_ROOT}/boot/disk.img"
            --cli "${SAVANXP_SXFS_CLI}"
            --sectors "${SAVANXP_ISO_SECTORS}"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_LIMINE_ROOT}/BOOTX64.EFI" "${SAVANXP_ISO_ROOT}/EFI/BOOT/BOOTX64.EFI"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_LIMINE_ROOT}/limine-bios-cd.bin" "${SAVANXP_ISO_ROOT}/boot/limine/limine-bios-cd.bin"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_LIMINE_ROOT}/limine-bios.sys" "${SAVANXP_ISO_ROOT}/boot/limine/limine-bios.sys"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SAVANXP_LIMINE_ROOT}/limine-uefi-cd.bin" "${SAVANXP_ISO_ROOT}/boot/limine/limine-uefi-cd.bin"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_SOURCE_DIR}/boot/startup.nsh" "${SAVANXP_ISO_ROOT}/startup.nsh"
    DEPENDS savanxp_kernel savanxp_initramfs savanxp_disk savanxp_sxfs_cli
    COMMENT "Staging the ISO boot tree"
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
                "${SAVANXP_ISO_ROOT}"
        COMMAND "${SAVANXP_LIMINE_INSTALLER}" bios-install "${SAVANXP_ISO_IMAGE}"
        DEPENDS savanxp_iso_root
        WORKING_DIRECTORY "${SAVANXP_ISO_ROOT}"
        COMMENT "Creating SavanXP.iso"
        VERBATIM
    )
endif()

add_custom_target(savanxp
    DEPENDS savanxp_image savanxp_disk
    COMMENT "Building SavanXP base system"
)

message(STATUS "SavanXP output root: ${SAVANXP_OUTPUT_ROOT}")
message(STATUS "SavanXP test applications: ${SAVANXP_INCLUDE_TEST_APPS}")
message(STATUS "SavanXP build id: ${SAVANXP_BUILD_ID}")
