# Win32 subsystem (docs/userland/WIN32.md). Three parts, all build-host for
# now:
#   - the PE32 parser, built for the host and tested against a synthetic
#     image and a malformed corpus with the address and undefined-behaviour
#     sanitizers on, since every byte it reads is untrusted;
#   - the 32-bit side: the subsystem's DLLs and the PE32 fixtures;
#   - win32-harness, which runs the fixtures through the loader and the gate.
#
# Tests that need the user's own Windows XP files read their folder from the
# SAVANXP_WIN32_FIXTURES cache variable or environment variable, and are
# reported as skipped without it. Those files never enter the tree or the
# build.

set(SAVANXP_WIN32_FIXTURES "$ENV{SAVANXP_WIN32_FIXTURES}" CACHE PATH
    "Folder holding the user's Windows XP sol.exe and cards.dll (optional)")

set(SAVANXP_WIN32_ROOT "${CMAKE_SOURCE_DIR}/subsystems/win32")

add_executable(savanxp-win32-pe-test
    "${SAVANXP_WIN32_ROOT}/pe/pe.c"
    tests/host/win32_pe_test.cpp)
target_include_directories(savanxp-win32-pe-test PRIVATE "${SAVANXP_WIN32_ROOT}/pe")
set_target_properties(savanxp-win32-pe-test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_compile_options(savanxp-win32-pe-test PRIVATE
    -Wall -Wextra -Werror -UNDEBUG
    -fsanitize=address,undefined -fno-sanitize-recover=all)
target_link_options(savanxp-win32-pe-test PRIVATE -fsanitize=address,undefined)
add_dependencies(savanxp_tests savanxp-win32-pe-test)

add_test(NAME win32-pe-test COMMAND savanxp-win32-pe-test)

# HKEY_CURRENT_USER's store: the file format, every error the services
# return, and data compaction.
add_executable(savanxp-win32-registry-test
    "${SAVANXP_WIN32_ROOT}/loader/registry.c"
    tests/host/win32_registry_test.c)
target_include_directories(savanxp-win32-registry-test PRIVATE
    "${SAVANXP_WIN32_ROOT}/include" "${SAVANXP_WIN32_ROOT}/loader")
set_target_properties(savanxp-win32-registry-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
target_compile_options(savanxp-win32-registry-test PRIVATE
    -Wall -Wextra -Werror -UNDEBUG -fsanitize=address,undefined -fno-sanitize-recover=all)
target_link_options(savanxp-win32-registry-test PRIVATE -fsanitize=address,undefined)
add_dependencies(savanxp_tests savanxp-win32-registry-test)
add_test(NAME win32-registry-test COMMAND savanxp-win32-registry-test)
# The pure parts of user32 and gdi32: regions, raster operations, lines.
add_executable(savanxp-win32-window-test
    "${SAVANXP_WIN32_ROOT}/dlls/common/region.c"
    "${SAVANXP_WIN32_ROOT}/dlls/gdi32/raster.c"
    tests/host/win32_window_test.c)
target_include_directories(savanxp-win32-window-test PRIVATE
    "${SAVANXP_WIN32_ROOT}/dlls/common" "${SAVANXP_WIN32_ROOT}/dlls/gdi32")
set_target_properties(savanxp-win32-window-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
target_compile_options(savanxp-win32-window-test PRIVATE
    -Wall -Wextra -Werror -UNDEBUG -fsanitize=address,undefined -fno-sanitize-recover=all)
target_link_options(savanxp-win32-window-test PRIVATE -fsanitize=address,undefined)
add_dependencies(savanxp_tests savanxp-win32-window-test)
add_test(NAME win32-window-test COMMAND savanxp-win32-window-test)
add_test(NAME win32-pe-fixtures-test
    COMMAND savanxp-win32-pe-test --fixtures "${SAVANXP_WIN32_FIXTURES}")
set_tests_properties(win32-pe-fixtures-test PROPERTIES SKIP_RETURN_CODE 77)

# ---- The 32-bit side: the subsystem's DLLs and the PE32 fixtures ----------
#
# Compiled by the project's clang for i686-pc-windows-msvc as freestanding
# code and linked by lld-link with no default libraries. DLLs go to
# win32/system32, fixtures (and the DLLs only a fixture uses) to
# win32/fixtures, and import libraries of DLLs that are never installed to
# win32/stubs.

find_program(SAVANXP_LLD_LINK NAMES lld-link REQUIRED)

set(SXW_OUT "${CMAKE_BINARY_DIR}/win32")
set(SXW_SYSTEM32 "${SXW_OUT}/system32")
set(SXW_FIXTURES "${SXW_OUT}/fixtures")
set(SXW_STUBS "${SXW_OUT}/stubs")
set(SXW_OBJ "${SXW_OUT}/obj")
set(SXW_LIB "${SXW_OUT}/lib")
file(MAKE_DIRECTORY "${SXW_SYSTEM32}" "${SXW_FIXTURES}" "${SXW_STUBS}" "${SXW_OBJ}" "${SXW_LIB}")

set(SXW_CFLAGS
    --target=i686-pc-windows-msvc -ffreestanding -fno-stack-protector -fno-builtin
    -O1 -Wall -Wextra -Werror -I "${SAVANXP_WIN32_ROOT}/include" -I "${SAVANXP_WIN32_ROOT}/dlls/common")
# /Brepro: the timestamp is a hash of the contents, so a fixture keeps its
# hash across builds and machines. Windows XP refuses an image whose
# subsystem version is above 5.1, which sxw_image sets.
set(SXW_LINKFLAGS /nodefaultlib /machine:x86 /Brepro)
set(SXW_ALL_OUTPUTS "")

# sxw_object(<var> <source>): compile one C file; <var> receives the object.
function(sxw_object var source)
    file(RELATIVE_PATH rel "${SAVANXP_WIN32_ROOT}" "${source}")
    string(REPLACE "/" "_" stem "${rel}")
    set(obj "${SXW_OBJ}/${stem}.obj")
    set(${var} "${obj}" PARENT_SCOPE)
    # One rule per object, however many images link it.
    get_property(made GLOBAL PROPERTY SXW_OBJECTS)
    if("${obj}" IN_LIST made)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY SXW_OBJECTS "${obj}")
    add_custom_command(OUTPUT "${obj}"
        COMMAND "${CMAKE_C_COMPILER}" ${SXW_CFLAGS} -MD -MF "${obj}.d" -c "${source}" -o "${obj}"
        DEPENDS "${source}"
        DEPFILE "${obj}.d"
        COMMENT "Compiling ${rel} (PE32)"
        VERBATIM)
endfunction()

# sxw_image(NAME <file> KIND dll|exe DIR <dir> SOURCES ... [DEF <def>]
#           [LIBS <names>...] [BASE <addr>] [ENTRY <symbol>] [SUBSYSTEM console|windows])
# LIBS name import libraries made by earlier sxw_image calls (without .lib).
function(sxw_image)
    cmake_parse_arguments(A "" "NAME;KIND;DIR;DEF;BASE;ENTRY;SUBSYSTEM" "SOURCES;LIBS" ${ARGN})
    set(objects "")
    foreach(source IN LISTS A_SOURCES)
        sxw_object(obj "${source}")
        list(APPEND objects "${obj}")
    endforeach()
    set(libs "")
    foreach(lib IN LISTS A_LIBS)
        list(APPEND libs "${SXW_LIB}/${lib}.lib")
    endforeach()
    if(NOT A_SUBSYSTEM)
        set(A_SUBSYSTEM windows)
    endif()
    set(flags ${SXW_LINKFLAGS} "/subsystem:${A_SUBSYSTEM},5.01" "/out:${A_DIR}/${A_NAME}")
    if(A_ENTRY)
        list(APPEND flags "/entry:${A_ENTRY}")
    elseif(A_KIND STREQUAL "dll")
        list(APPEND flags /noentry)
    endif()
    if(A_BASE)
        list(APPEND flags "/base:${A_BASE}")
    endif()
    set(outputs "${A_DIR}/${A_NAME}")
    if(A_KIND STREQUAL "dll")
        get_filename_component(stem "${A_NAME}" NAME_WE)
        if(A_DEF)
            get_filename_component(stem "${A_DEF}" NAME_WE)
        endif()
        list(APPEND flags /dll "/def:${A_DEF}" "/implib:${SXW_LIB}/${stem}.lib")
        list(APPEND outputs "${SXW_LIB}/${stem}.lib")
    endif()
    add_custom_command(OUTPUT ${outputs}
        COMMAND "${SAVANXP_LLD_LINK}" ${flags} ${objects} ${libs}
        DEPENDS ${objects} ${libs} ${A_DEF}
        COMMENT "Linking ${A_NAME} (PE32)"
        VERBATIM)
    set(SXW_ALL_OUTPUTS ${SXW_ALL_OUTPUTS} ${outputs} PARENT_SCOPE)
endfunction()

set(SXW_DLLS "${SAVANXP_WIN32_ROOT}/dlls")
set(SXW_FX "${SAVANXP_WIN32_ROOT}/fixtures")

# The DLL set. Bases are Windows XP's, so nothing of ours relocates.
sxw_image(NAME ntdll.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x7c900000
    SOURCES "${SXW_DLLS}/ntdll/ntdll.c" DEF "${SXW_DLLS}/ntdll/ntdll.def")
sxw_image(NAME kernel32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x7c800000
    SOURCES "${SXW_DLLS}/kernel32/kernel32.c" DEF "${SXW_DLLS}/kernel32/kernel32.def" LIBS ntdll)
sxw_image(NAME msvcrt.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x77c10000 ENTRY DllMain
    SOURCES "${SXW_DLLS}/msvcrt/msvcrt.c" "${SXW_DLLS}/common/codepage.c"
    DEF "${SXW_DLLS}/msvcrt/msvcrt.def" LIBS kernel32 ntdll)
sxw_image(NAME advapi32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x77dd0000
    SOURCES "${SXW_DLLS}/advapi32/advapi32.c" "${SXW_DLLS}/common/codepage.c"
    DEF "${SXW_DLLS}/advapi32/advapi32.def" LIBS ntdll)
sxw_image(NAME gdi32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x77f10000
    SOURCES "${SXW_DLLS}/gdi32/gdi32.c" "${SXW_DLLS}/gdi32/raster.c" "${SXW_DLLS}/common/region.c"
            "${SXW_DLLS}/common/memory.c"
    DEF "${SXW_DLLS}/gdi32/gdi32.def" LIBS ntdll)
sxw_image(NAME user32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x7e410000
    SOURCES "${SXW_DLLS}/user32/user32.c" "${SXW_DLLS}/common/region.c" "${SXW_DLLS}/common/memory.c"
    DEF "${SXW_DLLS}/user32/user32.def" LIBS gdi32 kernel32 ntdll)

# Fixtures: hello.exe; initorder.exe with alpha.dll and beta.dll, which
# share a base; missingdll.exe and missingsym.exe, whose loads must fail.
sxw_image(NAME hello.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/hello/hello.c" LIBS kernel32)
sxw_image(NAME beta.dll KIND dll DIR "${SXW_FIXTURES}" BASE 0x10000000 ENTRY DllMain
    SOURCES "${SXW_FX}/initorder/beta.c" DEF "${SXW_FX}/initorder/beta.def" LIBS kernel32)
sxw_image(NAME alpha.dll KIND dll DIR "${SXW_FIXTURES}" BASE 0x10000000 ENTRY DllMain
    SOURCES "${SXW_FX}/initorder/alpha.c" DEF "${SXW_FX}/initorder/alpha.def" LIBS beta kernel32)
sxw_image(NAME initorder.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/initorder/initorder.c" LIBS alpha beta kernel32)
sxw_image(NAME startup.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/startup/startup.c" LIBS msvcrt kernel32 advapi32)
sxw_image(NAME window.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/window/window.c" LIBS user32 gdi32 kernel32)
sxw_image(NAME nosuch.dll KIND dll DIR "${SXW_STUBS}"
    SOURCES "${SXW_FX}/missing/stub.c" DEF "${SXW_FX}/missing/nosuch.def")
sxw_image(NAME kernel32.dll KIND dll DIR "${SXW_STUBS}"
    SOURCES "${SXW_FX}/missing/stub.c" DEF "${SXW_FX}/missing/kernel32_extra.def")
sxw_image(NAME missingdll.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/missing/missing.c" LIBS nosuch kernel32)
sxw_image(NAME missingsym.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/missing/missing.c" LIBS kernel32_extra kernel32)

add_custom_target(savanxp_win32 DEPENDS ${SXW_ALL_OUTPUTS}
    COMMENT "Building the Win32 DLLs and fixtures")

# What the disk image carries (savanxp_diskfs expands the list): the DLL set
# in C:\windows\system32, and with the test applications the fixtures in
# C:\tests, where win32test runs them. Never a Microsoft file.
set(SXW_DRIVE_C "system/win/drive_c")
set(SAVANXP_WIN32_COPY_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/${SXW_DRIVE_C}/windows/system32"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SXW_SYSTEM32}/ntdll.dll" "${SXW_SYSTEM32}/kernel32.dll"
            "${SXW_SYSTEM32}/msvcrt.dll" "${SXW_SYSTEM32}/advapi32.dll"
            "${SXW_SYSTEM32}/gdi32.dll" "${SXW_SYSTEM32}/user32.dll"
            "${SAVANXP_DISKFS_ROOT}/${SXW_DRIVE_C}/windows/system32/")
if(SAVANXP_INCLUDE_TEST_APPS)
    list(APPEND SAVANXP_WIN32_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/${SXW_DRIVE_C}/tests"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SXW_FIXTURES}/hello.exe" "${SXW_FIXTURES}/initorder.exe"
                "${SXW_FIXTURES}/alpha.dll" "${SXW_FIXTURES}/beta.dll"
                "${SXW_FIXTURES}/missingdll.exe" "${SXW_FIXTURES}/missingsym.exe"
                "${SXW_FIXTURES}/startup.exe" "${SXW_FIXTURES}/window.exe"
                "${SAVANXP_DISKFS_ROOT}/${SXW_DRIVE_C}/tests/"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${SXW_FX}/expected" "${SAVANXP_DISKFS_ROOT}/${SXW_DRIVE_C}/tests/expected")
endif()

# ---- The host harness -----------------------------------------------------
#
# win32-harness runs a fixture on the Linux x86-64 build host through the
# same loader and DLLs as winhost, with a gate built on Linux's own 32-bit
# selectors and an LDT entry for the TEB.

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_executable(savanxp-win32-harness
        "${SAVANXP_WIN32_ROOT}/harness/harness.c"
        "${SAVANXP_WIN32_ROOT}/harness/gate_x86_64.S"
        "${SAVANXP_WIN32_ROOT}/loader/loader.c"
        "${SAVANXP_WIN32_ROOT}/loader/services.c"
        "${SAVANXP_WIN32_ROOT}/loader/registry.c"
        "${SAVANXP_WIN32_ROOT}/pe/pe.c")
    target_include_directories(savanxp-win32-harness PRIVATE
        "${SAVANXP_WIN32_ROOT}/include" "${SAVANXP_WIN32_ROOT}/pe" "${SAVANXP_WIN32_ROOT}/loader")
    set_target_properties(savanxp-win32-harness PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
    target_compile_options(savanxp-win32-harness PRIVATE -Wall -Wextra -Werror)
    add_dependencies(savanxp-win32-harness savanxp_win32)
    add_dependencies(savanxp_tests savanxp-win32-harness)

    add_test(NAME win32-harness-test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/host/win32_harness_test.py"
                --harness "$<TARGET_FILE:savanxp-win32-harness>"
                --system32 "${SXW_SYSTEM32}" --fixtures "${SXW_FIXTURES}"
                --expected "${SXW_FX}/expected")
endif()
