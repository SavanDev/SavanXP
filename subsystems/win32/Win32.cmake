# Win32 subsystem: build-host side (docs/userland/WIN32.md).
#
# The PE32 parser is portable C. Here it is built for the host and tested
# against a synthetic image and a malformed corpus, with the address and
# undefined-behaviour sanitizers on, since every byte it reads is untrusted.
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
add_test(NAME win32-pe-fixtures-test
    COMMAND savanxp-win32-pe-test --fixtures "${SAVANXP_WIN32_FIXTURES}")
set_tests_properties(win32-pe-fixtures-test PROPERTIES SKIP_RETURN_CODE 77)
