#!/usr/bin/env bash
# Build the Xash3D dedicated server, renderer, modules and POSIX probes.
set -euo pipefail

PORT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$PORT/../.." && pwd)
# shellcheck source=../../tools/lib/sdk_flags.sh
. "$ROOT/tools/lib/sdk_flags.sh"

OUTPUT_ROOT=${SAVANXP_OUTPUT_ROOT:-$ROOT/build}
WORK="$OUTPUT_ROOT/ports/xash3d"
IMAGE=${SAVANXP_DISK_IMAGE:-$OUTPUT_ROOT/disk.img}
SXFS_CLI=${SAVANXP_SXFS_CLI:-$OUTPUT_ROOT/tools/sxfs-cli}
CC=${SAVANXP_CLANG:-clang}
LD=${SAVANXP_LD:-ld.lld}
PYTHON=${SAVANXP_PYTHON:-python3}
OBJCOPY=${SAVANXP_OBJCOPY:-llvm-objcopy}
READELF=${SAVANXP_READELF:-llvm-readelf}
INSTALL=1
BUILD_CLIENT=0
VALVE_DATA=
PROGRAM=xash3d-probe
MODULE=hl_savanxp_amd64.so
FS_PROBE=xash3d-fsprobe
FS_MODULE=filesystem_stdio.so
DEDICATED=xash3d-dedicated
REF_MODULE=ref_soft.so
REF_PROBE=xash3d-refprobe

while (($#)); do
    case "$1" in
        --no-install) INSTALL=0 ;;
        --client) BUILD_CLIENT=1 ;;
        --valve)
            if [[ -z ${2:-} || $2 == --* ]]; then
                echo "xash3d: --valve requires an unpacked Half-Life valve directory" >&2
                exit 2
            fi
            VALVE_DATA=$2
            BUILD_CLIENT=1
            shift ;;
        -h|--help) echo "Usage: ports/xash3d/build.sh [--client] [--valve PATH] [--no-install]"; exit 0 ;;
        *) echo "xash3d: unknown argument '$1'" >&2; exit 2 ;;
    esac
    shift
done
if ((!INSTALL)) && [[ -n "$VALVE_DATA" ]]; then
    echo "xash3d: --valve installs game data and cannot be used with --no-install" >&2
    exit 2
fi
if ((INSTALL)) && [[ ( ! -f "$IMAGE" && ! -b "$IMAGE" ) || ! -x "$SXFS_CLI" ]]; then
    echo "xash3d: run ./build.sh build before installing the port" >&2
    exit 1
fi

pin() {
    sed -n "s/^$1=//p" "$PORT/UPSTREAM"
}

