# The platform services (docs/desktop/DESKTOP_ARCHITECTURE.md#native-from-the-start):
# native processes that are not programs of the POSIX subsystem, so they live
# in system/ and not in subsystems/posix/userland. Each service is a directory
# system/<name>/; system/include holds the protocols only services speak.
#
# They still link the static user runtime: its call-free parts (string
# functions, snprintf, the gfx2d regions and painter) work unchanged in a
# native process, and any POSIX call is ENOSYS there. That is also why each
# one has to end in sxn_exit rather than return from main.
#
# SOURCES are the service's own, relative to system/<name>; SHARED_SOURCES are
# relative to the repository root, for a unit a service shares with programs
# elsewhere.
#
# TEST marks a diagnostic program, left out when SAVANXP_INCLUDE_TEST_APPS is
# off, like savanxp_program's TEST.
function(savanxp_system_service)
    set(options TEST)
    set(oneValueArgs NAME LINK_PROFILE)
    set(multiValueArgs SOURCES SHARED_SOURCES DEPENDS)
    cmake_parse_arguments(S "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(S_UNPARSED_ARGUMENTS OR NOT S_NAME OR NOT S_SOURCES)
        message(FATAL_ERROR "savanxp_system_service: TEST, NAME, LINK_PROFILE, SOURCES, SHARED_SOURCES and DEPENDS only")
    endif()
    if(S_TEST AND NOT SAVANXP_INCLUDE_TEST_APPS)
        return()
    endif()
    set(sources "")
    foreach(source IN LISTS S_SOURCES)
        list(APPEND sources "system/${S_NAME}/${source}")
    endforeach()
    set(profile "")
    if(S_LINK_PROFILE)
        set(profile LINK_PROFILE ${S_LINK_PROFILE})
    endif()
    set(depends "")
    if(S_DEPENDS)
        set(depends DEPENDS ${S_DEPENDS})
    endif()
    savanxp_program(NAME ${S_NAME} NATIVE ${profile} ${depends} SOURCES ${sources} ${S_SHARED_SOURCES})
    target_include_directories(${S_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/system/include"
        "${CMAKE_SOURCE_DIR}/system/${S_NAME}")
endfunction()

# The compositor (docs/desktop/DESKTOP_ARCHITECTURE.md#composition-in-compositord):
# composes the scene windowd commits and presents it.
# The SUN programs that paint with SXGUI map the toolkit rather than carry it:
# libsxgui and libgfx2d are neutral, and the host part -- sxngui.c, the SUN
# application frame -- is linked into each program (docs/kernel/
# SHARED_LIBRARIES.md, "The toolkit, split into neutral and host").
# libsxfont too: the frame loads the interface faces from /system/fonts
# (docs/desktop/UI_FONTS.md, "Interface faces from files").
set(SAVANXP_SUN_TOOLKIT libsxgui libgfx2d libsxfont)

savanxp_system_service(NAME compositord SOURCES
    compositord.c
    compositord_scene.c
    cursor_cur.c
    cursor_catalog.c
    shadow.c
    SHARED_SOURCES subsystems/posix/sdk/v1/runtime/sximg.c)

# The session manager: starts compositord, windowd, the desktop and Program
# Manager, and restarts them. init starts it.
savanxp_system_service(NAME sessiond SOURCES sessiond.c)

# The elevation broker (docs/kernel/USERS_AND_PERMISSIONS.md#elevation): runs
# a program elevated once windowd's prompt says yes. init starts it.
savanxp_system_service(NAME elevationd SOURCES elevationd.c)

# Task Manager (docs/userland/SYSTEM_MONITORING.md): an application like the
# console, and the first SUN program drawn with SXGUI (include/sxn/sxngui.h).
savanxp_system_service(NAME taskmgr LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} SOURCES taskmgr.c)

# The window manager. Its sources are written against the POSIX SDK;
# windowd_sys.h, included first by every unit, renames those calls onto SXN
# (windowd_sys.c). The desktop's icons are shared with the desktop and Program
# Manager, so they stay with them. PIE, mapping libgfx2d and libsxfont, so its
# captions come from the interface faces in /system/fonts (docs/desktop/
# UI_FONTS.md, "Interface faces from files").
savanxp_system_service(NAME windowd LINK_PROFILE PIE DEPENDS libgfx2d libsxfont SOURCES
    windowd.c
    windowd_sys.c
    windowd_compositor_client.c
    windowd_presentation.c
    windowd_layout.c
    windowd_render.c
    windowd_consent.c
    SHARED_SOURCES system/shell/desktop_icons.c)

# The desktop: background, taskbar, Start menu and the taskbar's popups, as
# windows of one process (docs/desktop/DESKTOP_ARCHITECTURE.md). windowd starts
# it as the shell. No .sxres, so it stays out of the launcher. It shares the
# shell's data (system/shell/) with Program Manager and Files through
# shell_io_sun.c, and the background's format with Display Properties.
savanxp_system_service(NAME desktop LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} libsximg SOURCES
    desktop.c
    desktop_sys.c
    desktop_background.c
    desktop_taskbar.c
    desktop_startmenu.c
    desktop_volume.c
    desktop_clock.c
    desktop_keyboard.c
    desktop_hung.c
    desktop_contextmenu.c
    desktop_confirm.c
    desktop_shutdown.c
    desktop_submenu.c
    desktop_view.c
    shell_io_sun.c
    SHARED_SOURCES
    system/gfx/wallpaper.c
    system/shell/desktop_icons.c
    system/shell/progman_registry.c
    system/shell/shortcut.c
    system/shell/start_shortcuts.c
    system/shell/start_pins.c
    system/shell/file_assoc.c
    system/gfx/pe_version.c
    system/gfx/pe_resource.c
    system/gfx/pe_icon.c)

