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
set(SSL_DYNAMIC_DAMAGED "")
if(EXISTS "${SAVANXP_LINUX_MUSL_ROOT}/usr/lib/libc.a" AND EXISTS "${SAVANXP_LINUX_MUSL_ROOT}/usr/lib/crt1.o")
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE SSL_CLANG_RESOURCES OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(SSL_MUSL "${SAVANXP_LINUX_MUSL_ROOT}")
    foreach(fixture IN ITEMS args tls heap clock files tools transport)
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
    set(SSL_DYNAMIC_FLAGS --target=x86_64-linux-musl -nostdinc
        -isystem "${SSL_MUSL}/usr/include" -isystem "${SSL_CLANG_RESOURCES}/include"
        -nostdlib -fuse-ld=lld -O2 -Wall -s -Wl,--build-id=none)
    add_custom_command(OUTPUT "${SSL_OUT}/libl4tls.so"
        COMMAND "${CMAKE_C_COMPILER}" ${SSL_DYNAMIC_FLAGS} -fPIC -shared -Wl,-soname,libl4tls.so
                -o "${SSL_OUT}/libl4tls.so" "${SSL_ROOT}/fixtures/dynamic/tlslib.c"
                "${SSL_MUSL}/usr/lib/libc.so"
        DEPENDS "${SSL_ROOT}/fixtures/dynamic/tlslib.c" "${SSL_MUSL}/usr/lib/libc.so" VERBATIM)
    add_custom_command(OUTPUT "${SSL_OUT}/dynamic"
        COMMAND "${CMAKE_C_COMPILER}" ${SSL_DYNAMIC_FLAGS} -fPIE -pie
                -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-z,now -Wl,-rpath,/fixtures
                -o "${SSL_OUT}/dynamic" "${SSL_MUSL}/usr/lib/crt1.o" "${SSL_MUSL}/usr/lib/crti.o"
                "${SSL_ROOT}/fixtures/dynamic/dynamic.c" "${SSL_OUT}/libl4tls.so"
                "${SSL_MUSL}/usr/lib/libc.so" "${SSL_MUSL}/usr/lib/crtn.o"
        DEPENDS "${SSL_ROOT}/fixtures/dynamic/dynamic.c" "${SSL_OUT}/libl4tls.so" VERBATIM)
    set(SSL_DYNAMIC_DAMAGED "${SSL_OUT}/interp-duplicate" "${SSL_OUT}/interp-nested" "${SSL_OUT}/interp-unterminated")
    add_custom_command(OUTPUT ${SSL_DYNAMIC_DAMAGED}
        COMMAND "${Python3_EXECUTABLE}" "${SSL_ROOT}/profile/damage.py" "${SSL_OUT}/dynamic" "${SSL_OUT}" --dynamic
        DEPENDS "${SSL_OUT}/dynamic" "${SSL_ROOT}/profile/damage.py" VERBATIM)
    list(APPEND SSL_MUSL_FIXTURES dynamic libl4tls.so)
    if(EXISTS "${SSL_MUSL}/usr/lib/libX11.so")
        add_custom_command(OUTPUT "${SSL_OUT}/xclient"
            COMMAND "${CMAKE_C_COMPILER}" ${SSL_DYNAMIC_FLAGS}
                -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 -Wl,-z,now
                -o "${SSL_OUT}/xclient" "${SSL_MUSL}/usr/lib/crt1.o" "${SSL_MUSL}/usr/lib/crti.o"
                "${SSL_ROOT}/fixtures/xclient/xclient.c" "-L${SSL_MUSL}/usr/lib" -lX11 -lc "${SSL_MUSL}/usr/lib/crtn.o"
            DEPENDS "${SSL_ROOT}/fixtures/xclient/xclient.c" "${SSL_MUSL}/usr/lib/libX11.so" VERBATIM)
        list(APPEND SSL_MUSL_FIXTURES xclient)
    endif()
else()
    message(STATUS "Linux musl fixtures: no Alpine root at ${SAVANXP_LINUX_MUSL_ROOT} "
        "(subsystems/linux/profile/profile.sh setup); linuxtest skips them")
endif()
list(TRANSFORM SSL_MUSL_FIXTURES PREPEND "${SSL_OUT}/" OUTPUT_VARIABLE SSL_MUSL_FILES)

