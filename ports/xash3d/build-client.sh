# Sourced by build.sh after the common port modules have been built.
client_bin="$stage/bin/xash3d-client"
client_dir="$stage/games/xash3d/valve/cl_dlls"
client_module="$client_dir/client_savanxp_amd64.so"
menu_module="$client_dir/menu_savanxp_amd64.so"
mkdir -p "$client_dir" "$objects/client-engine" "$objects/client-game" "$objects/client-menu"

game_ui="$scratch/mainui_cpp"
freevgui="$scratch/freevgui"
cxxrt="$scratch/libcxxrt"
extract_source mainui_cpp "$(pin mainui_commit)" mainui_ "$game_ui"
extract_source miniutl "$(pin miniutl_commit)" miniutl_ "$game_ui/miniutl"
extract_source freevgui "$(pin freevgui_commit)" freevgui_ "$freevgui"
extract_source libcxxrt "$(pin cxxrt_commit)" cxxrt_ "$cxxrt"
git -C "$engine" apply "$PORT/patches/0005-build-client-engine.patch"
git -C "$source" apply "$PORT/patches/0006-build-hlsdk-client.patch"
git -C "$game_ui" init -q
git -C "$game_ui" apply "$PORT/patches/0007-build-mainui.patch"
git -C "$freevgui" init -q
git -C "$freevgui" apply "$PORT/patches/0008-build-freevgui.patch"
git -C "$game_ui/miniutl" init -q
git -C "$game_ui/miniutl" apply "$PORT/patches/0009-build-miniutl.patch"

client_objects=()
client_compile() {
    local output_dir=$1 file=$2 object
    shift 2
    local relative=${file#"$scratch/"}
    object="$objects/$output_dir/${relative//\//_}.o"
    "$CC" -c "$file" -o "$object" "$@"
    client_objects+=("$object")
}

echo "xash3d: building the POSIX client engine"
for file in "$engine"/engine/common/*.c "$engine"/engine/common/{imagelib,soundlib}/*.c \
    "$engine"/engine/server/*.c "$engine"/engine/platform/{posix,stub}/*.c \
    "$engine"/engine/client/*.c "$engine"/engine/client/*/*.c \
    "${public_sources[@]}" "$PORT"/overlay/{dedicated_savanxp,client_optional,client_video}.c; do
    case "${file##*/}" in
        whereami.c|xrcon.c|cl_steam.c|snd_mp3.c|snd_ogg_opus.c|snd_ogg_vorbis.c|voice.c) continue ;;
    esac
    client_compile client-engine "$file" "${engine_flags[@]}" -I "$engine/engine/client/sound" \
        -DXASH_REF_SOFT_ENABLED=1
done
"${SAVANXP_LINK_CC:-$CC}" "${link_flags[@]}" \
    -o "$client_bin" "${runtime[@]}" "${setjmp_runtime[@]}" "${client_objects[@]}"

client_objects=()
client_cxx=("${cxx[@]}" -fno-threadsafe-statics)
game_flags=(
    "${common[@]}" "${module_flags[@]}" -DCLIENT_DLL -DINTERNAL_VGUI_SUPPORT
    -I "$source/cl_dll" -I "$source/utils/vgui/include" -I "$freevgui"
    -fmacro-prefix-map="$freevgui=ports/xash3d/freevgui"
)
echo "xash3d: building the HLSDK client and FreeVGUI"
for file in "$source"/cl_dll/*.cpp "$source"/cl_dll/*/*.cpp "$source"/game_shared/*.cpp \
    "$source"/pm_shared/*.c "$source"/external/openbsd/strl{cpy,cat}.c \
    "$source/public/safe_snprintf.c" "$source"/dlls/{crossbow,crowbar,egon,gauss,glock,handgrenade,hornetgun,mp5,python,rpg,satchel,shotgun,squeakgrenade,tripmine}.cpp \
    "$freevgui"/*.cpp "$freevgui"/controls/*.cpp "$freevgui"/platform/{common,posix,xash3d-fwgs}/*.cpp; do
    case "$file" in
        */GameStudioModelRenderer_Sample.cpp|*/voice_vgui_tweakdlg.cpp|*/voice_gamemgr.cpp|*/game_shared/voice_status.cpp) continue ;;
    esac
    if [[ "$file" == *.c ]]; then
        client_compile client-game "$file" "${game_flags[@]}" -std=gnu11
    else
        client_compile client-game "$file" "${game_flags[@]}" "${client_cxx[@]}" -frtti
    fi
done

# FreeVGUI supplies allocation operators. libcxxrt supplies the type metadata
# and dynamic casts. Exceptions and the demangler are outside this build.
client_compile client-game "$PORT/overlay/cxx_runtime.cpp" \
    "${common[@]}" "${client_cxx[@]}" -fvisibility=hidden -DXASH3D_FREEVGUI
for file in "$cxxrt"/src/{dynamic_cast,typeinfo}.cc; do
    client_compile client-game "$file" "${abi_flags[@]}" -O2 -I "$SYSROOT/include" \
        "${client_cxx[@]}" -frtti -fvisibility=hidden \
        -fmacro-prefix-map="$cxxrt=ports/xash3d/libcxxrt"
done
"$LD" -shared -soname "${client_module##*/}" -z max-page-size=0x1000 --build-id=none \
    --gc-sections -o "$client_module" "${client_objects[@]}"

client_objects=()
menu_flags=(
    "${abi_flags[@]}" -O2 -I "$SYSROOT/include" -isystem "$PORT/overlay/cxx"
    -D__savanxp__ -D_DEFAULT_SOURCE '-DSTDINT_H=<stdint.h>' -fvisibility=hidden
    -fmacro-prefix-map="$game_ui=ports/xash3d/mainui_cpp"
)
for directory in . miniutl font controls menus model sdk_includes/{common,engine,public,pm_shared}; do
    menu_flags+=(-I "$game_ui/$directory")
done
echo "xash3d: building the main menu"
for file in "$game_ui"/*.cpp "$game_ui"/{miniutl,font,menus,controls}/*.cpp \
    "$game_ui"/menus/*/*.cpp "$PORT/overlay/cxx_runtime.cpp"; do
    client_compile client-menu "$file" "${menu_flags[@]}" "${client_cxx[@]}"
done
"$LD" -shared -soname "${menu_module##*/}" -z max-page-size=0x1000 --build-id=none \
    --gc-sections -o "$menu_module" "${client_objects[@]}"

programs+=("${client_bin##*/}")
outputs+=("$client_bin" "$client_module" "$menu_module")
