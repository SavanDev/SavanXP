# Camino de arranque de i386: un ISO BIOS puro con GRUB2 y Multiboot2
# (docs/REAL_HARDWARE.md). GRUB2 entra en modo protegido de 32 bits y entrega
# el control a boot/savanxp-kernel.elf; no hay arbol de desarrollo ni modulo
# Limine porque el kernel de arranque no monta el livecd todavia.
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
        DEPENDS savanxp_iso_root
        COMMENT "Creating SavanXP.iso (i386, GRUB2)"
        VERBATIM
    )
else()
    message(STATUS "grub-mkrescue or the i386-pc platform files were not found; the i386 ISO target is unavailable.")
endif()

add_custom_target(savanxp
    DEPENDS savanxp_kernel
    COMMENT "Building the i386 bootstrap kernel"
)
