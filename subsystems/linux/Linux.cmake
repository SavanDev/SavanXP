# SSL, the Linux subsystem (docs/userland/LINUX.md). The kernel half is
# compiled with the kernel (CMakeLists.txt); this file builds the Linux
# fixtures and puts them, with the outputs Linux gave for them, into the
# guest tree under /system/linux/fixtures. Test images only.
#
# The fixtures are built exactly as subsystems/linux/profile/profile.sh
# builds them on the reference host, so each is the same file, by hash, that
# produced the expected output; linux-fixture-hash-test says when they drift.

set(SSL_ROOT "${CMAKE_SOURCE_DIR}/subsystems/linux")
set(SSL_OUT "${CMAKE_BINARY_DIR}/linux/fixtures")
set(SSL_EXPECTED "${SSL_ROOT}/profile/expected")
file(MAKE_DIRECTORY "${SSL_OUT}")

set(SSL_TARGET --target=x86_64-pc-linux-gnu)
set(SSL_FREESTANDING ${SSL_TARGET} -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector -O1
    -fuse-ld=lld -s -Wl,--build-id=none)

add_custom_command(OUTPUT "${SSL_OUT}/hello"
    COMMAND "${CMAKE_C_COMPILER}" ${SSL_TARGET} -nostdlib -static -fuse-ld=lld -Wl,--build-id=none
            -o "${SSL_OUT}/hello" "${SSL_ROOT}/fixtures/hello/hello.S"
    DEPENDS "${SSL_ROOT}/fixtures/hello/hello.S"
    COMMENT "Building the Linux fixture hello"
    VERBATIM)
add_custom_command(OUTPUT "${SSL_OUT}/startup"
    COMMAND "${CMAKE_C_COMPILER}" ${SSL_FREESTANDING} -o "${SSL_OUT}/startup"
            "${SSL_ROOT}/fixtures/startup/entry.S" "${SSL_ROOT}/fixtures/startup/startup.c"
    DEPENDS "${SSL_ROOT}/fixtures/startup/entry.S" "${SSL_ROOT}/fixtures/startup/startup.c"
    COMMENT "Building the Linux fixture startup"
    VERBATIM)
add_custom_command(OUTPUT "${SSL_OUT}/errors"
    COMMAND "${CMAKE_C_COMPILER}" ${SSL_FREESTANDING} -o "${SSL_OUT}/errors"
            "${SSL_ROOT}/fixtures/errors/errors.c"
    DEPENDS "${SSL_ROOT}/fixtures/errors/errors.c"
    COMMENT "Building the Linux fixture errors"
    VERBATIM)
add_custom_command(OUTPUT "${SSL_OUT}/regions"
    COMMAND "${CMAKE_C_COMPILER}" ${SSL_FREESTANDING} -o "${SSL_OUT}/regions"
            "${SSL_ROOT}/fixtures/regions/regions.c"
    DEPENDS "${SSL_ROOT}/fixtures/regions/regions.c"
    COMMENT "Building the Linux fixture regions"
    VERBATIM)
# Damaged copies of hello the kernel must refuse.
set(SSL_DAMAGED "${SSL_OUT}/bad-header" "${SSL_OUT}/truncated" "${SSL_OUT}/other-machine")
add_custom_command(OUTPUT ${SSL_DAMAGED}
    COMMAND "${Python3_EXECUTABLE}" "${SSL_ROOT}/profile/damage.py" "${SSL_OUT}/hello" "${SSL_OUT}"
    DEPENDS "${SSL_OUT}/hello" "${SSL_ROOT}/profile/damage.py"
    COMMENT "Making the Linux images SSL must refuse"
    VERBATIM)

# The static musl fixtures of L2 link against the pinned musl-dev of the L0
# profile's Alpine root, which `profile.sh setup` fetches by hash; nothing of
# it is committed. Without that root they are not built and linuxtest skips
# them. The command is profile.sh's musl_static, so the bytes are the same.
set(SAVANXP_LINUX_MUSL_ROOT "${CMAKE_SOURCE_DIR}/build/linux-profile/root" CACHE PATH
    "Alpine root made by subsystems/linux/profile/profile.sh setup, for the static musl fixtures")
