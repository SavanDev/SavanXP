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
set(SAVANXP_WIN32_WORDPAD "$ENV{SAVANXP_WIN32_WORDPAD}" CACHE PATH
    "Folder holding the user's Windows XP wordpad.exe, with mfc42u.dll and mfc42loc.dll in system32/ (optional)")
set(SAVANXP_WIN32_TEXT "$ENV{SAVANXP_WIN32_TEXT}" CACHE PATH
    "Folder holding Windows XP's x.out for text.exe and the user's XP fonts in fonts/ (optional)")

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
# The pure parts of user32 and gdi32: regions, raster operations, lines,
# DIB decoding and blits.
add_executable(savanxp-win32-window-test
    "${SAVANXP_WIN32_ROOT}/dlls/common/region.c"
    "${SAVANXP_WIN32_ROOT}/dlls/gdi32/raster.c"
    "${SAVANXP_WIN32_ROOT}/dlls/gdi32/dib.c"
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
find_program(SAVANXP_LLVM_RC NAMES llvm-rc REQUIRED)

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

# sxw_object(<var> <source> [flags...]): compile one source file.
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
    set(compiler "${CMAKE_C_COMPILER}")
    if(source MATCHES "\\.cpp$")
        set(compiler "${CMAKE_CXX_COMPILER}")
    endif()
    add_custom_command(OUTPUT "${obj}"
        COMMAND "${compiler}" ${SXW_CFLAGS} ${ARGN} -MD -MF "${obj}.d" -c "${source}" -o "${obj}"
        DEPENDS "${source}"
        DEPFILE "${obj}.d"
        COMMENT "Compiling ${rel} (PE32)"
        VERBATIM)
endfunction()

# sxw_image(NAME <file> KIND dll|exe DIR <dir> SOURCES ... [DEF <def>]
#           [LIBS <names>...] [BASE <addr>] [ENTRY <symbol>] [SUBSYSTEM console|windows]
#           [FLAGS ...] [SAFESEH] [RC <script> RC_DEPENDS <files>... RC_INCLUDE <dir>])
# LIBS name import libraries made by earlier sxw_image calls (without .lib).
# RC compiles a resource script with llvm-rc, looking for the files it names
# in RC_INCLUDE.
function(sxw_image)
    cmake_parse_arguments(A "SAFESEH" "NAME;KIND;DIR;DEF;BASE;ENTRY;SUBSYSTEM;RC;RC_INCLUDE"
        "SOURCES;LIBS;FLAGS;RC_DEPENDS"
        ${ARGN})
    set(objects "")
    if(A_RC)
        get_filename_component(rc_stem "${A_RC}" NAME_WE)
        set(res "${SXW_OBJ}/${rc_stem}.res")
        add_custom_command(OUTPUT "${res}"
            COMMAND "${SAVANXP_LLVM_RC}" /nologo /C 65001 /I "${A_RC_INCLUDE}" /FO "${res}" "${A_RC}"
            DEPENDS "${A_RC}" ${A_RC_DEPENDS}
            COMMENT "Compiling ${rc_stem}.rc (PE32 resources)"
            VERBATIM)
        list(APPEND objects "${res}")
    endif()
    foreach(source IN LISTS A_SOURCES)
        sxw_object(obj "${source}" ${A_FLAGS})
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
    if(A_SAFESEH)
        sxw_object(load_config "${SAVANXP_WIN32_ROOT}/dlls/common/safeseh.c")
        list(APPEND objects "${load_config}")
        list(APPEND flags /safeseh)
    endif()
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

# sxw_declared(<stem>): the declared entry points of dlls/<stem>
# (declared.txt, see dlls/declared.py). Sets SXW_<stem>_DECLARED to the
# sources to add to the DLL and SXW_<stem>_DEF to its full .def file.
function(sxw_declared stem)
    set(table "${SXW_DLLS}/${stem}/declared.txt")
    set(base "${SXW_DLLS}/${stem}/${stem}.def")
    set(gen "${SXW_OUT}/gen")
    file(MAKE_DIRECTORY "${gen}")
    add_custom_command(OUTPUT "${gen}/${stem}_declared.c" "${gen}/${stem}.def"
        COMMAND "${Python3_EXECUTABLE}" "${SXW_DLLS}/declared.py" "${stem}.dll" "${table}" "${base}"
                "${gen}/${stem}_declared.c" "${gen}/${stem}.def"
        DEPENDS "${SXW_DLLS}/declared.py" "${table}" "${base}"
        COMMENT "Generating ${stem}.dll's declared entry points"
        VERBATIM)
    set(SXW_${stem}_DECLARED "${gen}/${stem}_declared.c" "${SXW_DLLS}/common/declared.c" PARENT_SCOPE)
    set(SXW_${stem}_DEF "${gen}/${stem}.def" PARENT_SCOPE)
endfunction()
foreach(stem kernel32 msvcrt advapi32 gdi32 user32 shell32 ole32 comdlg32 shlwapi msftedit)
    sxw_declared(${stem})
endforeach()

# The DLL set. Bases are Windows XP's, so nothing of ours relocates.
sxw_image(NAME ntdll.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x7c900000 SAFESEH
    SOURCES "${SXW_DLLS}/ntdll/ntdll.c" "${SXW_DLLS}/ntdll/exception.c"
            "${SXW_DLLS}/common/memory.c"
    DEF "${SXW_DLLS}/ntdll/ntdll.def")
sxw_image(NAME kernel32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x7c800000
    SOURCES "${SXW_DLLS}/kernel32/kernel32.c" "${SXW_DLLS}/kernel32/memory.c" "${SXW_DLLS}/kernel32/system.c"
            "${SXW_DLLS}/kernel32/strings.c" "${SXW_DLLS}/kernel32/locale.c" "${SXW_DLLS}/kernel32/file.c" "${SXW_DLLS}/kernel32/atom.c"
            "${SXW_DLLS}/kernel32/thread.c"
            "${SXW_DLLS}/kernel32/time.c" "${SXW_DLLS}/common/codepage.c"
            "${SXW_DLLS}/common/resource.c"
            ${SXW_kernel32_DECLARED}
    DEF "${SXW_kernel32_DEF}" LIBS ntdll)
sxw_image(NAME msvcrt.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x77c10000 ENTRY DllMain SAFESEH
    SOURCES "${SXW_DLLS}/msvcrt/msvcrt.c" "${SXW_DLLS}/msvcrt/memory.c" "${SXW_DLLS}/msvcrt/wstring.c"
            "${SXW_DLLS}/msvcrt/math.c" "${SXW_DLLS}/msvcrt/wprintf.c" "${SXW_DLLS}/common/wformat.c"
            "${SXW_DLLS}/msvcrt/exception.c" "${SXW_DLLS}/msvcrt/cxxexception.c"
            "${SXW_DLLS}/common/codepage.c" ${SXW_msvcrt_DECLARED}
    DEF "${SXW_msvcrt_DEF}" LIBS kernel32 ntdll)
sxw_image(NAME advapi32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x77dd0000
    SOURCES "${SXW_DLLS}/advapi32/advapi32.c" "${SXW_DLLS}/common/codepage.c" ${SXW_advapi32_DECLARED}
    DEF "${SXW_advapi32_DEF}" LIBS ntdll)
sxw_image(NAME gdi32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x77f10000
    SOURCES "${SXW_DLLS}/gdi32/gdi32.c" "${SXW_DLLS}/gdi32/raster.c" "${SXW_DLLS}/gdi32/dib.c"
            "${SXW_DLLS}/common/region.c" "${SXW_DLLS}/common/codepage.c"
            "${SXW_DLLS}/common/memory.c" ${SXW_gdi32_DECLARED}
    DEF "${SXW_gdi32_DEF}" LIBS ntdll)
sxw_image(NAME user32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x7e410000
    SOURCES "${SXW_DLLS}/user32/user32.c" "${SXW_DLLS}/user32/menu.c" "${SXW_DLLS}/user32/dialog.c"
            "${SXW_DLLS}/user32/controls.c" "${SXW_DLLS}/user32/system.c" "${SXW_DLLS}/user32/hook.c"
            "${SXW_DLLS}/user32/winpos.c" "${SXW_DLLS}/user32/caret.c" "${SXW_DLLS}/user32/scroll.c"
            "${SXW_DLLS}/user32/edit.c" "${SXW_DLLS}/user32/listbox.c" "${SXW_DLLS}/user32/combo.c"
            "${SXW_DLLS}/user32/clipboard.c"
            "${SXW_DLLS}/common/region.c" "${SXW_DLLS}/common/memory.c"
            "${SXW_DLLS}/common/codepage.c" "${SXW_DLLS}/common/resource.c" ${SXW_user32_DECLARED}
    DEF "${SXW_user32_DEF}" LIBS gdi32 kernel32 ntdll)
sxw_image(NAME shell32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x7c9c0000
    SOURCES "${SXW_DLLS}/shell32/shell32.c" "${SXW_DLLS}/shell32/drop.c" ${SXW_shell32_DECLARED}
    DEF "${SXW_shell32_DEF}" LIBS user32 kernel32 ntdll)
sxw_image(NAME comctl32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x5d090000
    SOURCES "${SXW_DLLS}/comctl32/comctl32.c" DEF "${SXW_DLLS}/comctl32/comctl32.def")
# What WordPad's start-up needs from them, and the rest of its load-time
# imports declared (docs/roadmaps/open/WIN32_WORDPAD_ROADMAP.md, WP1).
sxw_image(NAME ole32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x774e0000
    SOURCES "${SXW_DLLS}/ole32/ole32.c" ${SXW_ole32_DECLARED} DEF "${SXW_ole32_DEF}" LIBS ntdll)
sxw_image(NAME comdlg32.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x763b0000
    SOURCES ${SXW_comdlg32_DECLARED} DEF "${SXW_comdlg32_DEF}" LIBS ntdll)
sxw_image(NAME shlwapi.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x77f60000
    SOURCES "${SXW_DLLS}/shlwapi/shlwapi.c" ${SXW_shlwapi_DECLARED} DEF "${SXW_shlwapi_DEF}" LIBS ntdll)
# WP7: the RichEdit control WordPad's document is (WIN32_WORDPAD_ROADMAP.md,
# "RichEdit is written here"), at Windows XP's base for msftedit.dll.
sxw_image(NAME msftedit.dll KIND dll DIR "${SXW_SYSTEM32}" BASE 0x4b460000 ENTRY DllMain
    SOURCES "${SXW_DLLS}/msftedit/msftedit.c" "${SXW_DLLS}/msftedit/text.c" "${SXW_DLLS}/msftedit/layout.c"
            "${SXW_DLLS}/msftedit/undo.c" ${SXW_msftedit_DECLARED}
    DEF "${SXW_msftedit_DEF}" LIBS user32 gdi32 kernel32 ntdll)

# Fixtures: hello.exe; initorder.exe with alpha.dll and beta.dll, which
# share a base; missingdll.exe and missingsym.exe, whose loads must fail.
sxw_image(NAME hello.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/hello/hello.c" LIBS kernel32)
sxw_image(NAME exception.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start SAFESEH
    SOURCES "${SXW_FX}/exception/exception.c" LIBS kernel32)
sxw_image(NAME sehfail.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY uncaught SAFESEH
    SOURCES "${SXW_FX}/exception/exception.c" LIBS kernel32)
sxw_image(NAME sehscope.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start SAFESEH
    SOURCES "${SXW_FX}/exception/sehscope.c"
    FLAGS -fms-extensions -fexceptions -fno-omit-frame-pointer LIBS kernel32 msvcrt)
sxw_image(NAME cxxthrow.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start SAFESEH
    SOURCES "${SXW_FX}/exception/cxxthrow.cpp"
    FLAGS -std=c++17 -fexceptions -fcxx-exceptions LIBS kernel32 msvcrt)
sxw_image(NAME cxxfail.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY uncaught SAFESEH
    SOURCES "${SXW_FX}/exception/cxxthrow.cpp"
    FLAGS -std=c++17 -fexceptions -fcxx-exceptions LIBS kernel32 msvcrt)
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
sxw_image(NAME cards.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/cards/cards.c" LIBS user32 gdi32 kernel32)
# resource.exe's images are written by a script, not kept in the tree.
set(SXW_RESOURCE_IMAGES "${SXW_OBJ}/resource-images")
set(SXW_RESOURCE_IMAGE_FILES "")
foreach(image core1.bmp info1.bmp core4.bmp info24.bmp icon.ico cursor.cur)
    list(APPEND SXW_RESOURCE_IMAGE_FILES "${SXW_RESOURCE_IMAGES}/${image}")
endforeach()
add_custom_command(OUTPUT ${SXW_RESOURCE_IMAGE_FILES}
    COMMAND "${Python3_EXECUTABLE}" "${SXW_FX}/resource/images.py" "${SXW_RESOURCE_IMAGES}"
    DEPENDS "${SXW_FX}/resource/images.py"
    COMMENT "Writing resource.exe's images"
    VERBATIM)
sxw_image(NAME resource.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/resource/resource.c" LIBS user32 gdi32 kernel32
    RC "${SXW_FX}/resource/resource.rc" RC_INCLUDE "${SXW_RESOURCE_IMAGES}" RC_DEPENDS ${SXW_RESOURCE_IMAGE_FILES})
sxw_image(NAME wstartup.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/wstartup/wstartup.c" LIBS msvcrt kernel32 user32 gdi32 advapi32 shlwapi ole32
    RC "${SXW_FX}/wstartup/wstartup.rc" RC_INCLUDE "${SXW_FX}/wstartup")
sxw_image(NAME dialog.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/dialog/dialog.c" LIBS user32 gdi32 kernel32 shell32 comctl32
    RC "${SXW_FX}/dialog/dialog.rc" RC_INCLUDE "${SXW_FX}/dialog")
# WP3 and WP4 (WIN32_WORDPAD_ROADMAP.md): files, memory and atoms, the
# locale and win.ini; threads with the two DLLs that note thread attaches.
sxw_image(NAME files.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/kernel/files.c" LIBS kernel32)
sxw_image(NAME memory.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/kernel/memory.c" LIBS kernel32)
sxw_image(NAME locale.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/kernel/locale.c" LIBS kernel32)
sxw_image(NAME tlog.dll KIND dll DIR "${SXW_FIXTURES}" BASE 0x10000000 ENTRY DllMain
    SOURCES "${SXW_FX}/threads/tlog.c" DEF "${SXW_FX}/threads/tlog.def" LIBS kernel32)
sxw_image(NAME tquiet.dll KIND dll DIR "${SXW_FIXTURES}" BASE 0x10100000 ENTRY DllMain
    SOURCES "${SXW_FX}/threads/tquiet.c" DEF "${SXW_FX}/threads/tquiet.def" LIBS tlog kernel32)
sxw_image(NAME threads.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/threads/threads.c" LIBS tlog tquiet user32 msvcrt kernel32)
# WP5 (WIN32_WORDPAD_ROADMAP.md): what MFC does to every window.
sxw_image(NAME hooks.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/hooks/hooks.c" LIBS user32 gdi32 kernel32)
sxw_image(NAME controls.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/controls/controls.c" LIBS user32 gdi32 kernel32)
sxw_image(NAME clip.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/clip/clip.c" LIBS user32 gdi32 shell32 kernel32)
# WP6: text and pictures in gdi32.
sxw_image(NAME text.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/text/text.c" LIBS user32 gdi32 kernel32)
# WP7: the RichEdit control.
sxw_image(NAME richedit.exe KIND exe DIR "${SXW_FIXTURES}" SUBSYSTEM console ENTRY start
    SOURCES "${SXW_FX}/richedit/richedit.c" LIBS user32 gdi32 kernel32)
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
# in C:\windows\system32, the Start menu's default pin to Solitaire in
# /system/start (copied into a new profile's Start Menu folder, and shown once
# the user puts sol.exe at its target), and with the
# test applications the fixtures in C:\tests, where win32test runs them.
# Never a Microsoft file.
set(SXW_DRIVE_C "system/win/drive_c")
set(SAVANXP_WIN32_COPY_COMMANDS
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/${SXW_DRIVE_C}/windows/system32"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SXW_SYSTEM32}/ntdll.dll" "${SXW_SYSTEM32}/kernel32.dll"
            "${SXW_SYSTEM32}/msvcrt.dll" "${SXW_SYSTEM32}/advapi32.dll"
            "${SXW_SYSTEM32}/gdi32.dll" "${SXW_SYSTEM32}/user32.dll"
            "${SXW_SYSTEM32}/shell32.dll" "${SXW_SYSTEM32}/comctl32.dll"
            "${SXW_SYSTEM32}/ole32.dll" "${SXW_SYSTEM32}/comdlg32.dll" "${SXW_SYSTEM32}/shlwapi.dll"
            "${SXW_SYSTEM32}/msftedit.dll"
            "${SAVANXP_DISKFS_ROOT}/${SXW_DRIVE_C}/windows/system32/"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/system/start"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SAVANXP_WIN32_ROOT}/start/Solitaire.sxlnk" "${SAVANXP_DISKFS_ROOT}/system/start/")
if(SAVANXP_INCLUDE_TEST_APPS)
    list(APPEND SAVANXP_WIN32_COPY_COMMANDS
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/${SXW_DRIVE_C}/tests"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SXW_FIXTURES}/hello.exe" "${SXW_FIXTURES}/initorder.exe"
                "${SXW_FIXTURES}/exception.exe" "${SXW_FIXTURES}/sehfail.exe"
                "${SXW_FIXTURES}/sehscope.exe" "${SXW_FIXTURES}/cxxthrow.exe" "${SXW_FIXTURES}/cxxfail.exe"
                "${SXW_FIXTURES}/alpha.dll" "${SXW_FIXTURES}/beta.dll"
                "${SXW_FIXTURES}/missingdll.exe" "${SXW_FIXTURES}/missingsym.exe"
                "${SXW_FIXTURES}/startup.exe" "${SXW_FIXTURES}/window.exe" "${SXW_FIXTURES}/cards.exe"
                "${SXW_FIXTURES}/resource.exe" "${SXW_FIXTURES}/dialog.exe" "${SXW_FIXTURES}/wstartup.exe"
                "${SXW_FIXTURES}/files.exe" "${SXW_FIXTURES}/memory.exe" "${SXW_FIXTURES}/locale.exe"
                "${SXW_FIXTURES}/threads.exe" "${SXW_FIXTURES}/tlog.dll" "${SXW_FIXTURES}/tquiet.dll"
                "${SXW_FIXTURES}/hooks.exe" "${SXW_FIXTURES}/controls.exe" "${SXW_FIXTURES}/clip.exe"
                "${SXW_FIXTURES}/text.exe" "${SXW_FIXTURES}/richedit.exe"
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
        "${SAVANXP_WIN32_ROOT}/loader/files.c"
        "${SAVANXP_WIN32_ROOT}/loader/threads.c"
        "${SAVANXP_WIN32_ROOT}/loader/clipboard.c"
        "${SAVANXP_WIN32_ROOT}/loader/fonts.c"
        "${SAVANXP_WIN32_ROOT}/loader/registry.c"
        "${SAVANXP_WIN32_ROOT}/pe/pe.c"
        "${CMAKE_SOURCE_DIR}/system/font/sxfont.c")
    target_include_directories(savanxp-win32-harness PRIVATE
        "${SAVANXP_WIN32_ROOT}/include" "${SAVANXP_WIN32_ROOT}/pe" "${SAVANXP_WIN32_ROOT}/loader")
    # sxfont/font.h and sxgfx/pixels.h, after the host's own headers: the
    # platform's directory also holds headers named as the C library's.
    target_compile_options(savanxp-win32-harness PRIVATE "SHELL:-idirafter ${CMAKE_SOURCE_DIR}/include")
    # The font services draw with libsxfont over the host's FreeType build,
    # the same sources winhost links (docs/desktop/FONT_LIBRARY.md).
    target_link_libraries(savanxp-win32-harness PRIVATE freetype_host)
    target_compile_definitions(savanxp-win32-harness PRIVATE
        "SXW_HARNESS_FONT_DIRS=\"${CMAKE_SOURCE_DIR}/assets/fonts/liberation\", \"${CMAKE_SOURCE_DIR}/assets/desktop/fonts\"")
    set_target_properties(savanxp-win32-harness PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
    target_compile_options(savanxp-win32-harness PRIVATE -Wall -Wextra -Werror)
    add_dependencies(savanxp-win32-harness savanxp_win32)
    add_dependencies(savanxp_tests savanxp-win32-harness)

    add_test(NAME win32-harness-test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/host/win32_harness_test.py"
                --harness "$<TARGET_FILE:savanxp-win32-harness>"
                --system32 "${SXW_SYSTEM32}" --fixtures "${SXW_FIXTURES}"
                --expected "${SXW_FX}/expected")
    # cards.exe with the user's own cards.dll; skipped without it.
    add_test(NAME win32-cards-test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/host/win32_harness_test.py"
                --harness "$<TARGET_FILE:savanxp-win32-harness>"
                --system32 "${SXW_SYSTEM32}" --fixtures "${SXW_FIXTURES}"
                --expected "${SXW_FX}/expected" --cards "${SAVANXP_WIN32_FIXTURES}")
    set_tests_properties(win32-cards-test PROPERTIES SKIP_RETURN_CODE 77)
    # The user's own sol.exe, played to the end twice; skipped without it.
    add_test(NAME win32-solitaire-test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/host/win32_harness_test.py"
                --harness "$<TARGET_FILE:savanxp-win32-harness>"
                --system32 "${SXW_SYSTEM32}" --fixtures "${SXW_FIXTURES}"
                --expected "${SXW_FX}/expected" --solitaire "${SAVANXP_WIN32_FIXTURES}")
    set_tests_properties(win32-solitaire-test PROPERTIES SKIP_RETURN_CODE 77)
    # The user's own WordPad and MFC, started up to their first window;
    # skipped without them.
    add_test(NAME win32-wordpad-test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/host/win32_harness_test.py"
                --harness "$<TARGET_FILE:savanxp-win32-harness>"
                --system32 "${SXW_SYSTEM32}" --fixtures "${SXW_FIXTURES}"
                --expected "${SXW_FX}/expected" --wordpad "${SAVANXP_WIN32_WORDPAD}")
    set_tests_properties(win32-wordpad-test PROPERTIES SKIP_RETURN_CODE 77)
    # text.exe with the user's own Windows XP fonts, against what XP printed;
    # skipped without them.
    add_test(NAME win32-text-xp-test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/host/win32_harness_test.py"
                --harness "$<TARGET_FILE:savanxp-win32-harness>"
                --system32 "${SXW_SYSTEM32}" --fixtures "${SXW_FIXTURES}"
                --expected "${SXW_FX}/expected" --text "${SAVANXP_WIN32_TEXT}")
    set_tests_properties(win32-text-xp-test PROPERTIES SKIP_RETURN_CODE 77)
endif()

# The shell's readers of a .exe's icon and version strings
# (include/sxgfx/pe_icon.h, pe_version.h), on the resource.exe fixture and
# 20,000 damaged copies of it.
add_executable(savanxp-pe-icon-test
    "${CMAKE_SOURCE_DIR}/system/gfx/pe_resource.c"
    "${CMAKE_SOURCE_DIR}/system/gfx/pe_icon.c"
    "${CMAKE_SOURCE_DIR}/system/gfx/pe_version.c"
    tests/host/pe_icon_test.c)
target_include_directories(savanxp-pe-icon-test PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(savanxp-pe-icon-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
target_compile_options(savanxp-pe-icon-test PRIVATE
    -Wall -Wextra -Werror -UNDEBUG -fsanitize=address,undefined -fno-sanitize-recover=all)
target_link_options(savanxp-pe-icon-test PRIVATE -fsanitize=address,undefined)
add_dependencies(savanxp-pe-icon-test savanxp_win32)
add_dependencies(savanxp_tests savanxp-pe-icon-test)
add_test(NAME pe-icon-test
    COMMAND savanxp-pe-icon-test "${SXW_FIXTURES}/resource.exe" "${SXW_FIXTURES}/hello.exe")
