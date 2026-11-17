# v0.4.0 graphics units retain their shared-library ownership on ELF32.
savanxp_i386_library(libsxgfx subsystems/posix/sdk/v1/runtime/gfx.c)
savanxp_i386_library(libgfx2d subsystems/posix/sdk/v1/runtime/gfx2d.c subsystems/posix/sdk/v1/runtime/sxchrome.c)
target_link_libraries(savanxp-i386-libgfx2d PRIVATE savanxp-i386-libsxgfx)
savanxp_i386_library(libsxgui subsystems/posix/sdk/v1/runtime/sxgui.c subsystems/posix/sdk/v1/runtime/sxgui_app.c)
target_link_libraries(savanxp-i386-libsxgui PRIVATE savanxp-i386-libgfx2d savanxp-i386-libsxgfx)

savanxp_i386_pie(shellapp DEPENDS libgfx2d libsxgfx SOURCES
    subsystems/posix/userland/shellapp.c
    subsystems/posix/userland/shell_core.c
    subsystems/posix/userland/shellapp_stats.c)
savanxp_i386_pie(windowd DEPENDS libgfx2d libsxgfx  SOURCES
    subsystems/posix/userland/windowd.c
    subsystems/posix/userland/windowd_compositor_client.c
    subsystems/posix/userland/desktop_icons.c
    subsystems/posix/userland/windowd_appinfo.c
    subsystems/posix/userland/desktop_wallpaper.c
    subsystems/posix/userland/windowd_layout.c
    subsystems/posix/userland/windowd_render.c
    subsystems/posix/userland/windowd_stats.c)
savanxp_i386_pie(shellui DEPENDS libgfx2d libsxgfx  SOURCES
    subsystems/posix/userland/shellui.c
    subsystems/posix/userland/desktop_wallpaper.c)
savanxp_i386_pie(taskbar DEPENDS libgfx2d libsxgfx  SOURCES
    subsystems/posix/userland/taskbar.c
    subsystems/posix/userland/desktop_icons.c)
savanxp_i386_pie(kbdlayoutpopup DEPENDS libgfx2d libsxgfx  SOURCES subsystems/posix/userland/kbdlayoutpopup.c)
savanxp_i386_pie(volumepopup DEPENDS libgfx2d libsxgfx  SOURCES subsystems/posix/userland/volumepopup.c)
savanxp_i386_pie(startmenu DEPENDS libgfx2d libsxgfx  SOURCES
    subsystems/posix/userland/startmenu.c
    subsystems/posix/userland/progman_registry.c
    subsystems/posix/userland/desktop_icons.c)
savanxp_i386_pie(progman DEPENDS libgfx2d libsxgui libsxgfx  SOURCES
    subsystems/posix/userland/progman.c
    subsystems/posix/userland/progman_registry.c
    subsystems/posix/userland/desktop_icons.c
    subsystems/posix/userland/desktop_wallpaper.c)
savanxp_i386_pie(appwiz DEPENDS libgfx2d libsxgui libsxgfx  SOURCES
    subsystems/posix/userland/appwiz.c
    subsystems/posix/userland/appwiz_catalog.c)
savanxp_i386_pie(aboutapp DEPENDS libgfx2d libsxgui libsxgfx  SOURCES
    subsystems/posix/userland/aboutapp.c)
savanxp_i386_pie(taskmgr DEPENDS libgfx2d libsxgui libsxgfx  SOURCES
    subsystems/posix/userland/taskmgr.c)
savanxp_i386_pie(filesapp DEPENDS libgfx2d libsxgui libsxgfx  SOURCES
    subsystems/posix/userland/filesapp.c
    subsystems/posix/userland/file_assoc.c
    subsystems/posix/userland/mime_icon.c)
savanxp_i386_pie(notepad DEPENDS libgfx2d libsxgui libsxgfx  SOURCES
    subsystems/posix/userland/notepad.c)
savanxp_i386_pie(mines DEPENDS libgfx2d libsxgui libsxgfx  SOURCES
    subsystems/posix/userland/mines.c
    subsystems/posix/userland/mines_board.c)
