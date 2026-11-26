# FreeType 2.15.0 (vendor/freetype, docs/desktop/FONT_LIBRARY.md), built
# unmodified, twice from the same sources:
#
#   freetype_sun    PIC, against the SDK's C headers, linked into
#                   libsxfont.so.0.4
#   freetype_host   for the host test and, later, the Win32 harness
#
# Only the modules libsxfont registers are compiled (system/font/freetype/
# ftmodule.h), each through upstream's single-file wrapper, and the
# configuration is SavanXP's copy of ftoption.h in that directory. Angle
# brackets matter: "ftoption.h" would find upstream's own next to ftheader.h.

set(SAVANXP_FREETYPE_ROOT "${CMAKE_SOURCE_DIR}/vendor/freetype")
set(SAVANXP_FREETYPE_SOURCES
    "${SAVANXP_FREETYPE_ROOT}/src/base/ftbase.c"
    "${SAVANXP_FREETYPE_ROOT}/src/base/ftbitmap.c"
    "${SAVANXP_FREETYPE_ROOT}/src/base/ftdebug.c"
    "${SAVANXP_FREETYPE_ROOT}/src/base/ftgasp.c"
    "${SAVANXP_FREETYPE_ROOT}/src/base/ftinit.c"
    "${SAVANXP_FREETYPE_ROOT}/src/base/ftsystem.c"
    "${SAVANXP_FREETYPE_ROOT}/src/sfnt/sfnt.c"
    "${SAVANXP_FREETYPE_ROOT}/src/truetype/truetype.c"
    "${SAVANXP_FREETYPE_ROOT}/src/smooth/smooth.c"
    "${SAVANXP_FREETYPE_ROOT}/src/raster/raster.c")
# What every file that includes a FreeType header needs, libsxfont's too: the
# public headers' layout follows the options.
set(SAVANXP_FREETYPE_INCLUDES
    "${CMAKE_SOURCE_DIR}/system/font/freetype"
    "${SAVANXP_FREETYPE_ROOT}/include")
set(SAVANXP_FREETYPE_DEFINES
    "FT_CONFIG_OPTIONS_H=<ftoption.h>"
    "FT_CONFIG_MODULES_H=<ftmodule.h>")

foreach(flavour IN ITEMS sun host)
    set(target freetype_${flavour})
    add_library(${target} STATIC ${SAVANXP_FREETYPE_SOURCES})
    target_include_directories(${target} PUBLIC ${SAVANXP_FREETYPE_INCLUDES})
    target_compile_definitions(${target} PUBLIC ${SAVANXP_FREETYPE_DEFINES})
    target_compile_definitions(${target} PRIVATE FT2_BUILD_LIBRARY)
    if(flavour STREQUAL "host")
        target_compile_options(${target} PRIVATE -O2 -w)
    else()
        # -w: upstream's code, not ours to warn about. Line tables only, as
        # for NetSurf: backtraces without full debug information on the disk.
        target_compile_options(${target} PRIVATE ${SAVANXP_LIBRARY_COMPILE_OPTIONS} -O2 -w
            -gline-tables-only)
        target_include_directories(${target} PRIVATE
            "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
    endif()
endforeach()

# The cmap validator recovers from a damaged table with setjmp. In-tree
# programs do not link the runtime's setjmp group (only the ports do), so
# libsxfont carries its own copy of the runtime's sx_setjmp and sx_longjmp,
# hidden with the rest of the archives it links.
add_library(freetype_sun_setjmp STATIC
    "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/runtime/setjmp.S")
target_compile_options(freetype_sun_setjmp PRIVATE "-target;${SAVANXP_TARGET_TRIPLE}" -fPIC)
target_link_libraries(freetype_sun PUBLIC freetype_sun_setjmp)

# What FreeType may ask the program for: C functions the runtime defines
# without a system call, plus sx_setjmp and sx_longjmp, which come from the
# archive above. A file, stdio or the environment would be a POSIX call
# inside a SUN program, and fails the build here.
set(SAVANXP_FREETYPE_ALLOWED_SYMBOLS
    calloc free malloc memchr memcmp memcpy memmove memset qsort realloc
    strcat strcmp strcpy strlen strncmp strncpy strrchr strstr strtol
    sx_longjmp sx_setjmp
    __stack_chk_fail __stack_chk_guard _GLOBAL_OFFSET_TABLE_)
string(REPLACE ";" "," freetype_allowed "${SAVANXP_FREETYPE_ALLOWED_SYMBOLS}")
set(SAVANXP_FREETYPE_SYMBOLS_STAMP "${CMAKE_BINARY_DIR}/freetype-symbols.stamp")
add_custom_command(
    OUTPUT "${SAVANXP_FREETYPE_SYMBOLS_STAMP}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_archive_symbols.py"
            --nm "${SAVANXP_NM}" --name FreeType
            --allow "${freetype_allowed}"
            --stamp "${SAVANXP_FREETYPE_SYMBOLS_STAMP}"
            "$<TARGET_FILE:freetype_sun>"
    DEPENDS freetype_sun "${CMAKE_SOURCE_DIR}/tools/check_archive_symbols.py"
    COMMENT "FreeType: what the SUN build asks the program for"
    VERBATIM)
add_custom_target(freetype_symbols ALL DEPENDS "${SAVANXP_FREETYPE_SYMBOLS_STAMP}")
