# Camino de arranque de x86_64: Limine (BIOS y UEFI), su arbol de desarrollo
# y su ISO. Solo se incluye en x86_64; i386 arranca con GRUB2 y Multiboot2
# (docs/REAL_HARDWARE.md).
#
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