commit=$(pin commit)
engine_commit=$(pin engine_commit)
suffix_commit=$(pin suffix_commit)
mkdir -p "$WORK"
scratch=$(mktemp -d "$WORK/work.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
verify_archive() {
    printf '%s  %s\n' "$2" "$1" | sha256sum --check --status
}

extract_source() {
    local name=$1 revision=$2 key_prefix=$3 destination=$4
    local archive="$WORK/$name-$revision.tar.gz"
    local digest
    digest=$(pin "${key_prefix}archive_sha256")
    if [[ ! -f "$archive" ]]; then
        curl -fL --retry 2 --output "$scratch/$name.tar.gz" "$(pin "${key_prefix}archive_url")"
        if ! verify_archive "$scratch/$name.tar.gz" "$digest"; then
            echo "xash3d: downloaded $name archive checksum mismatch" >&2
            exit 1
        fi
        mv -f -- "$scratch/$name.tar.gz" "$archive"
    fi
    if ! verify_archive "$archive" "$digest"; then
        echo "xash3d: $name archive checksum mismatch: $archive" >&2
        exit 1
    fi
    mkdir -p -- "$destination"
    tar -xf "$archive" -C "$destination" --strip-components=1
}

SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$ROOT/tools/make-sdk.sh" --quiet
SYSROOT=$(SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$ROOT/tools/make-sdk.sh" --print-path)

source="$scratch/hlsdk"
engine="$scratch/engine"
suffix="$scratch/suffix"
objects="$scratch/objects"
resources="$scratch/sxe"
stage="$scratch/stage"
mkdir -p "$objects/fs" "$objects/dedicated" "$objects/ref" "$stage/bin" "$stage/games/xash3d/valve/dlls"
if [[ -n "$VALVE_DATA" ]]; then
    "$PYTHON" "$PORT/assets.py" "$VALVE_DATA" "$stage"
fi
extract_source hlsdk-portable "$commit" "" "$source"
extract_source xash3d-fwgs "$engine_commit" engine_ "$engine"
extract_source library-suffix "$suffix_commit" suffix_ "$suffix"
git -C "$source" init -q
git -C "$engine" init -q
git -C "$suffix" init -q
git -C "$source" apply "$PORT/patches/0001-recognize-savanxp.patch"
git -C "$engine" apply "$PORT/patches/0002-build-filesystem-stdio.patch"
git -C "$suffix" apply "$PORT/patches/0003-library-suffix-savanxp.patch"
git -C "$engine" apply "$PORT/patches/0004-build-dedicated-server.patch"
git -C "$engine" apply "$PORT/patches/0010-pack-software-screenshots.patch"

cat >"$scratch/vcs_info.h" <<EOF
#define HLSDK_COMMIT "$commit"
[[maybe_unused]] static char g_VCSInfo_Commit[] = HLSDK_COMMIT;
[[maybe_unused]] static char g_VCSInfo_Branch[] = "master";
EOF

mapfile -t abi_flags < <(savanxp_sdk_abi_flags 1 1)
mapfile -t link_flags < <(savanxp_sdk_pie_link_flags 0)
common=(
    "${abi_flags[@]}" -O2 -I "$SYSROOT/include" -I "$scratch"
    -D__savanxp__ -D_DEFAULT_SOURCE
    -fmacro-prefix-map="$source=ports/xash3d/hlsdk-portable"
)
for directory in dlls common engine pm_shared game_shared public; do
    common+=(-I "$source/$directory")
done
cxx=(
    -std=c++17 -fno-exceptions -fno-rtti -fsized-deallocation
    -include "$PORT/overlay/sdk_c_headers.h"
    -Wno-invalid-offsetof -Wno-nontrivial-memcall
)
module_flags=(
    -fvisibility=hidden -Dstricmp=strcasecmp -Dstrnicmp=strncasecmp
    -D_snprintf=snprintf -D_vsnprintf=vsnprintf -DXASH3D_HLSDK
)

echo "xash3d: building HLSDK Portable $commit"
module_objects=()
for file in "$source"/dlls/*.cpp "$source"/pm_shared/*.c \
    "$source"/external/openbsd/strl{cpy,cat}.c "$source/public/safe_snprintf.c" \
    "$source/game_shared/voice_gamemgr.cpp" "$PORT/overlay/cxx_runtime.cpp"; do
    name=${file##*/}
    case "$name" in
        mpstubb.cpp|stats.cpp|Wxdebug.cpp) continue ;;
    esac
    object="$objects/${name%.*}.o"
    if [[ "$file" == *.cpp ]]; then
        "$CC" -c "$file" -o "$object" "${common[@]}" "${module_flags[@]}" "${cxx[@]}"
    else
        "$CC" -c "$file" -o "$object" "${common[@]}" "${module_flags[@]}" -std=gnu11
    fi
    module_objects+=("$object")
done

module="$stage/games/xash3d/valve/dlls/$MODULE"
"$LD" -shared -soname "$MODULE" -z max-page-size=0x1000 --build-id=none \
    -o "$module" "${module_objects[@]}"

probe="$stage/bin/$PROGRAM"
"$CC" -c "$PORT/overlay/module_probe.cpp" -o "$objects/module_probe.o" \
    "${common[@]}" "${cxx[@]}"
mapfile -t runtime < <(savanxp_sdk_runtime_objects "$SYSROOT" pie core math loader dlfcn)
"${SAVANXP_LINK_CC:-$CC}" "${link_flags[@]}" \
    -o "$probe" "${runtime[@]}" "$objects/module_probe.o"

