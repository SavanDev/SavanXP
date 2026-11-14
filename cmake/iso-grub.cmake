# GRUB2/Multiboot2 is the only boot path on this branch.
find_program(SAVANXP_GRUB_MKRESCUE grub-mkrescue)
find_path(SAVANXP_GRUB_I386_PC cdboot.img
    PATHS /usr/lib/grub/i386-pc /usr/share/grub/i386-pc
    NO_DEFAULT_PATH
)

if(SAVANXP_GRUB_MKRESCUE AND SAVANXP_GRUB_I386_PC)
    add_custom_target(savanxp_iso_root
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SAVANXP_ISO_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_ISO_ROOT}/boot/grub"
        COMMAND "${CMAKE_COMMAND}" -E copy
                "${SAVANXP_KERNEL_ELF}" "${SAVANXP_ISO_ROOT}/boot/savanxp-kernel.elf"
        COMMAND "${CMAKE_COMMAND}" -E copy
                "${CMAKE_SOURCE_DIR}/boot/grub-i386.cfg" "${SAVANXP_ISO_ROOT}/boot/grub/grub.cfg"
        DEPENDS savanxp_kernel
        COMMENT "Staging the i386 ISO boot tree"
        VERBATIM
    )
    add_custom_target(savanxp_iso
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${SAVANXP_ISO_IMAGE}"
        COMMAND "${SAVANXP_GRUB_MKRESCUE}"
                --directory "${SAVANXP_GRUB_I386_PC}"
                --output "${SAVANXP_ISO_IMAGE}"
                "${SAVANXP_ISO_ROOT}"
        DEPENDS savanxp_iso_root savanxp_disk
        COMMENT "Creating SavanXP.iso (i386, GRUB2)"
        VERBATIM
    )
else()
    message(STATUS "grub-mkrescue or the i386-pc platform files were not found; the i386 ISO target is unavailable.")
endif()

add_custom_target(savanxp
    DEPENDS savanxp_kernel savanxp_i386_disk
    COMMENT "Building the i386 kernel and its disk image"
)

set(SAVANXP_QEMU_ARGS
    --iso "${SAVANXP_ISO_IMAGE}" --disk-image "${SAVANXP_DISK_IMAGE}"
    --accel "${SAVANXP_ACCEL}")
if(SAVANXP_HEADLESS)
    list(APPEND SAVANXP_QEMU_ARGS --headless)
endif()
foreach(mode run debug)
    set(mode_args)
    if(mode STREQUAL "debug")
        set(mode_args --debug)
    endif()
    add_custom_target(savanxp_${mode}
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/run_qemu.py"
                ${SAVANXP_QEMU_ARGS} ${mode_args}
        DEPENDS savanxp_iso savanxp_disk
        USES_TERMINAL VERBATIM)
endforeach()