set(SSL_MUSL_FIXTURES "")
if(EXISTS "${SAVANXP_LINUX_MUSL_ROOT}/usr/lib/libc.a" AND EXISTS "${SAVANXP_LINUX_MUSL_ROOT}/usr/lib/crt1.o")
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE SSL_CLANG_RESOURCES OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(SSL_MUSL "${SAVANXP_LINUX_MUSL_ROOT}")
    foreach(fixture IN ITEMS args tls heap clock files)
        add_custom_command(OUTPUT "${SSL_OUT}/${fixture}"
            COMMAND "${CMAKE_C_COMPILER}" --target=x86_64-linux-musl -nostdinc
                    -isystem "${SSL_MUSL}/usr/include" -isystem "${SSL_CLANG_RESOURCES}/include"
                    -static -nostdlib -fuse-ld=lld -O2 -Wall -s -Wl,--build-id=none
                    -o "${SSL_OUT}/${fixture}" "${SSL_MUSL}/usr/lib/crt1.o" "${SSL_MUSL}/usr/lib/crti.o"
                    "${SSL_ROOT}/fixtures/static/${fixture}.c"
                    "${SSL_MUSL}/usr/lib/libc.a" "${SSL_MUSL}/usr/lib/crtn.o"
            DEPENDS "${SSL_ROOT}/fixtures/static/${fixture}.c" "${SSL_MUSL}/usr/lib/libc.a"
            COMMENT "Building the Linux fixture ${fixture} (static musl)"
            VERBATIM)
        list(APPEND SSL_MUSL_FIXTURES "${fixture}")
    endforeach()
else()
    message(STATUS "Linux musl fixtures: no Alpine root at ${SAVANXP_LINUX_MUSL_ROOT} "
        "(subsystems/linux/profile/profile.sh setup); linuxtest skips them")
endif()
list(TRANSFORM SSL_MUSL_FIXTURES PREPEND "${SSL_OUT}/" OUTPUT_VARIABLE SSL_MUSL_FILES)

add_custom_target(savanxp_linux_fixtures
    DEPENDS "${SSL_OUT}/hello" "${SSL_OUT}/startup" "${SSL_OUT}/errors" "${SSL_OUT}/regions" ${SSL_DAMAGED}
            ${SSL_MUSL_FILES})

add_test(NAME linux-fixture-hash-test
    COMMAND "${Python3_EXECUTABLE}" "${SSL_ROOT}/profile/check_hashes.py"
            "${SSL_EXPECTED}/fixtures.sha256" "${SSL_OUT}" hello startup errors regions ${SSL_MUSL_FIXTURES})

set(SAVANXP_LINUX_COPY_COMMANDS "")
if(SAVANXP_INCLUDE_TEST_APPS)
    set(SSL_GUEST_FIXTURES "${SAVANXP_DISKFS_ROOT}/system/linux/fixtures")
    set(SAVANXP_LINUX_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SSL_GUEST_FIXTURES}/expected"
        # Where the files fixture writes, as /work in the guest.
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/system/linux/work"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SSL_OUT}/hello" "${SSL_OUT}/startup" "${SSL_OUT}/errors" "${SSL_OUT}/regions" ${SSL_DAMAGED}
                ${SSL_MUSL_FILES}
                "${SSL_GUEST_FIXTURES}/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SSL_EXPECTED}/hello.stdout" "${SSL_EXPECTED}/hello.status"
                "${SSL_EXPECTED}/startup.stdout" "${SSL_EXPECTED}/startup.status"
                "${SSL_EXPECTED}/errors.stdout" "${SSL_EXPECTED}/errors.status"
                "${SSL_EXPECTED}/regions.stdout" "${SSL_EXPECTED}/regions.status"
                "${SSL_EXPECTED}/args.stdout" "${SSL_EXPECTED}/args.status"
                "${SSL_EXPECTED}/tls.stdout" "${SSL_EXPECTED}/tls.status"
                "${SSL_EXPECTED}/heap.stdout" "${SSL_EXPECTED}/heap.status"
                "${SSL_EXPECTED}/clock.stdout" "${SSL_EXPECTED}/clock.status"
                "${SSL_EXPECTED}/files-write.stdout" "${SSL_EXPECTED}/files-write.status"
                "${SSL_EXPECTED}/files-check.stdout" "${SSL_EXPECTED}/files-check.status"
                "${SSL_EXPECTED}/files-remove.stdout" "${SSL_EXPECTED}/files-remove.status"
                "${SSL_GUEST_FIXTURES}/expected/")
endif()