fs_common=(
    "${abi_flags[@]}" -O2 -I "$SYSROOT/include" -I "$suffix/include"
    -I "$engine/common" -I "$engine/engine" -I "$engine/public" -I "$engine/filesystem"
    -I "$engine/3rdparty/miniz" -D__savanxp__ -D_DEFAULT_SOURCE -DHAVE_STRCASECMP=1
    -DXASH_MINIZ=1 -DMINIZ_NO_ARCHIVE_APIS
    "-DXASH_BUILD_COMMIT=\"$engine_commit\"" '-DXASH_BUILD_BRANCH="master"'
    "-DXASH_BUILD_COMMIT_DATE=\"$(pin engine_commit_date)\""
    -fmacro-prefix-map="$engine=ports/xash3d/xash3d-fwgs"
    -fmacro-prefix-map="$suffix=ports/xash3d/library-suffix"
)
echo "xash3d: building filesystem_stdio from Xash3D FWGS $engine_commit"
public_sources=("$engine"/public/*.c "$engine/3rdparty/miniz/miniz.c" "$suffix/src/library_suffix.c")
fs_objects=()
for file in "$engine"/filesystem/*.c "${public_sources[@]}"; do
    name=${file##*/}
    object="$objects/fs/${name%.c}.o"
    "$CC" -c "$file" -o "$object" "${fs_common[@]}" -std=gnu11 -fvisibility=hidden
    fs_objects+=("$object")
done
"$CC" -c "$engine/filesystem/VFileSystem009.cpp" -o "$objects/fs/VFileSystem009.o" \
    "${fs_common[@]}" "${cxx[@]}" -fvisibility=hidden '-DALLOCA_H=<stdlib.h>' -Dalloca=__builtin_alloca
"$CC" -c "$PORT/overlay/cxx_runtime.cpp" -o "$objects/fs/cxx_runtime.o" \
    "${fs_common[@]}" "${cxx[@]}" -fvisibility=hidden

fs_module="$stage/games/xash3d/$FS_MODULE"
"$LD" -shared -soname "$FS_MODULE" -z max-page-size=0x1000 --build-id=none \
    --gc-sections -o "$fs_module" "${fs_objects[@]}" "$objects/fs/VFileSystem009.o" "$objects/fs/cxx_runtime.o"

fs_probe="$stage/bin/$FS_PROBE"
"$CC" -c "$PORT/overlay/filesystem_probe.c" -o "$objects/filesystem_probe.o" \
    "${fs_common[@]}" -std=gnu11
"${SAVANXP_LINK_CC:-$CC}" "${link_flags[@]}" \
    -o "$fs_probe" "${runtime[@]}" "$objects/filesystem_probe.o"

engine_flags=(
    "${fs_common[@]}" -std=gnu11 -Wno-pointer-sign
    -I "$PORT/overlay" -I "$engine/pm_shared" -I "$engine/engine/platform"
    -I "$engine/engine/common" -I "$engine/engine/common/imagelib"
    -I "$engine/engine/common/soundlib" -I "$engine/engine/server"
    -I "$engine/engine/client" -I "$engine/engine/client/vgui"
    -DENGINE_DLL=1 -DXASH_ENABLE_MAIN=1 -DXASH_CRASHHANDLER=0
    -DSTDIN_FILENO=0 -DSTDOUT_FILENO=1 -DSTDERR_FILENO=2
)
echo "xash3d: building the POSIX dedicated server"
dedicated_objects=()
for file in "$engine"/engine/common/*.c "$engine"/engine/common/{imagelib,soundlib}/*.c \
    "$engine"/engine/server/*.c "$engine"/engine/platform/posix/*.c \
    "${public_sources[@]}" "$PORT/overlay/dedicated_savanxp.c"; do
    name=${file##*/}
    case "$name" in
        whereami.c|xrcon.c) continue ;;
    esac
    object="$objects/dedicated/${name%.c}.o"
    "$CC" -c "$file" -o "$object" "${engine_flags[@]}" -DXASH_DEDICATED=1
    dedicated_objects+=("$object")
