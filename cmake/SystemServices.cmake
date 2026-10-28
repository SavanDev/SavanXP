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
function(savanxp_system_service)
    set(oneValueArgs NAME)
    set(multiValueArgs SOURCES SHARED_SOURCES)
    cmake_parse_arguments(S "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(S_UNPARSED_ARGUMENTS OR NOT S_NAME OR NOT S_SOURCES)
        message(FATAL_ERROR "savanxp_system_service: NAME, SOURCES and SHARED_SOURCES only")
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
    compositord_scene.c)

# The session manager: starts compositord, windowd, the desktop and Program
# Manager, and restarts them. init starts it.
savanxp_system_service(NAME sessiond SOURCES sessiond.c)

# The elevation broker (docs/kernel/USERS_AND_PERMISSIONS.md#elevation): runs
# a program elevated once windowd's prompt says yes. init starts it.
savanxp_system_service(NAME elevationd SOURCES elevationd.c)

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
