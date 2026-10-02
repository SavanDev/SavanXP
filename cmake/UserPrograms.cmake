# Every in-tree program is registered explicitly. Keeping this list in CMake
# makes the target graph the single source of truth for the userland.
function(savanxp_program)
    set(options TEST)
    set(oneValueArgs NAME INTERPRETER LINK_PROFILE)
    set(multiValueArgs SOURCES DEPENDS)
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
    if(P_LINK_PROFILE STREQUAL "STATIC" OR NOT P_LINK_PROFILE)
        target_link_libraries(${P_NAME} PRIVATE savanxp_user_runtime)
    elseif(P_LINK_PROFILE STREQUAL "PIE")
        # Un PIE nace del runtime PIC: crt0.S ya es position independent
        # (%rip y call relativo) y esta dentro de savanxp_user_runtime_pic.
        #
        # El runtime que corresponde es el que NO trae las unidades que sus
        # DEPENDSLeaving en una libreria. No es una bandera que el autor pone:
        # sale de la lista, porque forgettingla es un fallo silencioso --el
        # programa enlazaria bien y estaria con una copia propia-- en vez de uno
        # ruidoso.
        set(dropped_units "")
        foreach(lib IN LISTS P_DEPENDS)
            if(SAVANXP_LIBRARY_REPLACES_${lib})
                foreach(unit IN LISTS SAVANXP_LIBRARY_REPLACES_${lib})
                    list(APPEND dropped_units "subsystems/posix/sdk/v1/runtime/${unit}")
                endforeach()
            endif()
        endforeach()
        if(dropped_units)
            list(SORT dropped_units)
            set(key full)
            foreach(unit IN LISTS dropped_units)
                get_filename_component(base "${unit}" NAME)
                set(key "${key}_${base}")
            endforeach()
            string(REGEX REPLACE "[.]c$" "" key "${key}")
            string(REGEX REPLACE "_" "-" key "${key}")
            set(variant "savanxp_user_runtime_pic_${key}")
            if(NOT TARGET ${variant})
                savanxp_runtime_variant(NAME ${variant} PIC EXCLUDE ${dropped_units})
            endif()
            target_link_libraries(${P_NAME} PRIVATE ${variant})
        else()
            target_link_libraries(${P_NAME} PRIVATE savanxp_user_runtime_pic)
        endif()
        # El cargador va con el perfil, no con el programa. Una imagen ET_DYN la
        # mapea el kernel pero no la reubica, asi que sin ldso.c el GOT de este
        # executable queda vacio y la primera llamada a una libreria salta a
        # donde se le ocurra.
        #
        # Que sea automatico no es cosmetico: con la lista a mano, calc llevo
        # PIE sin el cargador y fallo. Y peor, ldtest lo lleva y lo lleva bien,
        # porque llama al cargador a mano -- una regla que el build no verifica y
        # que se cumple por costumbre en un programa y se olvida en el otro.
        # crt0 lo corre solo por el hook debil sx_run_interpreter, que es lo que
        # define ldso.c.
        target_sources(${P_NAME} PRIVATE subsystems/posix/userland/ldso.c)
        target_compile_options(${P_NAME} PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS_PIC})
    endif()
    # DEPENDS produce los DT_NEEDED del ejecutable. Solo el -soname de cada
    # libreria llega a la tabla dinamica, no la ruta de build, asi que el
    # nombre que el cargador tiene que volver a convertir en ruta bajo /lib.
    if(P_DEPENDS)
        target_link_libraries(${P_NAME} PRIVATE ${P_DEPENDS})
    endif()
    if(P_INTERPRETER)
        target_link_options(${P_NAME} PRIVATE "-Wl,--dynamic-linker,${P_INTERPRETER}")
    endif()
    # Un perfil que no existe es un fallo de configuracion, no un default
    # silencioso: enlazar como STATIC algo que el autor marco PIE daria un
    # ET_EXEC donde se esperaba un ET_DYN, y el cargador cargaria la imagen
    # equivocada sin que nadie lo notara.
    if(P_LINK_PROFILE STREQUAL "PIE" AND DEFINED SAVANXP_USER_LINK_OPTIONS_PIE)
        target_link_options(${P_NAME} PRIVATE ${SAVANXP_USER_LINK_OPTIONS_PIE})
        target_compile_options(${P_NAME} PRIVATE ${SAVANXP_USER_COMPILE_OPTIONS_PIE})
    elseif(P_LINK_PROFILE STREQUAL "PIE")
        message(FATAL_ERROR "savanxp_program(${P_NAME}): LINK_PROFILE PIE sin bloque SAVANXP_USER_LINK_OPTIONS_PIE")
    elseif(P_LINK_PROFILE AND NOT P_LINK_PROFILE STREQUAL "STATIC")
        message(FATAL_ERROR "savanxp_program(${P_NAME}): LINK_PROFILE desconocido '${P_LINK_PROFILE}'")
    else()
        target_link_options(${P_NAME} PRIVATE ${SAVANXP_USER_LINK_OPTIONS_STATIC})
    endif()
    set_target_properties(${P_NAME} PROPERTIES OUTPUT_NAME "${P_NAME}" SUFFIX "")
    set_property(GLOBAL APPEND PROPERTY SAVANXP_USER_TARGETS ${P_NAME})
    set_property(GLOBAL APPEND PROPERTY SAVANXP_USER_PROGRAM_NAMES ${P_NAME})
