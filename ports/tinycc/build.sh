#!/bin/bash
# Build and install the TinyCC port: /bin/tcc and its tree under /system/tcc.
#
# Steps: clone the pinned revision, apply patches/, drop in overlay/,
# generate tccdefs_.h with the patched c2str, build a host cross tcc that
# targets SavanXP, build libtcc1.a with it, build tcc itself for SavanXP
# with the SDK's clang flags, then stage and sync into build/disk.img.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
PORT="$ROOT/ports/tinycc"
OUTPUT_ROOT=$(realpath -m -- "${SAVANXP_OUTPUT_ROOT:-"$ROOT/build"}")
WORK="$OUTPUT_ROOT/ports/tinycc"
SRC="$WORK/source"
SYSROOT=$("$ROOT/tools/make-sdk.sh" --print-path)
IMAGE=${SAVANXP_DISK_IMAGE:-"$OUTPUT_ROOT/disk.img"}
SXFS_CLI=${SAVANXP_SXFS_CLI:-"$OUTPUT_ROOT/tools/sxfs-cli"}
PYTHON=${PYTHON:-python3}
HOST_CC=${HOST_CC:-cc}
CC=${SAVANXP_CLANG:-clang}
LD=${SAVANXP_LD:-ld.lld}
NO_INSTALL=0

usage() {
    cat <<EOF
Usage: ports/tinycc/build.sh [--no-install]

  --no-install   build /bin/tcc and the /system/tcc tree without touching
                 build/disk.img
EOF
}