savanxp_i386_pie(calc DEPENDS libgfx2d libmath libsxgui libsxgfx
    SOURCES
    subsystems/posix/userland/calc.c)
# The pinned FFmpeg port is optional until its external engine has been built.
set(SAVANXP_FFMPEG_ROOT "${SAVANXP_OUTPUT_ROOT}/ports/ffmpeg/work" CACHE PATH "FFmpeg port work directory")
set(SAVANXP_FFMPEG_LIBRARY "${SAVANXP_OUTPUT_ROOT}/external/libffmpeg.so.0.4" CACHE FILEPATH "ELF32 FFmpeg engine")
file(GLOB ffmpeg_sources "${SAVANXP_FFMPEG_ROOT}/ffmpeg-*/libavformat/avformat.h")
if(EXISTS "${SAVANXP_FFMPEG_LIBRARY}" AND ffmpeg_sources)
    file(READ "${SAVANXP_FFMPEG_LIBRARY}" ffmpeg_header LIMIT 20 HEX)
    string(SUBSTRING "${ffmpeg_header}" 0 10 ffmpeg_class)
    string(SUBSTRING "${ffmpeg_header}" 36 4 ffmpeg_machine)
    if(ffmpeg_class STREQUAL "7f454c4601" AND ffmpeg_machine STREQUAL "0300")
        list(GET ffmpeg_sources 0 ffmpeg_header_path)
        get_filename_component(ffmpeg_include "${ffmpeg_header_path}" DIRECTORY)
        get_filename_component(ffmpeg_include "${ffmpeg_include}" DIRECTORY)
        add_library(savanxp-i386-libffmpeg SHARED IMPORTED)
        set_target_properties(savanxp-i386-libffmpeg PROPERTIES IMPORTED_LOCATION "${SAVANXP_FFMPEG_LIBRARY}")
        savanxp_i386_pie(mediaplayer DEPENDS libgfx2d libffmpeg libmath libsxgui libsxgfx SOURCES
            subsystems/posix/userland/mediaplayer/media.c
            subsystems/posix/userland/mediaplayer/mediaplayer.c
            subsystems/posix/userland/mediaplayer/playback.c
            subsystems/posix/userland/mediaplayer/selftest.c)
        target_include_directories(savanxp-i386-mediaplayer PRIVATE "${ffmpeg_include}" "${SAVANXP_FFMPEG_ROOT}/build")
        add_custom_command(TARGET savanxp_i386_diskfs POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy "${SAVANXP_FFMPEG_LIBRARY}" "${SAVANXP_DISKFS_ROOT}/lib/libffmpeg.so.0.4"
            VERBATIM)
        list(APPEND SAVANXP_I386_LIBRARIES libffmpeg.so.0.4)
    else()
        message(STATUS "Retaining foreign FFmpeg engine as data; rebuild ports/ffmpeg for i386")
    endif()
endif()

if(SAVANXP_INCLUDE_TEST_APPS)
savanxp_i386_pie(widgetsdemo DEPENDS libgfx2d libsxgui libsxgfx   SOURCES
    subsystems/posix/userland/widgetsdemo.c)
endif()
if(SAVANXP_INCLUDE_TEST_APPS)
savanxp_i386_pie(gfxdemo DEPENDS libsxgfx   SOURCES
    subsystems/posix/userland/gfxdemo.c)
endif()
if(SAVANXP_INCLUDE_TEST_APPS)
savanxp_i386_pie(gears DEPENDS libsxgfx   SOURCES
    subsystems/posix/userland/gears.c)
endif()
if(SAVANXP_INCLUDE_TEST_APPS)
savanxp_i386_pie(gputest DEPENDS libsxgfx   SOURCES
    subsystems/posix/userland/gputest.c)
endif()
if(SAVANXP_INCLUDE_TEST_APPS)
savanxp_i386_pie(keytest DEPENDS libsxgfx   SOURCES
    subsystems/posix/userland/keytest.c)
endif()
if(SAVANXP_INCLUDE_TEST_APPS)
savanxp_i386_pie(mousetest DEPENDS libsxgfx   SOURCES
    subsystems/posix/userland/mousetest.c)
