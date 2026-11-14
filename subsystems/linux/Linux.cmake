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
# Damaged copies of hello the kernel must refuse.
set(SSL_DAMAGED "${SSL_OUT}/bad-header" "${SSL_OUT}/truncated" "${SSL_OUT}/other-machine")
add_custom_command(OUTPUT ${SSL_DAMAGED}
    COMMAND "${Python3_EXECUTABLE}" "${SSL_ROOT}/profile/damage.py" "${SSL_OUT}/hello" "${SSL_OUT}"
    DEPENDS "${SSL_OUT}/hello" "${SSL_ROOT}/profile/damage.py"
    COMMENT "Making the Linux images SSL must refuse"
    VERBATIM)
add_custom_target(savanxp_linux_fixtures
    DEPENDS "${SSL_OUT}/hello" "${SSL_OUT}/startup" "${SSL_OUT}/errors" ${SSL_DAMAGED})

add_test(NAME linux-fixture-hash-test
    COMMAND "${Python3_EXECUTABLE}" "${SSL_ROOT}/profile/check_hashes.py"
            "${SSL_EXPECTED}/fixtures.sha256" "${SSL_OUT}" hello startup errors)

set(SAVANXP_LINUX_COPY_COMMANDS "")
if(SAVANXP_INCLUDE_TEST_APPS)
    set(SSL_GUEST_FIXTURES "${SAVANXP_DISKFS_ROOT}/system/linux/fixtures")
    set(SAVANXP_LINUX_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SSL_GUEST_FIXTURES}/expected"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SSL_OUT}/hello" "${SSL_OUT}/startup" "${SSL_OUT}/errors" ${SSL_DAMAGED}
                "${SSL_GUEST_FIXTURES}/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SSL_EXPECTED}/hello.stdout" "${SSL_EXPECTED}/hello.status"
                "${SSL_EXPECTED}/startup.stdout" "${SSL_EXPECTED}/startup.status"
                "${SSL_EXPECTED}/errors.stdout" "${SSL_EXPECTED}/errors.status"
                "${SSL_GUEST_FIXTURES}/expected/")
endif()