add_custom_target(savanxp_linux_fixtures
    DEPENDS "${SSL_OUT}/hello" "${SSL_OUT}/startup" "${SSL_OUT}/errors" "${SSL_OUT}/regions" ${SSL_DAMAGED}
            ${SSL_MUSL_FILES} ${SSL_DYNAMIC_DAMAGED})

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
                ${SSL_MUSL_FILES} ${SSL_DYNAMIC_DAMAGED}
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
                "${SSL_EXPECTED}/tools-denied.stdout" "${SSL_EXPECTED}/tools-denied.status"
                "${SSL_EXPECTED}/tools.stdout" "${SSL_EXPECTED}/tools.status"
                "${SSL_EXPECTED}/files-write.stdout" "${SSL_EXPECTED}/files-write.status"
                "${SSL_EXPECTED}/files-check.stdout" "${SSL_EXPECTED}/files-check.status"
                "${SSL_EXPECTED}/files-remove.stdout" "${SSL_EXPECTED}/files-remove.status"
                "${SSL_GUEST_FIXTURES}/expected/")
endif()

# L3 uses the pinned distribution binary, unchanged.
if(EXISTS "${SAVANXP_LINUX_MUSL_ROOT}/bin/busybox.static")
    file(SHA256 "${SAVANXP_LINUX_MUSL_ROOT}/bin/busybox.static" SSL_BUSYBOX_HASH)
    if(NOT SSL_BUSYBOX_HASH STREQUAL "1690ed6db71fa35621537580135deb8de4f55f30a50d9d591050256e334cd8b9")
        message(FATAL_ERROR "Linux BusyBox does not match the pinned L0 profile")
    endif()
    if(SAVANXP_INCLUDE_TEST_APPS)
        list(APPEND SAVANXP_LINUX_COPY_COMMANDS
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/system/linux/bin"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SAVANXP_LINUX_MUSL_ROOT}/bin/busybox.static" "${SAVANXP_DISKFS_ROOT}/system/linux/bin/")
        foreach(case IN ITEMS echo cat ls pwd-root wc mkdir cat-missing)
            list(APPEND SAVANXP_LINUX_COPY_COMMANDS
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SSL_EXPECTED}/bb-${case}.stdout" "${SSL_EXPECTED}/bb-${case}.stderr" "${SSL_EXPECTED}/bb-${case}.status"
                "${SSL_GUEST_FIXTURES}/expected/")
        endforeach()
    endif()
endif()

# L4: stage the distribution's original executable and resolved library names.
if(EXISTS "${SAVANXP_LINUX_MUSL_ROOT}/usr/bin/jq" AND SAVANXP_INCLUDE_TEST_APPS)
    foreach(item IN ITEMS
        "usr/bin/jq|usr/bin/jq|08eee682e6f041c89a61b7b95bdfcdac182d4ced3c342f57884e603ace4430ed"
        "usr/lib/libjq.so.1.0.4|usr/lib/libjq.so.1|59d38cbad1e15be199449f7ebc1b1d74194d9bd50fe1ea3320257264577ee4ce"
        "usr/lib/libonig.so.5.5.0|usr/lib/libonig.so.5|5c2f25ea34db8ab4408ad220f8760fb037346c20f683bd16787963ac7308524f")
        string(REPLACE "|" ";" fields "${item}")
        list(GET fields 0 source)
        list(GET fields 1 destination)
        list(GET fields 2 expected_hash)
        file(SHA256 "${SAVANXP_LINUX_MUSL_ROOT}/${source}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Linux L4 profile hash mismatch: ${source}")
        endif()
        get_filename_component(directory "${destination}" DIRECTORY)
        list(APPEND SAVANXP_LINUX_COPY_COMMANDS
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/system/linux/${directory}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SAVANXP_LINUX_MUSL_ROOT}/${source}"
                    "${SAVANXP_DISKFS_ROOT}/system/linux/${destination}")
    endforeach()
    file(SHA256 "${SAVANXP_LINUX_MUSL_ROOT}/lib/ld-musl-x86_64.so.1" SSL_INTERPRETER_HASH)
    if(NOT SSL_INTERPRETER_HASH STREQUAL "095b047c631447e4fa50401fa7ffce01f678587040d65ba18c574981dc630de5")
        message(FATAL_ERROR "Linux L4 profile hash mismatch: musl interpreter")
    endif()
    list(APPEND SAVANXP_LINUX_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/system/linux/lib"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SAVANXP_LINUX_MUSL_ROOT}/lib/ld-musl-x86_64.so.1"
                "${SAVANXP_DISKFS_ROOT}/system/linux/lib/ld-musl-x86_64.so.1")
    foreach(case IN ITEMS jq jq-missing-lib jq-missing-interp dynamic)
        list(APPEND SAVANXP_LINUX_COPY_COMMANDS COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SSL_EXPECTED}/${case}.stdout" "${SSL_EXPECTED}/${case}.stderr" "${SSL_EXPECTED}/${case}.status"
            "${SSL_GUEST_FIXTURES}/expected/")
    endforeach()