done

dedicated="$stage/bin/$DEDICATED"
mapfile -t setjmp_runtime < <(savanxp_sdk_runtime_objects "$SYSROOT" pie setjmp)
"${SAVANXP_LINK_CC:-$CC}" "${link_flags[@]}" \
    -o "$dedicated" "${runtime[@]}" "${setjmp_runtime[@]}" "${dedicated_objects[@]}"

ref_flags=(
    "${fs_common[@]}" -std=gnu11 -Wno-pointer-sign
    -I "$engine/ref/common" -I "$engine/ref/soft"
    -I "$engine/pm_shared" -I "$engine/engine/common" -I "$engine/engine/client"
)
echo "xash3d: building the POSIX software renderer"
ref_objects=()
for file in "$engine"/ref/{common,soft}/*.c "$engine"/public/*.c "$suffix/src/library_suffix.c"; do
    name=${file##*/}
    object="$objects/ref/${name%.c}.o"
    "$CC" -c "$file" -o "$object" "${ref_flags[@]}" -DREF_DLL=1 -fvisibility=hidden
    ref_objects+=("$object")
done
ref_module="$stage/games/xash3d/$REF_MODULE"
"$LD" -shared -soname "$REF_MODULE" -z max-page-size=0x1000 --build-id=none \
    --gc-sections -o "$ref_module" "${ref_objects[@]}"

ref_probe="$stage/bin/$REF_PROBE"
"$CC" -c "$PORT/overlay/renderer_probe.c" -o "$objects/renderer_probe.o" "${ref_flags[@]}"
"${SAVANXP_LINK_CC:-$CC}" "${link_flags[@]}" \
    -o "$ref_probe" "${runtime[@]}" "$objects/renderer_probe.o"

programs=("$PROGRAM" "$FS_PROBE" "$DEDICATED" "$REF_PROBE")
manifest_dirs=(--manifest-dir "$PORT/overlay")
outputs=("$probe" "$module" "$fs_probe" "$fs_module" "$dedicated" "$ref_probe" "$ref_module")
notices=()
for notice in "$PORT"/licenses/*.notice; do
    if ((BUILD_CLIENT)) || [[ "${notice##*/}" != client-* ]]; then
        notices+=("$notice")
    fi
done
if ((BUILD_CLIENT)); then
    . "$PORT/build-client.sh"
fi

build_id=$(git -C "$ROOT" rev-parse --short HEAD)
program_args=()
for program in "${programs[@]}"; do
    program_args+=(--program "$program")
done
"$PYTHON" "$ROOT/tools/gen_sxe_resources.py" --project-root "$ROOT" \
    --output-dir "$resources" "${manifest_dirs[@]}" --build-id "$build_id" "${program_args[@]}"
for program in "${programs[@]}"; do
    "$PYTHON" "$ROOT/tools/stamp_sxe.py" --binary "$stage/bin/$program" --name "$program" \
        --resource-dir "$resources" --objcopy "$OBJCOPY" --readelf "$READELF"
done

cp -- "${outputs[@]}" "$WORK/"
if ((INSTALL)); then
    "$PYTHON" "$ROOT/tools/stage_licenses.py" stage --root "$ROOT" --out "$stage/system/licenses" \
        "${notices[@]}"
    "$PYTHON" "$ROOT/tools/sxfs_sync.py" --target "$IMAGE" --source "$stage" --cli "$SXFS_CLI" \
        --sectors "${SAVANXP_SXFS_SECTORS:-2097152}"
fi
echo "xash3d: built $WORK/$PROGRAM and $WORK/$MODULE"
echo "xash3d: built $WORK/$FS_PROBE and $WORK/$FS_MODULE"
echo "xash3d: built $WORK/$DEDICATED"
echo "xash3d: built $WORK/$REF_PROBE and $WORK/$REF_MODULE"
if ((BUILD_CLIENT)); then
    echo "xash3d: built $WORK/${client_bin##*/}, $WORK/${client_module##*/} and $WORK/${menu_module##*/}"
fi
