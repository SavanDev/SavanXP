# Keep the reference embedded-loader profiles: the executable exports the SDK,
# shared objects supply their units, and crt0 resolves DT_NEEDED before main.
set(SAVANXP_I386_PIE_COMPILE ${SAVANXP_USER_COMPILE_OPTIONS})
list(REMOVE_ITEM SAVANXP_I386_PIE_COMPILE -fno-pic -fno-pie)
list(APPEND SAVANXP_I386_PIE_COMPILE -fPIE -mstack-protector-guard=global)
set(SAVANXP_I386_LIBRARY_COMPILE ${SAVANXP_I386_PIE_COMPILE})
list(REMOVE_ITEM SAVANXP_I386_LIBRARY_COMPILE -fPIE)
list(APPEND SAVANXP_I386_LIBRARY_COMPILE -fPIC)

foreach(variant IN ITEMS full nomath)
    set(sources ${SAVANXP_I386_USER_RUNTIME_SOURCES})
    if(variant STREQUAL "nomath")
        list(REMOVE_ITEM sources subsystems/posix/sdk/v1/runtime/math.c)
    endif()
    add_library(savanxp_i386_pie_${variant} OBJECT ${sources})
    target_include_directories(savanxp_i386_pie_${variant} PRIVATE
        "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland" "${SAVANXP_GENERATED_ROOT}")
    target_compile_options(savanxp_i386_pie_${variant} PRIVATE ${SAVANXP_I386_PIE_COMPILE})
endforeach()

function(savanxp_i386_library name source)
    add_library(savanxp-i386-${name} SHARED ${source})
    target_include_directories(savanxp-i386-${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include")
    target_compile_options(savanxp-i386-${name} PRIVATE ${SAVANXP_I386_LIBRARY_COMPILE})
    target_link_options(savanxp-i386-${name} PRIVATE -nostdlib -fuse-ld=lld
        -Wl,-m,elf_i386 -Wl,-soname,${name}.so.0.4 -Wl,-z,max-page-size=0x1000
        -Wl,--hash-style=sysv -Wl,--build-id=none)
    set_target_properties(savanxp-i386-${name} PROPERTIES SKIP_BUILD_RPATH TRUE PREFIX "" SUFFIX "" OUTPUT_NAME "${name}.so.0.4")
    add_dependencies(savanxp_i386_userland savanxp-i386-${name})
    add_custom_command(TARGET savanxp_i386_diskfs POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SAVANXP_DISKFS_ROOT}/lib"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:savanxp-i386-${name}>" "${SAVANXP_DISKFS_ROOT}/lib/${name}.so.0.4"
        VERBATIM)
    set(SAVANXP_I386_LIBRARIES ${SAVANXP_I386_LIBRARIES} ${name}.so.0.4 PARENT_SCOPE)
endfunction()

function(savanxp_i386_pie name)
    cmake_parse_arguments(P "" "" "DEPENDS;DEFINES" ${ARGN})
    add_executable(savanxp-i386-${name} subsystems/posix/userland/${name}.c subsystems/posix/userland/ldso.c)
    target_include_directories(savanxp-i386-${name} PRIVATE
        "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/subsystems/posix/sdk/v1/include"
        "${CMAKE_SOURCE_DIR}/subsystems/posix/userland" "${SAVANXP_GENERATED_ROOT}")
    target_compile_options(savanxp-i386-${name} PRIVATE ${SAVANXP_I386_PIE_COMPILE})
    target_compile_definitions(savanxp-i386-${name} PRIVATE ${P_DEFINES})
    if("libmath" IN_LIST P_DEPENDS)
        target_link_libraries(savanxp-i386-${name} PRIVATE savanxp_i386_pie_nomath)
    else()
        target_link_libraries(savanxp-i386-${name} PRIVATE savanxp_i386_pie_full)
    endif()
    foreach(lib IN LISTS P_DEPENDS)
        target_link_libraries(savanxp-i386-${name} PRIVATE savanxp-i386-${lib})
    endforeach()
    target_link_options(savanxp-i386-${name} PRIVATE -nostdlib -fuse-ld=lld
        -Wl,-m,elf_i386 -Wl,-Bdynamic -pie -Wl,--export-dynamic
        -Wl,--dynamic-linker,/disk/lib/ld.so.0.4 -Wl,-z,max-page-size=0x1000
        -Wl,--hash-style=sysv -Wl,--build-id=none)
    set_target_properties(savanxp-i386-${name} PROPERTIES SKIP_BUILD_RPATH TRUE SUFFIX "" OUTPUT_NAME ${name})
    add_dependencies(savanxp_i386_userland savanxp-i386-${name})
    add_custom_command(TARGET savanxp_i386_diskfs POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:savanxp-i386-${name}>" "${SAVANXP_DISKFS_ROOT}/bin/${name}"
        VERBATIM)
    set(SAVANXP_I386_DYNAMIC_PROGRAMS ${SAVANXP_I386_DYNAMIC_PROGRAMS} ${name} PARENT_SCOPE)
endfunction()

savanxp_i386_library(libmath subsystems/posix/sdk/v1/runtime/math.c)
if(SAVANXP_INCLUDE_TEST_APPS)
    savanxp_i386_library(libchainbase subsystems/posix/userland/chaintest_lib.c)
    savanxp_i386_library(libchaintop subsystems/posix/userland/chaintest_lib.c)
    target_compile_definitions(savanxp-i386-libchaintop PRIVATE CHAIN_TOP)
    target_link_libraries(savanxp-i386-libchaintop PRIVATE savanxp-i386-libchainbase)
    foreach(part IN ITEMS leaf left right top)
        savanxp_i386_library(libdia_${part} subsystems/posix/userland/diamondlib.c)
        string(TOUPPER "${part}" define)
        target_compile_definitions(savanxp-i386-libdia_${part} PRIVATE DIA_${define})
    endforeach()
    target_link_libraries(savanxp-i386-libdia_left PRIVATE savanxp-i386-libdia_leaf)
    target_link_libraries(savanxp-i386-libdia_right PRIVATE savanxp-i386-libdia_leaf)
    target_link_libraries(savanxp-i386-libdia_top PRIVATE savanxp-i386-libdia_left savanxp-i386-libdia_right)
    savanxp_i386_library(libelf32 subsystems/posix/userland/elf32lib.c)
    savanxp_i386_pie(dyntest32 DEPENDS libelf32)
    savanxp_i386_library(libbroken subsystems/posix/userland/brokenlib.c)
    savanxp_i386_library(libneeded subsystems/posix/userland/neededlib.c)
    savanxp_i386_pie(missingtest)
    savanxp_i386_pie(brokentest)
    savanxp_i386_pie(needstest DEPENDS libneeded)
    savanxp_i386_library(libsdkint subsystems/posix/userland/sdkintlib.c)
    savanxp_i386_pie(sdkintpie DEPENDS libsdkint)
    savanxp_i386_pie(pietest)
    savanxp_i386_pie(ldtest)
    savanxp_i386_pie(libtest DEPENDS libmath)
    savanxp_i386_pie(interptest)
    savanxp_i386_pie(diamondtest)
    savanxp_i386_pie(slottest DEFINES SAVANXP_LD_MAX_LIBRARIES=4)
endif()

# Enforce actual sharing using the built ELF artifacts, as in the reference.
add_custom_command(TARGET savanxp_i386_diskfs POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_shared_libs.py"
        "${CMAKE_BINARY_DIR}" --library-dir "${SAVANXP_DISKFS_ROOT}/lib" --nm "${SAVANXP_NM}" --readelf "${SAVANXP_READELF}"
    VERBATIM)
