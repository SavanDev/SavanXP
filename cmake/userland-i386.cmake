# ELF32 programs share the SDK runtime and the private user address window.
set(SAVANXP_I386_USER_RUNTIME_SOURCES
    ${SAVANXP_USER_RUNTIME_SOURCES}
    subsystems/posix/sdk/v1/runtime/div64.c
    subsystems/posix/sdk/v1/runtime/bootstrap32.c
)

add_library(savanxp_i386_user_runtime OBJECT ${SAVANXP_I386_USER_RUNTIME_SOURCES})
target_include_directories(savanxp_i386_user_runtime PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp_i386_user_runtime PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})

# Userland init owns startup. The shell launches independent programs through
# the same SDK runtime and ELF32 linker script as the diagnostic clients.
add_executable(savanxp-i386-sysinfo subsystems/posix/userland/sysinfo.c)
target_include_directories(savanxp-i386-sysinfo PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp-i386-sysinfo PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
target_link_libraries(savanxp-i386-sysinfo PRIVATE savanxp_i386_user_runtime)
target_link_options(savanxp-i386-sysinfo PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
set_target_properties(savanxp-i386-sysinfo PROPERTIES OUTPUT_NAME sysinfo SUFFIX "")

# Manual keyboard-event diagnostic, launched explicitly from the shell.
if(SAVANXP_INCLUDE_TEST_APPS)
add_executable(savanxp-i386-keyecho subsystems/posix/userland/keyecho.c)
target_include_directories(savanxp-i386-keyecho PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp-i386-keyecho PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
target_link_libraries(savanxp-i386-keyecho PRIVATE savanxp_i386_user_runtime)
target_link_options(savanxp-i386-keyecho PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
set_target_properties(savanxp-i386-keyecho PROPERTIES OUTPUT_NAME keyecho SUFFIX "")

endif()

# Interactive shell, launched and restarted by init32.
add_executable(savanxp-i386-sh32 subsystems/posix/userland/sh32.c)
target_include_directories(savanxp-i386-sh32 PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp-i386-sh32 PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
target_link_libraries(savanxp-i386-sh32 PRIVATE savanxp_i386_user_runtime)
target_link_options(savanxp-i386-sh32 PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
set_target_properties(savanxp-i386-sh32 PROPERTIES OUTPUT_NAME sh32 SUFFIX "")

# Fullscreen diagnostic retained beside the reference graphical session.
add_executable(savanxp-i386-desktop32 subsystems/posix/userland/desktop32.c)
target_include_directories(savanxp-i386-desktop32 PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp-i386-desktop32 PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
target_link_libraries(savanxp-i386-desktop32 PRIVATE savanxp_i386_user_runtime)
target_link_options(savanxp-i386-desktop32 PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
set_target_properties(savanxp-i386-desktop32 PROPERTIES OUTPUT_NAME desktop32 SUFFIX "")

# PID 1 owns process startup and restart policy.
add_executable(savanxp-i386-init32 subsystems/posix/userland/init32.c)
target_include_directories(savanxp-i386-init32 PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp-i386-init32 PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
target_link_libraries(savanxp-i386-init32 PRIVATE savanxp_i386_user_runtime)
target_link_options(savanxp-i386-init32 PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
set_target_properties(savanxp-i386-init32 PROPERTIES OUTPUT_NAME init32 SUFFIX "")

if(SAVANXP_INCLUDE_TEST_APPS)
# Runtime regression: two CPU-bound workers plus process-lifecycle checks.
add_executable(savanxp-i386-multitest32
    subsystems/posix/userland/multitest32.c
    subsystems/posix/userland/context_probe32.S
)
target_include_directories(savanxp-i386-multitest32 PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp-i386-multitest32 PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
target_link_libraries(savanxp-i386-multitest32 PRIVATE savanxp_i386_user_runtime)
target_link_options(savanxp-i386-multitest32 PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
set_target_properties(savanxp-i386-multitest32 PROPERTIES OUTPUT_NAME multitest32 SUFFIX "")

add_executable(savanxp-i386-memhog32 subsystems/posix/userland/memhog32.c)
target_include_directories(savanxp-i386-memhog32 PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}")
target_compile_options(savanxp-i386-memhog32 PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
target_link_libraries(savanxp-i386-memhog32 PRIVATE savanxp_i386_user_runtime)
target_link_options(savanxp-i386-memhog32 PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
set_target_properties(savanxp-i386-memhog32 PROPERTIES OUTPUT_NAME memhog32 SUFFIX "")

endif()

# Reuse the reference console shell and BusyBox subset with the static ELF32 ABI.
function(savanxp_i386_program name)
    add_executable(savanxp-i386-${name} ${ARGN})
    target_include_directories(savanxp-i386-${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
        "${SAVANXP_GENERATED_ROOT}")
    target_compile_options(savanxp-i386-${name} PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
    target_link_libraries(savanxp-i386-${name} PRIVATE savanxp_i386_user_runtime)
    target_link_options(savanxp-i386-${name} PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
    set_target_properties(savanxp-i386-${name} PROPERTIES OUTPUT_NAME ${name} SUFFIX "")
endfunction()
savanxp_i386_program(sh subsystems/posix/userland/sh.c subsystems/posix/userland/shell_core.c)
file(GLOB SAVANXP_BUSYBOX_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/vendor/busybox-port/*.c")
list(SORT SAVANXP_BUSYBOX_SOURCES)
savanxp_i386_program(busybox ${SAVANXP_BUSYBOX_SOURCES})
target_include_directories(savanxp-i386-busybox PRIVATE "${CMAKE_SOURCE_DIR}/vendor/busybox-port")
set(SAVANXP_I386_CORE_APPS uname df rmdir truncate sync errtest netinfo ping udpsend udprecv tcpget beep volume fscheck)
foreach(name IN LISTS SAVANXP_I386_CORE_APPS)
    savanxp_i386_program(${name} subsystems/posix/userland/${name}.c)
endforeach()

if(SAVANXP_INCLUDE_TEST_APPS)
    set(SAVANXP_I386_POSIX_TESTS sdkint32 vmtest32 heaptest mmaptest posixtest32 fdtest waittest pipestress seektest renametest truncatetest eventtest timertest semaphoretest handletest sxetest nettest udptest tcptest audiotest cliptest devtest32)
    foreach(name IN LISTS SAVANXP_I386_POSIX_TESTS)
        savanxp_i386_program(${name} subsystems/posix/userland/${name}.c)
    endforeach()
    savanxp_i386_program(nativeguihost subsystems/native/test/guihost.c)
    savanxp_i386_program(nativesxguihost subsystems/native/test/sxguihost.c)
    list(APPEND SAVANXP_I386_POSIX_TESTS nativeguihost nativesxguihost)
endif()

add_custom_target(savanxp_i386_userland
    DEPENDS savanxp-i386-init32 savanxp-i386-sysinfo savanxp-i386-sh32 savanxp-i386-desktop32 savanxp-i386-sh savanxp-i386-busybox
    COMMENT "Linking the i386 userland"
)

# Additive synchronization preserves external apps and persistent game assets.
add_custom_target(savanxp_i386_diskfs
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_DISKFS_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/diskfs" "${SAVANXP_DISKFS_ROOT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_mime_icons.py"
            --project-root "${CMAKE_SOURCE_DIR}" --output-dir "${SAVANXP_DISKFS_ROOT}/icons"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:savanxp-i386-sysinfo>" "${SAVANXP_DISKFS_ROOT}/bin/sysinfo"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:savanxp-i386-sh32>" "${SAVANXP_DISKFS_ROOT}/bin/sh32"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:savanxp-i386-desktop32>" "${SAVANXP_DISKFS_ROOT}/bin/desktop32"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:savanxp-i386-init32>" "${SAVANXP_DISKFS_ROOT}/bin/init32"
    DEPENDS savanxp_i386_userland
    COMMENT "Staging the i386 disk tree"
    VERBATIM
)

set(SAVANXP_I386_APPLETS busybox ls cat echo mkdir rm mv cp ps true false sleep)
foreach(name IN LISTS SAVANXP_I386_APPLETS)
    add_custom_command(TARGET savanxp_i386_diskfs POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:savanxp-i386-busybox>" "${SAVANXP_DISKFS_ROOT}/bin/${name}"
        VERBATIM)
endforeach()
foreach(name IN ITEMS sh ${SAVANXP_I386_CORE_APPS} ${SAVANXP_I386_POSIX_TESTS})
    add_dependencies(savanxp_i386_userland savanxp-i386-${name})
    add_custom_command(TARGET savanxp_i386_diskfs POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:savanxp-i386-${name}>" "${SAVANXP_DISKFS_ROOT}/bin/${name}"
        VERBATIM)
endforeach()

include("${CMAKE_SOURCE_DIR}/cmake/elf32-libraries.cmake")

# Restore the reference read-only /bin and /README through its existing CPIO
# format. The persistent /disk/bin remains the first command search location.
set(SAVANXP_I386_ROOT_PROGRAMS ${SAVANXP_I386_APPLETS} ${SAVANXP_I386_CORE_APPS}
    init32 sysinfo sh32 desktop32 sh ${SAVANXP_I386_DYNAMIC_PROGRAMS})
if(SAVANXP_INCLUDE_TEST_APPS)
    list(APPEND SAVANXP_I386_ROOT_PROGRAMS keyecho multitest32 memhog32 ${SAVANXP_I386_POSIX_TESTS})
endif()
set(SAVANXP_I386_ROOT_STRIP_COMMANDS)
foreach(name IN LISTS SAVANXP_I386_ROOT_PROGRAMS)
    list(APPEND SAVANXP_I386_ROOT_STRIP_COMMANDS
        COMMAND "${SAVANXP_OBJCOPY}" --strip-unneeded "${SAVANXP_ROOTFS_ROOT}/bin/${name}")
endforeach()
foreach(name IN LISTS SAVANXP_I386_LIBRARIES)
    if(name STREQUAL "libffmpeg.so.0.4")
        continue() # Optional codec engine is loaded from the persistent disk.
    endif()
    list(APPEND SAVANXP_I386_ROOT_STRIP_COMMANDS
        COMMAND "${SAVANXP_OBJCOPY}" --strip-unneeded "${SAVANXP_ROOTFS_ROOT}/lib/${name}")
endforeach()
add_custom_target(savanxp_i386_initramfs
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_ROOTFS_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_ROOTFS_ROOT}/bin" "${SAVANXP_ROOTFS_ROOT}/lib"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SAVANXP_DISKFS_ROOT}/bin" "${SAVANXP_ROOTFS_ROOT}/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SAVANXP_DISKFS_ROOT}/lib" "${SAVANXP_ROOTFS_ROOT}/lib"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/rootfs/README" "${SAVANXP_ROOTFS_ROOT}/README"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${SAVANXP_ROOTFS_ROOT}/lib/libffmpeg.so.0.4"
    ${SAVANXP_I386_ROOT_STRIP_COMMANDS}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/mkinitramfs.py"
        --source "${SAVANXP_ROOTFS_ROOT}" --output "${SAVANXP_INITRAMFS}"
    DEPENDS savanxp_i386_diskfs
    COMMENT "Building the reference CPIO root for i386"
    VERBATIM)

add_custom_target(savanxp_i386_disk
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/sxfs_sync.py"
            --image "${SAVANXP_DISK_IMAGE}"
            --source "${SAVANXP_DISKFS_ROOT}"
            --cli "${SAVANXP_SXFS_CLI}"
            --sectors "${SAVANXP_SXFS_SECTORS}"
    DEPENDS savanxp_i386_diskfs savanxp_sxfs_cli
    COMMENT "Synchronizing the i386 SxFS image"
    VERBATIM
)
add_custom_target(savanxp_userland DEPENDS savanxp_i386_userland)
add_custom_target(savanxp_diskfs DEPENDS savanxp_i386_diskfs)
add_custom_target(savanxp_disk DEPENDS savanxp_i386_disk)

if(SAVANXP_INCLUDE_TEST_APPS)
    add_dependencies(savanxp_i386_userland savanxp-i386-keyecho savanxp-i386-multitest32 savanxp-i386-memhog32)
    add_custom_command(TARGET savanxp_i386_diskfs POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:savanxp-i386-keyecho>" "${SAVANXP_DISKFS_ROOT}/bin/keyecho"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:savanxp-i386-multitest32>" "${SAVANXP_DISKFS_ROOT}/bin/multitest32"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:savanxp-i386-memhog32>" "${SAVANXP_DISKFS_ROOT}/bin/memhog32"
        VERBATIM)
endif()
