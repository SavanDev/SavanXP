# Every in-tree program is registered explicitly. Keeping this list in CMake
# makes the target graph the single source of truth for the userland.
function(savanxp_program)
    set(options TEST)
    set(oneValueArgs NAME)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(P "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "savanxp_program(${P_NAME}): unknown arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()
    if(P_TEST AND NOT SAVANXP_INCLUDE_TEST_APPS)
        return()
    endif()
    if(NOT P_NAME OR NOT P_SOURCES)
        message(FATAL_ERROR "savanxp_program requires NAME and SOURCES")
    endif()
    add_executable(${P_NAME} ${P_SOURCES})
    target_sources(${P_NAME} PRIVATE ${SAVANXP_GENERATED_HEADER_FILES})
    target_include_directories(${P_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
        "${SAVANXP_GENERATED_ROOT}"
    )
    target_compile_definitions(${P_NAME} PRIVATE DESKTOP_INCLUDE_TEST_APPS=$<BOOL:${SAVANXP_INCLUDE_TEST_APPS}>)
    target_compile_options(${P_NAME} PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
    target_link_libraries(${P_NAME} PRIVATE savanxp_user_runtime)
    target_link_options(${P_NAME} PRIVATE ${SAVANXP_USER_LINK_OPTIONS})
    set_target_properties(${P_NAME} PROPERTIES OUTPUT_NAME "${P_NAME}" SUFFIX "")
    set_property(GLOBAL APPEND PROPERTY SAVANXP_USER_TARGETS ${P_NAME})
    set_property(GLOBAL APPEND PROPERTY SAVANXP_USER_PROGRAM_NAMES ${P_NAME})
endfunction()

savanxp_program(NAME init SOURCES subsystems/posix/userland/init.c)
savanxp_program(NAME sh SOURCES
    subsystems/posix/userland/sh.c
    subsystems/posix/userland/shell_core.c)
savanxp_program(NAME shellapp SOURCES
    subsystems/posix/userland/shellapp.c
    subsystems/posix/userland/shell_core.c
    subsystems/posix/userland/shellapp_stats.c)
savanxp_program(NAME uname SOURCES subsystems/posix/userland/uname.c)
savanxp_program(NAME df SOURCES subsystems/posix/userland/df.c)
savanxp_program(NAME ticker TEST SOURCES subsystems/posix/userland/ticker.c)
savanxp_program(NAME demo TEST SOURCES subsystems/posix/userland/demo.c)
savanxp_program(NAME fdtest TEST SOURCES subsystems/posix/userland/fdtest.c)
savanxp_program(NAME waittest TEST SOURCES subsystems/posix/userland/waittest.c)
savanxp_program(NAME pipestress TEST SOURCES subsystems/posix/userland/pipestress.c)
savanxp_program(NAME spawnloop TEST SOURCES subsystems/posix/userland/spawnloop.c)
savanxp_program(NAME badptr TEST SOURCES subsystems/posix/userland/badptr.c)
savanxp_program(NAME rmdir SOURCES subsystems/posix/userland/rmdir.c)
savanxp_program(NAME truncate SOURCES subsystems/posix/userland/truncate.c)
savanxp_program(NAME sync SOURCES subsystems/posix/userland/sync.c)
savanxp_program(NAME seektest TEST SOURCES subsystems/posix/userland/seektest.c)
savanxp_program(NAME renametest TEST SOURCES subsystems/posix/userland/renametest.c)
savanxp_program(NAME truncatetest TEST SOURCES subsystems/posix/userland/truncatetest.c)
savanxp_program(NAME errtest TEST SOURCES subsystems/posix/userland/errtest.c)
savanxp_program(NAME netinfo SOURCES subsystems/posix/userland/netinfo.c)
savanxp_program(NAME ping SOURCES subsystems/posix/userland/ping.c)
savanxp_program(NAME udpsend SOURCES subsystems/posix/userland/udpsend.c)
savanxp_program(NAME udprecv SOURCES subsystems/posix/userland/udprecv.c)
savanxp_program(NAME udptest TEST SOURCES subsystems/posix/userland/udptest.c)
savanxp_program(NAME nettest TEST SOURCES subsystems/posix/userland/nettest.c)
savanxp_program(NAME tcpget SOURCES subsystems/posix/userland/tcpget.c)
savanxp_program(NAME tcptest TEST SOURCES subsystems/posix/userland/tcptest.c)
savanxp_program(NAME beep SOURCES subsystems/posix/userland/beep.c)
savanxp_program(NAME audiotest TEST SOURCES subsystems/posix/userland/audiotest.c)
savanxp_program(NAME compositord SOURCES subsystems/posix/userland/compositord.c)
savanxp_program(NAME windowd SOURCES
    subsystems/posix/sdk/v1/runtime/sxe.c
    subsystems/posix/userland/windowd.c
    subsystems/posix/userland/windowd_compositor_client.c
    subsystems/posix/userland/desktop_icons.c
    subsystems/posix/userland/windowd_appinfo.c
    subsystems/posix/userland/desktop_wallpaper.c
    subsystems/posix/userland/windowd_layout.c
    subsystems/posix/userland/windowd_render.c
    subsystems/posix/userland/windowd_stats.c)
savanxp_program(NAME shellui SOURCES
    subsystems/posix/userland/shellui.c
    subsystems/posix/userland/desktop_wallpaper.c)
savanxp_program(NAME taskbar SOURCES
    subsystems/posix/userland/taskbar.c
    subsystems/posix/userland/desktop_icons.c)
savanxp_program(NAME kbdlayoutpopup SOURCES subsystems/posix/userland/kbdlayoutpopup.c)
savanxp_program(NAME progman SOURCES
    subsystems/posix/sdk/v1/runtime/sxe.c
    subsystems/posix/userland/progman.c
    subsystems/posix/userland/progman_registry.c
    subsystems/posix/userland/desktop_icons.c
    subsystems/posix/userland/desktop_wallpaper.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME appwiz SOURCES
    subsystems/posix/sdk/v1/runtime/sxe.c
    subsystems/posix/userland/appwiz.c
    subsystems/posix/userland/appwiz_catalog.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME aboutapp SOURCES
    subsystems/posix/userland/aboutapp.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME taskmgr SOURCES
    subsystems/posix/userland/taskmgr.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME filesapp SOURCES
    subsystems/posix/userland/filesapp.c
    subsystems/posix/userland/file_assoc.c
    subsystems/posix/userland/mime_icon.c
    subsystems/posix/sdk/v1/runtime/sxe.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME notepad SOURCES
    subsystems/posix/userland/notepad.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME mines SOURCES
    subsystems/posix/userland/mines.c
    subsystems/posix/userland/mines_board.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME calc SOURCES
    subsystems/posix/userland/calc.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME mediaplayer SOURCES
    subsystems/posix/userland/mediaplayer.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME widgetsdemo TEST SOURCES
    subsystems/posix/userland/widgetsdemo.c
    subsystems/posix/sdk/v1/runtime/sxgui.c
    subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_program(NAME gfxdemo TEST SOURCES subsystems/posix/userland/gfxdemo.c)
savanxp_program(NAME gears TEST SOURCES subsystems/posix/userland/gears.c)
savanxp_program(NAME gputest TEST SOURCES subsystems/posix/userland/gputest.c)
savanxp_program(NAME keytest TEST SOURCES subsystems/posix/userland/keytest.c)
savanxp_program(NAME kbdtest TEST SOURCES subsystems/posix/userland/kbdtest.c)
savanxp_program(NAME mousetest TEST SOURCES subsystems/posix/userland/mousetest.c)
savanxp_program(NAME sysinfo SOURCES subsystems/posix/userland/sysinfo.c)
savanxp_program(NAME forktest TEST SOURCES subsystems/posix/userland/forktest.c)
savanxp_program(NAME smptest TEST SOURCES subsystems/posix/userland/smptest.c)
savanxp_program(NAME polltest TEST SOURCES subsystems/posix/userland/polltest.c)
savanxp_program(NAME clocktest TEST SOURCES subsystems/posix/userland/clocktest.c)
savanxp_program(NAME sigtest TEST SOURCES subsystems/posix/userland/sigtest.c)
savanxp_program(NAME eventtest TEST SOURCES subsystems/posix/userland/eventtest.c)
savanxp_program(NAME timertest TEST SOURCES subsystems/posix/userland/timertest.c)
savanxp_program(NAME sectiontest TEST SOURCES subsystems/posix/userland/sectiontest.c)
savanxp_program(NAME handletest TEST SOURCES subsystems/posix/userland/handletest.c)
savanxp_program(NAME semaphoretest TEST SOURCES subsystems/posix/userland/semaphoretest.c)
savanxp_program(NAME cliptest TEST SOURCES subsystems/posix/userland/cliptest.c)
savanxp_program(NAME seltest TEST SOURCES
    subsystems/posix/userland/seltest.c
    subsystems/posix/sdk/v1/runtime/sxgui.c)
savanxp_program(NAME mmaptest TEST SOURCES subsystems/posix/userland/mmaptest.c)
savanxp_program(NAME libctest TEST SOURCES subsystems/posix/userland/libctest.c)
savanxp_program(NAME stacktest TEST SOURCES subsystems/posix/userland/stacktest.c)
savanxp_program(NAME imagetest TEST SOURCES subsystems/posix/userland/imagetest.c)
savanxp_program(NAME heaptest TEST SOURCES subsystems/posix/userland/heaptest.c)
savanxp_program(NAME smoke TEST SOURCES subsystems/posix/userland/smoke.c)
savanxp_program(NAME sxetest TEST SOURCES
    subsystems/posix/userland/sxetest.c
    subsystems/posix/sdk/v1/runtime/sxe.c)
