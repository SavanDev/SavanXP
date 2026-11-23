#!/usr/bin/env bash
# Build HLSDK Portable's server module and the POSIX program that tests it.
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
PROGRAM=xash3d-probe
MODULE=hl_savanxp_amd64.so

case "${1:-}" in
    "") ;;
    --no-install) INSTALL=0; shift ;;
    -h|--help) echo "Usage: ports/xash3d/build.sh [--no-install]"; exit 0 ;;
esac
if (($#)); then
    echo "xash3d: unknown argument '$1'" >&2
    exit 2
fi
if ((INSTALL)) && [[ ! -f "$IMAGE" || ! -x "$SXFS_CLI" ]]; then
    echo "xash3d: run ./build.sh build before installing the port" >&2
    exit 1
fi

pin() {
    sed -n "s/^$1=//p" "$PORT/UPSTREAM"
}

commit=$(pin commit)
url=$(pin archive_url)
digest=$(pin archive_sha256)
archive="$WORK/hlsdk-portable-$commit.tar.gz"
mkdir -p "$WORK"
scratch=$(mktemp -d "$WORK/work.XXXXXX")
trap 'rm -rf -- "$scratch"' EXIT
verify_archive() {
    printf '%s  %s\n' "$digest" "$1" | sha256sum --check --status
}
if [[ ! -f "$archive" ]]; then
    curl -fL --retry 2 --output "$scratch/hlsdk.tar.gz" "$url"
    if ! verify_archive "$scratch/hlsdk.tar.gz"; then
        echo "xash3d: downloaded HLSDK archive checksum mismatch" >&2
        exit 1
    fi
    mv -f -- "$scratch/hlsdk.tar.gz" "$archive"
fi
if ! verify_archive "$archive"; then
    echo "xash3d: HLSDK archive checksum mismatch: $archive" >&2
    exit 1
fi

SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$ROOT/tools/make-sdk.sh" --quiet
SYSROOT=$(SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$ROOT/tools/make-sdk.sh" --print-path)

source="$scratch/hlsdk"
objects="$scratch/objects"
resources="$scratch/sxe"
stage="$scratch/stage"
mkdir -p "$source" "$objects" "$stage/bin" "$stage/games/xash3d/valve/dlls"
tar -xf "$archive" -C "$source" --strip-components=1
git -C "$source" init -q
git -C "$source" apply "$PORT/patches/0001-recognize-savanxp.patch"

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
    -D_snprintf=snprintf -D_vsnprintf=vsnprintf
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

build_id=$(git -C "$ROOT" rev-parse --short HEAD)
"$PYTHON" "$ROOT/tools/gen_sxe_resources.py" --project-root "$ROOT" \
    --output-dir "$resources" --manifest-dir "$PORT/overlay" --build-id "$build_id" \
    --program "$PROGRAM"
"$PYTHON" "$ROOT/tools/stamp_sxe.py" --binary "$probe" --name "$PROGRAM" \
    --resource-dir "$resources" --objcopy "$OBJCOPY" --readelf "$READELF"

cp -- "$probe" "$module" "$WORK/"
if ((INSTALL)); then
    "$PYTHON" "$ROOT/tools/stage_licenses.py" stage --root "$ROOT" --out "$stage/system/licenses" \
        "$PORT/licenses/hlsdk-portable.notice"
    "$PYTHON" "$ROOT/tools/sxfs_sync.py" --image "$IMAGE" --source "$stage" --cli "$SXFS_CLI" \
        --sectors "${SAVANXP_SXFS_SECTORS:-2097152}"
fi
echo "xash3d: built $WORK/$PROGRAM and $WORK/$MODULE"