# The Command Prompt (docs/desktop/CONSOLE.md): the console host, the first
# SUN application. Not a service, but a piece of the system written against
# the native API like them, so it lives here and links the same way.
savanxp_system_service(NAME console SOURCES console.c)

# sysinfo: the system's identity and the kernel's report on the machine, on
# standard output. A command, not a service, and one of the rescue system's
# tools (SAVANXP_RESCUE_PROGRAMS), staged there as sysinfo.sxe.
savanxp_system_service(NAME sysinfo SOURCES sysinfo.c)

# Nightwood (docs/userland/NIGHTWOOD.md): the forest game, the first game
# written against SUN and the consumer of SxGL's textures, fog and blending.
# Like the console it is an application, not a service, linked the same way;
# being a SUN program is what puts it in system/ rather than in the POSIX
# userland.
savanxp_system_service(NAME nightwood SOURCES nightwood.c nightwood_world.c nightwood_audio.c)

# sxgltest (docs/roadmaps/open/VIRGL_ROADMAP.md): SxGL's GPU backend drawn
# against its software rasterizer, pixel by pixel.
savanxp_system_service(NAME sxgltest TEST SOURCES sxgltest.c)

# Disk Management (docs/userland/DISK_MANAGEMENT.md): the consumer of the
# storage calls. libsxdisk and the SxFS core are shared with setup and the
# host tools.
savanxp_system_service(NAME diskmgmt LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} SOURCES diskmgmt.c
    SHARED_SOURCES libsxdisk/sxdisk.c libsxfs/sxfs_core.c)
target_include_directories(diskmgmt PRIVATE "${CMAKE_SOURCE_DIR}/libsxfs")

# Device Manager (docs/userland/DEVICE_MANAGER.md): the consumer of the
# hardware inventory, SXN_SYS_DEVICE_SNAPSHOT.
savanxp_system_service(NAME devmgmt LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} SOURCES devmgmt.c)

# Sound control panel: SUN consumer of audiod's native control port.
savanxp_system_service(NAME sound LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} SOURCES sound.c)

# SxGL Diagnostic Tool (docs/desktop/SXGL_DIAG.md): the machine, the display
# and both SxGL backends, with a 3D test of each.
savanxp_system_service(NAME sxgldiag LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} SOURCES sxgldiag.c)

# Log Viewer (docs/userland/LOG_VIEWER.md): the system log on screen, the
# consumer of SXN_SYS_LOG_READ.
savanxp_system_service(NAME logview LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} SOURCES logview.c)

# Display Properties (docs/desktop/DISPLAY_PROPERTIES.md): the wallpaper and
# the resolution. It shares desktop.cfg's format, the decoding and the painting
# with the POSIX desktop through system/gfx/wallpaper.c.
savanxp_system_service(NAME display LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} libsximg SOURCES display.c
    SHARED_SOURCES system/gfx/wallpaper.c)

# Mouse Properties: native cursor scheme selection and previews.
savanxp_system_service(NAME mouse LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} SOURCES mouse.c
    SHARED_SOURCES system/compositord/cursor_catalog.c system/compositord/cursor_cur.c system/compositord/shadow.c
        subsystems/posix/sdk/v1/runtime/sximg.c)
target_include_directories(mouse PRIVATE "${CMAKE_SOURCE_DIR}/system/compositord")