while (($#)); do
    case "$1" in
        --no-install) NO_INSTALL=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "tinycc: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

REPOSITORY=$(sed -n 's/^repository=//p' "$PORT/UPSTREAM")
REVISION=$(sed -n 's/^revision=//p' "$PORT/UPSTREAM")

"$ROOT/tools/make-sdk.sh" --quiet

# --- source: the pinned revision, patched, with the overlay ---------------
mkdir -p "$WORK"
if [[ ! -d "$SRC/.git" ]]; then
    git clone --quiet "$REPOSITORY" "$SRC"
fi
git -C "$SRC" fetch --quiet origin "$REVISION" 2>/dev/null || true
git -C "$SRC" checkout --quiet --force "$REVISION"
git -C "$SRC" clean --quiet -fdx
for patch in "$PORT"/patches/*.patch; do
    patch -d "$SRC" -p1 --quiet <"$patch"
done
cp "$PORT/overlay/config.h" "$SRC/config.h"

# tccdefs.h is compiled into tcc; c2str (patched to know __savanxp__)
# converts it.
"$HOST_CC" -DC2STR "$SRC/conftest.c" -o "$WORK/c2str"
"$WORK/c2str" "$SRC/include/tccdefs.h" "$SRC/tccdefs_.h"

# --- the /system/tcc tree, as the image will hold it ----------------------
STAGE="$WORK/install-stage"
TCCDIR="$STAGE/system/tcc"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$TCCDIR/include" "$TCCDIR/lib" "$TCCDIR/sdk"
cp "$SRC"/include/*.h "$TCCDIR/include/"
rm -f "$TCCDIR/include/tccdefs.h" "$TCCDIR/include/tcclib.h"
cp "$PORT/overlay/stdint.h" "$TCCDIR/include/"
cp -r "$SYSROOT/include" "$TCCDIR/sdk/include"
# TCC always passes floats in SSE registers: the sse runtime variant.
cp "$SYSROOT/lib/sse/crt0.o" "$TCCDIR/lib/crt0.o"
cp "$SYSROOT/lib/sse/libsavanxp.a" "$TCCDIR/lib/libc.a"
# A SUN program (-o <name>.sxe) adds the SUN runtime's core group.
cp "$SYSROOT/lib/sse/sxn_runtime.o" "$TCCDIR/lib/sxn_runtime.o"
cp "$SYSROOT/lib/sse/libsxn.a" "$TCCDIR/lib/libsxn.a"

# --- host cross compiler, then libtcc1.a for SavanXP ----------------------
CROSS="$WORK/sx-tcc-cross"
# The SDK headers come after the host's: tcc reads only its contract
# headers (abi/, sxe/) from there.
"$HOST_CC" -O1 -w -DTCC_SX_TCCDIR="\"$TCCDIR\"" -I"$SRC" -idirafter "$SYSROOT/include" \
    -o "$CROSS" "$SRC/tcc.c" -lm -ldl -lpthread
LIB1_OBJ="$WORK/libtcc1"
rm -rf "$LIB1_OBJ"
mkdir -p "$LIB1_OBJ"
# tcov.c is left out: it needs fcntl record locks, which the SDK lacks.
LIB1_SOURCES=(libtcc1.c stdatomic.c atomic.S builtin.c alloca.S alloca-bt.S va_list.c dsohandle.c)
LIB1_OBJECTS=()
for source in "${LIB1_SOURCES[@]}"; do
    object="$LIB1_OBJ/${source%.*}.o"
    "$CROSS" -c "$SRC/lib/$source" -o "$object"
    LIB1_OBJECTS+=("$object")
done
"$CROSS" -ar rcs "$TCCDIR/lib/libtcc1.a" "${LIB1_OBJECTS[@]}"
# -run starts the program through runmain.o (its exit, atexit and ctors).
"$CROSS" -c "$SRC/lib/runmain.c" -o "$TCCDIR/lib/runmain.o"

# --- tcc itself, for SavanXP, with the SDK's static SSE flags -------------
# shellcheck source=../../tools/lib/sdk_flags.sh
. "$ROOT/tools/lib/sdk_flags.sh"
mapfile -t FLAGS < <(savanxp_sdk_abi_flags 1 0)
"$CC" "${FLAGS[@]}" -O2 -w -I"$SYSROOT/include" -I"$SRC" \
    -c "$SRC/tcc.c" -o "$WORK/tcc.o"
# -run resolves the program's libc calls against this tcc's own libc: a table
# of every global symbol of the libc.a it would otherwise load (tccrun.c).
"$PYTHON" "$PORT/runsyms.py" --readelf "${SAVANXP_READELF:-llvm-readelf}" \
    --archive "$TCCDIR/lib/libc.a" --output "$WORK/runsyms.c"
"$CC" "${FLAGS[@]}" -O2 -w -c "$WORK/runsyms.c" -o "$WORK/runsyms.o"
RUNTIME_OBJECTS=()
while IFS= read -r object; do
    RUNTIME_OBJECTS+=("$object")
done < <(savanxp_sdk_runtime_objects "$SYSROOT" sse core math setjmp)
"$LD" -nostdlib -static -T "$SYSROOT/linker.ld" \
    -z max-page-size=0x1000 --build-id=none \
    -o "$WORK/tcc.elf" "${RUNTIME_OBJECTS[@]}" "$WORK/tcc.o" "$WORK/runsyms.o"
mkdir -p "$WORK/resources"
"$PYTHON" "$ROOT/tools/stamp_sxe.py" \
    --binary "$WORK/tcc.elf" \
    --name tcc \
    --resource-dir "$WORK/resources" \
    --objcopy "${SAVANXP_OBJCOPY:-llvm-objcopy}" \
    --readelf "${SAVANXP_READELF:-llvm-readelf}"
cp "$WORK/tcc.elf" "$STAGE/bin/tcc"

# The guest check behind the tinycc-smoke scenario, a SUN program.
"$ROOT/tools/build-user.sh" --source "$PORT/tests/tccsmoke.c" --name tccsmoke \
    --native --no-install --output "$WORK/tccsmoke.elf"
cp "$WORK/tccsmoke.elf" "$STAGE/bin/tccsmoke.sxe"

"$PYTHON" "$ROOT/tools/stage_licenses.py" stage --root "$ROOT" \
    --out "$STAGE/system/licenses" "$PORT/licenses/tinycc.notice"

if ((NO_INSTALL)); then
    echo "tinycc: built $WORK/tcc.elf and $TCCDIR"
    exit 0
fi

[[ ( -f "$IMAGE" || -b "$IMAGE" ) && -x "$SXFS_CLI" ]] || {
    echo "tinycc: the SxFS target or sxfs-cli is missing; run ./build.sh build first" >&2
    exit 1
}
"$PYTHON" "$ROOT/tools/sxfs_sync.py" --target "$IMAGE" --source "$STAGE" --cli "$SXFS_CLI"
echo "tinycc: installed /bin/tcc and /system/tcc"