endif()
if(SAVANXP_INCLUDE_TEST_APPS)
savanxp_i386_pie(seltest DEPENDS libgfx2d libsxgui libsxgfx   SOURCES
    subsystems/posix/userland/seltest.c)
endif()

foreach(name IN ITEMS init compositord audiod)
    add_executable(savanxp-i386-${name} subsystems/posix/userland/${name}.c)
    target_include_directories(savanxp-i386-${name} PRIVATE "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland" "${SAVANXP_GENERATED_ROOT}")
    target_compile_options(savanxp-i386-${name} PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS})
    target_link_options(savanxp-i386-${name} PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
    target_link_libraries(savanxp-i386-${name} PRIVATE savanxp_i386_user_runtime)
    add_dependencies(savanxp-i386-${name} savanxp_generated_headers)
    set_target_properties(savanxp-i386-${name} PROPERTIES OUTPUT_NAME ${name} SUFFIX "")
    add_dependencies(savanxp_i386_userland savanxp-i386-${name})
    add_custom_command(TARGET savanxp_i386_diskfs POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:savanxp-i386-${name}>" "${SAVANXP_DISKFS_ROOT}/bin/${name}"
        VERBATIM)
    list(APPEND SAVANXP_I386_DYNAMIC_PROGRAMS ${name})
endforeach()

# Preserve the reference desktop names/icons and metadata in native ELF32 apps.
set(resource_args)
foreach(name IN LISTS SAVANXP_I386_DYNAMIC_PROGRAMS)
    list(APPEND resource_args --program "${name}")
endforeach()
set(resource_dir "${SAVANXP_GENERATED_ROOT}/sxe")
set(resource_stamp "${SAVANXP_GENERATED_ROOT}/i386-sxe.stamp")
set(resource_config "${CMAKE_BINARY_DIR}/i386-sxe-programs.txt")
set(resource_content "${SAVANXP_BUILD_ID}\n${SAVANXP_I386_DYNAMIC_PROGRAMS}\n")
set(old_resource_content "")
if(EXISTS "${resource_config}")
    file(READ "${resource_config}" old_resource_content)
endif()
if(NOT old_resource_content STREQUAL resource_content)
    file(WRITE "${resource_config}" "${resource_content}")
endif()
file(GLOB_RECURSE resource_manifests CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/subsystems/posix/userland/*.sxres")
file(GLOB_RECURSE resource_assets CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/desktop/icons/*.png")
add_custom_command(OUTPUT "${resource_stamp}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_sxe_resources.py"
        --project-root "${CMAKE_SOURCE_DIR}"
        --manifest-dir "${CMAKE_SOURCE_DIR}/subsystems/posix/userland"
        --build-id "${SAVANXP_BUILD_ID}" --output-dir "${resource_dir}" ${resource_args}
    COMMAND "${CMAKE_COMMAND}" -E touch "${resource_stamp}"
    DEPENDS "${resource_config}" ${resource_manifests} ${resource_assets}
        "${CMAKE_SOURCE_DIR}/tools/gen_sxe_resources.py"
        "${CMAKE_SOURCE_DIR}/tools/stamp_sxe.py"
        "${CMAKE_SOURCE_DIR}/include/sxe/sxe_format.h"
        "${CMAKE_SOURCE_DIR}/include/abi/savanxp_native_abi.h"
        "${CMAKE_SOURCE_DIR}/include/shared/version.h"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include/savanxp/syscall.h"
    VERBATIM)
add_custom_target(savanxp_i386_resources DEPENDS "${resource_stamp}")
foreach(name IN LISTS SAVANXP_I386_DYNAMIC_PROGRAMS)
    add_dependencies(savanxp-i386-${name} savanxp_i386_resources)
    set_property(TARGET savanxp-i386-${name} APPEND PROPERTY LINK_DEPENDS "${resource_stamp}")
    add_custom_command(TARGET savanxp-i386-${name} POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/stamp_sxe.py"
            --binary "$<TARGET_FILE:savanxp-i386-${name}>" --name "${name}"
            --resource-dir "${resource_dir}" --objcopy "${SAVANXP_OBJCOPY}" --readelf "${SAVANXP_READELF}"
        VERBATIM)
endforeach()
