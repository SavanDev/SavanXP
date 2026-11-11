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
    set(oneValueArgs NAME)
    set(multiValueArgs SOURCES SHARED_SOURCES)
    cmake_parse_arguments(S "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(S_UNPARSED_ARGUMENTS OR NOT S_NAME OR NOT S_SOURCES)
        message(FATAL_ERROR "savanxp_system_service: TEST, NAME, SOURCES and SHARED_SOURCES only")
    endif()
    if(S_TEST AND NOT SAVANXP_INCLUDE_TEST_APPS)
        return()
    endif()
    set(sources "")
    foreach(source IN LISTS S_SOURCES)
        list(APPEND sources "system/${S_NAME}/${source}")
    endforeach()
    savanxp_program(NAME ${S_NAME} NATIVE SOURCES ${sources} ${S_SHARED_SOURCES})
    target_include_directories(${S_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/system/include"
        "${CMAKE_SOURCE_DIR}/system/${S_NAME}")
endfunction()

# The compositor (docs/desktop/DESKTOP_ARCHITECTURE.md#composition-in-compositord):
# composes the scene windowd commits and presents it.
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
savanxp_system_service(NAME taskmgr SOURCES taskmgr.c)

# The window manager. Its sources are written against the POSIX SDK;
# windowd_sys.h, included first by every unit, renames those calls onto SXN
# (windowd_sys.c). The desktop's icons are shared with the desktop and Program
# Manager, so they stay with them.
savanxp_system_service(NAME windowd SOURCES
    windowd.c
    windowd_sys.c
    windowd_compositor_client.c
    windowd_presentation.c
    windowd_layout.c
    windowd_render.c
    windowd_consent.c
    SHARED_SOURCES subsystems/posix/userland/desktop_icons.c)

# The Command Prompt (docs/desktop/CONSOLE.md): the console host, the first
# SUN application. Not a service, but a piece of the system written against
# the native API like them, so it lives here and links the same way.
savanxp_system_service(NAME console SOURCES console.c)

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
savanxp_system_service(NAME diskmgmt SOURCES diskmgmt.c
    SHARED_SOURCES libsxdisk/sxdisk.c libsxfs/sxfs_core.c)
target_include_directories(diskmgmt PRIVATE "${CMAKE_SOURCE_DIR}/libsxfs")

# Sound control panel: SUN consumer of audiod's native control port.
savanxp_system_service(NAME sound SOURCES sound.c)

# Log Viewer (docs/userland/LOG_VIEWER.md): the system log on screen, the
# consumer of SXN_SYS_LOG_READ.
savanxp_system_service(NAME logview SOURCES logview.c)

# Mouse Properties: native cursor scheme selection and previews.
savanxp_system_service(NAME mouse SOURCES mouse.c
    SHARED_SOURCES system/compositord/cursor_catalog.c system/compositord/cursor_cur.c system/compositord/shadow.c
        subsystems/posix/sdk/v1/runtime/sximg.c)
target_include_directories(mouse PRIVATE "${CMAKE_SOURCE_DIR}/system/compositord")
