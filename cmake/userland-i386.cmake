# Userland de i386. En x86_64 lo construye cmake/UserPrograms.cmake, con el
# loader de 64 bits y linker.ld en 0x400000; aqui se enlaza dentro de la ventana
# de usuario (linker32.ld) con el mismo runtime del SDK, que ya habla por
# int $0x80 igual en los dos ABI (docs/REAL_HARDWARE.md).

set(SAVANXP_I386_DISKFS_ROOT "${SAVANXP_OUTPUT_ROOT}/diskfs-i386")
set(SAVANXP_I386_DISK_IMAGE "${SAVANXP_OUTPUT_ROOT}/disk-i386.img")

# El runtime es el mismo que en x86_64 mas las rutinas de division de 64 bits,
# que en modo 32 bits no tienen instruccion. Con --gc-sections el linker se
 # queda solo con lo que el programa usa.
set(SAVANXP_I386_USER_RUNTIME_SOURCES
    ${SAVANXP_USER_RUNTIME_SOURCES}
    subsystems/posix/sdk/v1/runtime/div64.c
)

add_library(savanxp_i386_user_runtime OBJECT ${SAVANXP_I386_USER_RUNTIME_SOURCES})
target_include_directories(savanxp_i386_user_runtime PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
    "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
    "${SAVANXP_GENERATED_ROOT}"
)
target_compile_options(savanxp_i386_user_runtime PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})

# Fixed boot chain: sysinfo -> sh32 -> desktop32. keyecho is a standalone
# keyboard diagnostic. All four use the SDK runtime and the i386 linker script.
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

# El lector de eventos de teclado: diagnostico manual de la entrega por READ
# (fuera de la cadena sysinfo -> sh32: hace bucle infinito y bloquearia al shell).
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

# La shell minima: el segundo programa que el kernel lanza (sysinfo -> sh32).
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

# Fullscreen desktop, launched after sh32 exits; renders 64-row strips
# through /dev/gpu0 to keep its working buffer small.
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

add_custom_target(savanxp_i386_userland
    DEPENDS savanxp-i386-sysinfo savanxp-i386-keyecho savanxp-i386-sh32 savanxp-i386-desktop32
    COMMENT "Linking the i386 userland"
)

# Imagen propia: la del userland x86_64 no se toca (regla de persistencia) y esta
# solo lleva binarios de 32 bits.
add_custom_target(savanxp_i386_diskfs
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_I386_DISKFS_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_I386_DISKFS_ROOT}/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:savanxp-i386-sysinfo>" "${SAVANXP_I386_DISKFS_ROOT}/bin/sysinfo"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:savanxp-i386-keyecho>" "${SAVANXP_I386_DISKFS_ROOT}/bin/keyecho"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:savanxp-i386-sh32>" "${SAVANXP_I386_DISKFS_ROOT}/bin/sh32"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:savanxp-i386-desktop32>" "${SAVANXP_I386_DISKFS_ROOT}/bin/desktop32"
    DEPENDS savanxp_i386_userland
    COMMENT "Staging the i386 disk tree"
    VERBATIM
)

add_custom_target(savanxp_i386_disk
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/sxfs_sync.py"
            --image "${SAVANXP_I386_DISK_IMAGE}"
            --source "${SAVANXP_I386_DISKFS_ROOT}"
            --cli "${SAVANXP_SXFS_CLI}"
            --sectors "${SAVANXP_SXFS_SECTORS}"
    DEPENDS savanxp_i386_diskfs savanxp_sxfs_cli
    COMMENT "Synchronizing the i386 SxFS image"
    VERBATIM
)