endfunction()

# A shared library. Not a program: it gets no SXE stamp, no desktop entry, and
# lands in /lib rather than /bin, so Program Manager never lists it.
#
# The canary is on. Every object now references __stack_chk_guard and
# __stack_chk_fail, both of which live in the executable, so a library only
# loads if the loader puts the executable in the symbol scope. That is no longer
# conditional, and it is the reason the loader can do it.
function(savanxp_library)
    set(options TEST)
    set(oneValueArgs NAME SONAME)
    set(multiValueArgs SOURCES DEFINES DEPENDS)
    cmake_parse_arguments(P "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    if(P_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "savanxp_library(${P_NAME}): unknown arguments: ${P_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT P_NAME OR NOT P_SOURCES OR NOT P_SONAME)
        message(FATAL_ERROR "savanxp_library requires NAME, SONAME and SOURCES")
    endif()
    if(P_TEST AND NOT SAVANXP_INCLUDE_TEST_APPS)
        return()
    endif()
    # SHARED, no MODULE: una MODULE_LIBRARY no se puede enlazar contra otro
    # objetivo, y una libreria que depende de otra es justamente el caso que
    # DT_NEEDED tiene que cubrir.
    add_library(${P_NAME} SHARED ${P_SOURCES})
    if(P_DEFINES)
        target_compile_definitions(${P_NAME} PRIVATE ${P_DEFINES})
    endif()
    # DEPENDS produces the DT_NEEDED the loader walks. Only the -soname reaches
    # the dynamic table, not the build path, so the recorded name is the one the
    # loader has to turn back into a path under /lib.
    if(P_DEPENDS)
        target_link_libraries(${P_NAME} PRIVATE ${P_DEPENDS})
    endif()
    target_include_directories(${P_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
        "${SAVANXP_GENERATED_ROOT}"
    )
    target_compile_options(${P_NAME} PRIVATE ${SAVANXP_LIBRARY_COMPILE_OPTIONS})
    # -soname is what a DT_NEEDED would name; today nothing resolves it, because
    # the loader that reads it does not exist yet.
    target_link_options(${P_NAME} PRIVATE
        -nostdlib -fuse-ld=lld "-Wl,-soname,${P_SONAME}"
        "-Wl,-z,max-page-size=0x1000" -Wl,--build-id=none)
    set_target_properties(${P_NAME} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${P_SONAME}"
        SUFFIX "")
    set_property(GLOBAL APPEND PROPERTY SAVANXP_USER_LIBRARY_TARGETS ${P_NAME})
    set_property(GLOBAL APPEND PROPERTY SAVANXP_USER_LIBRARY_NAMES ${P_SONAME})
endfunction()

savanxp_program(NAME pietest TEST LINK_PROFILE PIE SOURCES subsystems/posix/userland/pietest.c)
# El unico programa que linkea contra una libreria de verdad. Todo lo demas usa
# el cargador a mano; este usa sqrt como funcion normal y no busca su direccion.
savanxp_program(NAME libtest TEST LINK_PROFILE PIE DEPENDS libmath
    SOURCES subsystems/posix/userland/libtest.c)
# PIE, no STATIC: para que una libreria resuelva un simbolo CONTRA el
# ejecutable, el ejecutable tiene que exportar una tabla dinamica. lld no emite
# .dynsym para una ET_EXEC, asi que un ldtest no-PIE no tendria contra que
# resolver y la mitad de la cadena no se podria probar.
savanxp_program(NAME ldtest TEST LINK_PROFILE PIE SOURCES
    subsystems/posix/userland/ldtest.c)
savanxp_program(NAME interptest TEST INTERPRETER /disk/lib/ld.so.0.4
    SOURCES subsystems/posix/userland/interptest.c)
savanxp_library(NAME libmath SONAME libmath.so.0.4
    SOURCES subsystems/posix/sdk/v1/runtime/math.c)
# SxGFX: la capa de graficos del cliente, con sus tres tablas de fuente dentro.
# Pide del ejecutable solo syscalls y libc, asi que resuelve contra el binario
# como las otras dos.
savanxp_library(NAME libsxgfx SONAME libsxgfx.so.0.4
    SOURCES subsystems/posix/sdk/v1/runtime/gfx.c)

# SxGUI encima de SxGFX. Antes pedia gfx_* al EJECUTABLE, lo que obligaba a que
# cada programa fuera PIE solo para poder exportar; ahora lo pide a la libreria
# de abajo y esa atadura se afloja.
savanxp_library(NAME libsxgui SONAME libsxgui.so.0.4 DEPENDS libsxgfx
    SOURCES
        subsystems/posix/sdk/v1/runtime/sxgui.c
        subsystems/posix/sdk/v1/runtime/sxgui_app.c)
savanxp_library(NAME libchainbase TEST SONAME libchainbase.so.0.4
    SOURCES subsystems/posix/userland/chaintest_lib.c)
savanxp_library(NAME libchaintop TEST SONAME libchaintop.so.0.4
    SOURCES subsystems/posix/userland/chaintest_lib.c
    DEFINES CHAIN_TOP
    DEPENDS libchainbase)

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
savanxp_program(NAME fscheck SOURCES subsystems/posix/userland/fscheck.c)
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
savanxp_program(NAME progman LINK_PROFILE PIE DEPENDS libsxgui SOURCES
    subsystems/posix/userland/progman.c
    subsystems/posix/userland/progman_registry.c
    subsystems/posix/userland/desktop_icons.c
    subsystems/posix/userland/desktop_wallpaper.c)
savanxp_program(NAME appwiz LINK_PROFILE PIE DEPENDS libsxgui SOURCES
    subsystems/posix/userland/appwiz.c
    subsystems/posix/userland/appwiz_catalog.c)
savanxp_program(NAME aboutapp LINK_PROFILE PIE DEPENDS libsxgui SOURCES
    subsystems/posix/userland/aboutapp.c)
savanxp_program(NAME taskmgr LINK_PROFILE PIE DEPENDS libsxgui SOURCES
    subsystems/posix/userland/taskmgr.c)
savanxp_program(NAME filesapp LINK_PROFILE PIE DEPENDS libsxgui SOURCES
    subsystems/posix/userland/filesapp.c
    subsystems/posix/userland/file_assoc.c
    subsystems/posix/userland/mime_icon.c)
# El editor es la app con mejor verificacion automatizada del arbol: el escenario
# shoot_session notepadwheel teclea 40 lineas, captura, scrollea con la rueda y
# COMPARA PIXELES. Si SxGUI viniera de una libreria y dibujara distinto, el
# scroll no moveria el area y la comparacion fallaria.
#
# Ese escenario es lo que faltaba para poder decir que SxGUI funciona bajo una
# libreria: ningun self-test llega al camino de dibujo.
savanxp_program(NAME notepad LINK_PROFILE PIE DEPENDS libsxgui SOURCES
    subsystems/posix/userland/notepad.c)
savanxp_program(NAME mines LINK_PROFILE PIE DEPENDS libsxgui SOURCES
    subsystems/posix/userland/mines.c
    subsystems/posix/userland/mines_board.c)
# La calculadora va como ET_DYN. Es la primera aplicacion real fuera de las
# pruebas en el perfil PIE: depende de SxGUI y se dibuja, asi que si el perfil
# serviriera para algo mas que para mover sqrt, este es el programa que lo
# demuestra. La migracion es una linea porque las unidades del runtime ya no se
# listan a mano.
# La calculadora es el primer programa de escritorio en el perfil PIE. Antes de
# ella solo lo usaban libtest y ldtest, y por eso el perfil parecia funcionar:
# esos dos llaman sqrt y nada mas, y una llamada entra por un JUMP_SLOT que se
# resuelve por nombre. Enseñar un programa real que escribe en stdout destapo
# que R_X86_64_RELATIVE estaba leyendo la memoria en vez de la adenda, y que
# lld deja la casilla en cero. docs/SHARED_LIBRARIES.md tiene los numeros.
#
savanxp_program(NAME calc LINK_PROFILE PIE
    DEPENDS libmath libsxgui SOURCES
    subsystems/posix/userland/calc.c)
# Galeria de controles: el usuario mas amplio de SxGUI, con 20 funciones, y la
# app que mas conviene mirar a ojo porque esta hecha para eso. Es la que prueba
# a mano si SxGUI dibuja bien cuando viene de una libreria, que ninguna
# comprobacion automatica cubre hoy.
savanxp_program(NAME widgetsdemo TEST LINK_PROFILE PIE DEPENDS libsxgui SOURCES
    subsystems/posix/userland/widgetsdemo.c)
savanxp_program(NAME gfxdemo TEST LINK_PROFILE PIE DEPENDS libsxgfx SOURCES
    subsystems/posix/userland/gfxdemo.c)
savanxp_program(NAME gears TEST LINK_PROFILE PIE DEPENDS libsxgfx SOURCES
    subsystems/posix/userland/gears.c)
savanxp_program(NAME gputest TEST LINK_PROFILE PIE DEPENDS libsxgfx SOURCES
    subsystems/posix/userland/gputest.c)
savanxp_program(NAME keytest TEST LINK_PROFILE PIE DEPENDS libsxgfx SOURCES
    subsystems/posix/userland/keytest.c)
savanxp_program(NAME kbdtest TEST SOURCES subsystems/posix/userland/kbdtest.c)
savanxp_program(NAME mousetest TEST LINK_PROFILE PIE DEPENDS libsxgfx SOURCES
    subsystems/posix/userland/mousetest.c)
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
savanxp_program(NAME seltest TEST LINK_PROFILE PIE DEPENDS libsxgui SOURCES
    subsystems/posix/userland/seltest.c)
savanxp_program(NAME mmaptest TEST SOURCES subsystems/posix/userland/mmaptest.c)
savanxp_program(NAME libctest TEST SOURCES subsystems/posix/userland/libctest.c)
savanxp_program(NAME stacktest TEST SOURCES subsystems/posix/userland/stacktest.c)
savanxp_program(NAME imagetest TEST SOURCES subsystems/posix/userland/imagetest.c)
savanxp_program(NAME heaptest TEST SOURCES subsystems/posix/userland/heaptest.c)
savanxp_program(NAME smoke TEST SOURCES subsystems/posix/userland/smoke.c)
savanxp_program(NAME sxetest TEST SOURCES
    subsystems/posix/userland/sxetest.c)