endif()

# The same portable engine is tested through bytes and through a real Xlib
# connection on the host. It uses the platform painter in both environments.
add_library(savanxp_x11_host_engine STATIC
    "${SSL_ROOT}/x11/x11.c" "${CMAKE_SOURCE_DIR}/system/gfx/gfx2d.c" "${CMAKE_SOURCE_DIR}/system/gfx/pixels.c")
target_include_directories(savanxp_x11_host_engine PUBLIC "${CMAKE_SOURCE_DIR}/include")
add_executable(savanxp-linux-x11-test "${CMAKE_SOURCE_DIR}/tests/host/linux_x11_test.c")
target_link_libraries(savanxp-linux-x11-test PRIVATE savanxp_x11_host_engine)
add_test(NAME linux-x11-protocol-test COMMAND savanxp-linux-x11-test)
add_dependencies(savanxp_tests savanxp-linux-x11-test)
add_executable(savanxp-linux-x11-server "${CMAKE_SOURCE_DIR}/tests/host/linux_x11_server.c")
target_link_libraries(savanxp-linux-x11-server PRIVATE savanxp_x11_host_engine)
add_dependencies(savanxp_tests savanxp-linux-x11-server)

if(SSL_MUSL AND EXISTS "${SSL_MUSL}/usr/lib/libX11.so" AND SAVANXP_INCLUDE_TEST_APPS)
    foreach(item IN ITEMS
        "libX11.so.6.4.0|libX11.so.6|5a7c2df00148b914e9d66b7455680bd2a1b29a0eaef68b115a0133ce441d2a76"
        "libxcb.so.1.1.0|libxcb.so.1|fd7bc55e8680c183fe038b22d5cc7407c323e3b1e1bd6ee488b57137124b0292"
        "libXau.so.6.0.0|libXau.so.6|0fd15be141dc06c149f2f2b92408dc91f4429d1f872a3fb9a655490e8c994578"
        "libXdmcp.so.6.0.0|libXdmcp.so.6|cb4e243ca95695d59f1de5abc8cb0092d5ac1066811302c0f874a9341778b7eb"
        "libbsd.so.0.12.2|libbsd.so.0|a5b321623fe7274c695805b7f46bb2ed8ac4e970413a4930f09701a17c44ed21"
        "libmd.so.0.2.0|libmd.so.0|4fd22b05b1297facf99c57988915345ff0ba033fbb2c6f07e4e3bb41fdedfeea")
        string(REPLACE "|" ";" fields "${item}")
        list(GET fields 0 source)
        list(GET fields 1 destination)
        list(GET fields 2 expected_hash)
        file(SHA256 "${SSL_MUSL}/usr/lib/${source}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Linux L5 profile hash mismatch: ${source}")
        endif()
        list(APPEND SAVANXP_LINUX_COPY_COMMANDS
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/system/linux/usr/lib"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SSL_MUSL}/usr/lib/${source}"
                "${SAVANXP_DISKFS_ROOT}/system/linux/usr/lib/${destination}")
    endforeach()
endif()
add_test(NAME linux-x11-xlib-test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/host/linux_xclient_test.py"
        --server "$<TARGET_FILE:savanxp-linux-x11-server>" --root "${SAVANXP_LINUX_MUSL_ROOT}" --fixture "${SSL_OUT}/xclient")
set_tests_properties(linux-x11-xlib-test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
add_test(NAME linux-transport-reference-test
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/host/linux_transport_test.py" "${SSL_OUT}/transport")
set_tests_properties(linux-transport-reference-test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