# The Win32 subsystem's host (docs/userland/WIN32.md#winhost): a SUN program
# that loads a PE32 executable and its DLLs below 4 GiB and runs it as 32-bit
# code. Its sources live with the subsystem in subsystems/win32/. It stays a
# static program at 0x400000, below where sol.exe must load, so libsxfont is
# linked into it whole (system/font/sxfont.c over freetype_sun) rather than
# mapped; gdi32's fonts come from it (loader/fonts.c).
savanxp_program(NAME winhost NATIVE SOURCES
    subsystems/win32/host/winhost.c
    subsystems/win32/loader/loader.c
    subsystems/win32/loader/services.c
    subsystems/win32/loader/files.c
    subsystems/win32/loader/threads.c
    subsystems/win32/loader/clipboard.c
    subsystems/win32/loader/fonts.c
    subsystems/win32/loader/registry.c
    subsystems/win32/pe/pe.c
    system/font/sxfont.c)
target_include_directories(winhost PRIVATE
    "${CMAKE_SOURCE_DIR}/subsystems/win32/include"
    "${CMAKE_SOURCE_DIR}/subsystems/win32/loader"
    "${CMAKE_SOURCE_DIR}/subsystems/win32/pe")
target_link_libraries(winhost PRIVATE freetype_sun)

# SSL's launcher (docs/userland/LINUX.md#the-launcher): `linux GUEST_PATH`
# starts a Linux program from the guest tree under /system/linux.
savanxp_program(NAME linux NATIVE SOURCES subsystems/linux/launcher/linux.c)
# The X11 server is a SUN program; its portable engine is shared by host tests.
savanxp_program(NAME x11d NATIVE SOURCES subsystems/linux/x11/x11d.c subsystems/linux/x11/x11.c)

# The linux-hello smoke scenario's runner: a SUN parent that runs the L1
# fixtures through the launcher and compares their output with Linux's.
savanxp_program(NAME linuxtest TEST NATIVE SOURCES subsystems/linux/tests/linuxtest.c)

# The win32-hello smoke scenario's runner: a SUN parent that runs the W1
# fixtures through winhost and compares their output with Windows XP's.
savanxp_program(NAME win32test TEST NATIVE SOURCES subsystems/win32/tests/win32test.c)

# Web Viewer parses HTML through libdom and hubbub and paints with SXGUI.
# It opens https:// through sxnhttp's TLS (system/runtime/sxnhttp_tls.c) and
# the OpenSSL port, so it is built only when the port is (ports/openssl); the
# registry drops it otherwise.
if(SAVANXP_HAVE_OPENSSL)
    savanxp_system_service(NAME webview LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} libssl libcrypto netsurf_css netsurf_dom
        SOURCES webview.c download.c favorites.c engine/webengine_html.c engine/webengine.c engine/webengine_select.c
                engine/webengine_box.c engine/webengine_layout.c engine/webengine_lines.c
                engine/webengine_paint.c engine/webengine_table.c
        SHARED_SOURCES system/runtime/sxnhttp_tls.c)
    add_dependencies(webview netsurf_symbols)
endif()

# Native image viewer over the common memory-only decoder.
savanxp_system_service(NAME imgview LINK_PROFILE PIE DEPENDS ${SAVANXP_SUN_TOOLKIT} libsximg
    SOURCES imgview.c view.c)

# SxBASIC (docs/userland/SXBASIC.md). libsxbasic carries the Savan BASIC core
# -- compiler, virtual machine, program image and Make SXE's writer, call-free
# and also built for the host test -- and the forms runtime over SXGUI. Like
# libsximg it asks the program for its C library (snprintf, strtod, the math
# functions), which lld exports from the program that links it. sxbasic.sxe
# is the runner and the stub every made executable is a copy of
# (docs/userland/SXBASIC_IMAGE.md).
savanxp_library(NAME libsxbasic SONAME libsxbasic.so.0.4 DEPENDS libsxgui libgfx2d SOURCES
    system/sxbasic/forms.c
    system/sxbasic/core/sxb_forms_def.c
    system/sxbasic/core/sxb_lex.c
    system/sxbasic/core/sxb_parse.c
    system/sxbasic/core/sxb_compile.c
    system/sxbasic/core/sxb_value.c
    system/sxbasic/core/sxb_vm.c
    system/sxbasic/core/sxb_builtins.c
    system/sxbasic/core/sxb_image.c
    system/sxbasic/core/sxb_exe.c)
savanxp_system_service(NAME sxbasic LINK_PROFILE PIE DEPENDS libsxbasic ${SAVANXP_SUN_TOOLKIT} libsximg
    SOURCES sxbasic.c)
# Savan BASIC, the development environment (docs/userland/SXBASIC_IDE.md). It
# runs and makes projects through sxbasic.sxe in a child process; from
# libsxbasic it takes the controls' tables, which its form designer follows.
savanxp_system_service(NAME sxbide LINK_PROFILE PIE DEPENDS libsxbasic ${SAVANXP_SUN_TOOLKIT} SOURCES sxbide.c)
target_include_directories(sxbide PRIVATE "${CMAKE_SOURCE_DIR}/system/sxbasic")
