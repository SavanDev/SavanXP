#!/usr/bin/env bash
# Assemble the SDK sysroot, build/sdk/<MAJOR.MINOR>: the public headers, the
# runtime prebuilt in its two variants, the linker script and the tools that
# build a program against it (docs/roadmaps/done/SDK_V2_ROADMAP.md, phase 2).
#
# ./build.sh build runs it, and tools/build-user.sh runs it before every build,
# so the sysroot never lags behind the sources. It rebuilds only when one of
# its inputs is newer than the last build: the runtime's sources and every
# header they include, the SDK's headers, the header allowlist and the tools.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK_SRC="$ROOT/subsystems/posix/sdk/v1"
# shellcheck source=lib/sdk_flags.sh
. "$ROOT/tools/lib/sdk_flags.sh"
# shellcheck source=lib/runtime_units.sh
. "$ROOT/tools/lib/runtime_units.sh"
BUILD_ROOT=$(realpath -m -- "${SAVANXP_OUTPUT_ROOT:-"$ROOT/build"}")
CC=${SAVANXP_CLANG:-clang}
AR=${SAVANXP_AR:-llvm-ar}
QUIET=0
FORCE=0

usage() {
    cat <<'EOF'
Usage: tools/make-sdk.sh [--quiet] [--force] [--print-path]

  --quiet       print nothing when the sysroot is already up to date
  --force       rebuild even if nothing changed
  --print-path  print the sysroot's path and exit
EOF
}

VERSION=$(savanxp_sdk_version "$ROOT/include/abi/savanxp_abi.h")
SDK_PARENT="$BUILD_ROOT/sdk"
OUT="$SDK_PARENT/$VERSION"

while (($#)); do
    case "$1" in
        --quiet) QUIET=1; shift ;;
        --force) FORCE=1; shift ;;
        --print-path) printf '%s\n' "$OUT"; exit 0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "make-sdk.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

for tool in "$CC" "$AR"; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "make-sdk.sh: required tool not found: $tool" >&2
        exit 1
    }
done

mkdir -p "$SDK_PARENT"
# One assembler at a time: the smoke scenarios and the host tests may run
# build-user.sh while ./build.sh build is assembling.
exec 9>"$SDK_PARENT/.lock"
flock 9

# Up to date when every input recorded by the last build still exists and
# none is newer than its stamp. The input directories are recorded too, so a
# header added to one of them also triggers a rebuild.
if ((FORCE == 0)) && [[ -f "$OUT/.stamp" && -f "$OUT/.inputs" ]]; then
    stale=0
    while IFS= read -r input; do
        if [[ ! -e "$input" || "$input" -nt "$OUT/.stamp" ]]; then
            stale=1
            break
        fi
    done <"$OUT/.inputs"
    if ((stale == 0)); then
        ((QUIET)) || echo "make-sdk.sh: $OUT is up to date"
        exit 0
    fi
fi

TMP="$SDK_PARENT/.$VERSION.tmp.$$"
cleanup() { rm -rf -- "$TMP"; }
trap cleanup EXIT
rm -rf -- "$TMP"
mkdir -p "$TMP"
# The stamp is taken before reading any input, so a source edited while this
# runs is newer than the stamp and the next run rebuilds.
touch "$TMP/.stamp"
INPUTS=("${BASH_SOURCE[0]}")

# --- headers ---------------------------------------------------------------
mkdir -p "$TMP/include"
cp -R "$SDK_SRC/include/." "$TMP/include/"
while IFS= read -r -d '' entry; do
    INPUTS+=("$entry")
done < <(find "$SDK_SRC/include" -print0)
ALLOWLIST="$SDK_SRC/public-headers.txt"
INPUTS+=("$ALLOWLIST" "$ROOT/include")
while read -r header; do
    [[ -n "$header" ]] || continue
    source_header="$ROOT/include/$header"
    [[ -f "$source_header" ]] || {
        echo "make-sdk.sh: $ALLOWLIST names a missing header: include/$header" >&2
        exit 1
    }
    [[ ! -e "$TMP/include/$header" ]] || {
        echo "make-sdk.sh: include/$header collides with an SDK header" >&2
        exit 1
    }
    mkdir -p "$TMP/include/$(dirname -- "$header")"
    cp "$source_header" "$TMP/include/$header"
    INPUTS+=("$source_header" "$(dirname -- "$source_header")")
done < <(sed -e 's/#.*//' -e 's/[[:space:]]*$//' "$ALLOWLIST")

cp "$SDK_SRC/linker.ld" "$TMP/linker.ld"
INPUTS+=("$SDK_SRC/linker.ld")
printf '%s\n' "$VERSION" >"$TMP/VERSION"

# --- runtime ---------------------------------------------------------------
# Every unit of the groups a variant needs, in manifest order, compiled with
# the flags build-user.sh gives a program. lib/<variant>/units.txt keeps the
# order and the group of each object, and build-user.sh links them in it.
# The SUN runtime's units follow the POSIX ones, their groups prefixed sun-.
MANIFEST="$SDK_SRC/runtime/units.txt"
SUN_MANIFEST="$ROOT/system/runtime/units.txt"
INPUTS+=("$MANIFEST" "$SUN_MANIFEST")
DEPS="$TMP/.deps"
mkdir -p "$DEPS"

# compile_manifest MANIFEST PREFIX LIB VARIANT GROUP...
# Compiles the manifest's units in the named groups into LIB, appending
# "<PREFIX><group> <object>" to LIB/units.txt. Uses the caller's flags array.
compile_manifest() {
    local manifest=$1 prefix=$2 lib=$3 variant=$4
    shift 4
    local group path name wanted unit object depfile
    while read -r group path; do
        wanted=0
        for name in "$@"; do
            [[ "$group" == "$name" ]] && { wanted=1; break; }
        done
        ((wanted)) || continue
        unit="$ROOT/$path"
        object=$(savanxp_runtime_object "$lib" "$unit")
        [[ ! -e "$object" ]] || {
            echo "make-sdk.sh: two runtime units compile to $(basename -- "$object")" >&2
            exit 1
        }
        depfile="$DEPS/$variant-$(basename -- "$object" .o).d"
        case "${unit##*/}" in
            *.S) "$CC" -c "$unit" -o "$object" "${flags[@]}" -MD -MF "$depfile" ;;
            math.c) "$CC" -c -x c "$unit" -o "$object" "${flags[@]}" -fno-builtin -MD -MF "$depfile" ;;
            *) "$CC" -c -x c "$unit" -o "$object" "${flags[@]}" -MD -MF "$depfile" ;;
        esac
        printf '%s%s %s\n' "$prefix" "$group" "$(basename -- "$object")" >>"$lib/units.txt"
    done < <(grep -v '^[[:space:]]*\(#\|$\)' "$manifest")
}

for variant_spec in "0 0" "1 0" "1 1"; do
    read -r sse pie <<<"$variant_spec"
    variant=$(savanxp_sdk_variant "$sse" "$pie")
    lib="$TMP/lib/$variant"
    mkdir -p "$lib"
    if ((pie)); then
        mapfile -t flags < <(savanxp_sdk_pie_compile_flags)
    else
        mapfile -t flags < <(savanxp_sdk_compile_flags "$sse")
    fi
    flags+=(-I "$SDK_SRC/include" -I "$ROOT/include")
    mapfile -t groups < <(savanxp_sdk_groups "$sse" "$pie")
    : >"$lib/units.txt"
    compile_manifest "$MANIFEST" "" "$lib" "$variant" "${groups[@]}"
    compile_manifest "$SUN_MANIFEST" "sun-" "$lib" "$variant" core gui
    if ((pie)); then
        # The loader goes into every PIE program: crt0 runs it through the
        # weak sx_run_interpreter hook (docs/kernel/SHARED_LIBRARIES.md).
        # <dlfcn.h> is POSIX's layer over it, for POSIX programs only.
        for loader_unit in "loader system/loader/ldso.c" "dlfcn subsystems/posix/userland/dlfcn.c"; do
            read -r group path <<<"$loader_unit"
            object="$lib/$(basename -- "$path" .c).o"
            "$CC" -c -x c "$ROOT/$path" -o "$object" "${flags[@]}" \
                -MD -MF "$DEPS/$variant-$(basename -- "$path" .c).d"
            printf '%s %s\n' "$group" "$(basename -- "$object")" >>"$lib/units.txt"
        done
    fi
done

# --- archives ----------------------------------------------------------------
# For build systems that link with -l (tools/sxcc, the CMake toolchain file),
# each variant also has the groups as ordinary archives, which take only the
# objects a program references. build-user.sh keeps linking the objects
# themselves. What must be linked whether or not anything references it stays
# a loose object: crt0.o, which starts the program; sxn_runtime.o, whose heap
# page source replaces the POSIX runtime's weak one; and ldso.o, which crt0
# finds through a weak hook.
#
#   libsavanxp.a   core (without crt0), math and setjmp: what every program needs
#   libm.a         math alone, for builds that add -lm (empty without SSE)
#   libsxgui.a     gui, libsxaudio.a audio, libsxmidi.a midi, libsxgl.a gl
#   libsxn.a       sun-core: SUN's profile and settings path (sxnprofile.h)
#                  DNS resolver (sxndns.h) and HTTP loader (sxnhttp.h)
#   libsxngui.a    sun-gui: SUN's application frame and file dialog
#   libdl.a        dlfcn, in the PIE variant
make_archive() {
    local lib=$1 archive=$2
    shift 2
    local group object wanted
    local -a members=()
    while read -r group object; do
        [[ "$object" != crt0.o ]] || continue
        for wanted in "$@"; do
            if [[ "$group" == "$wanted" ]]; then
                members+=("$lib/$object")
                break
            fi
        done
    done <"$lib/units.txt"
    rm -f "$lib/$archive"
    "$AR" rcsD "$lib/$archive" "${members[@]}"
}
for variant in nosse sse pie; do
    lib="$TMP/lib/$variant"
    make_archive "$lib" libsavanxp.a core math setjmp
    make_archive "$lib" libm.a math
    make_archive "$lib" libsxgui.a gui
    make_archive "$lib" libsxaudio.a audio
    make_archive "$lib" libsxmidi.a midi
    make_archive "$lib" libsxgl.a gl
    make_archive "$lib" libsxn.a sun-core
    make_archive "$lib" libsxngui.a sun-gui
done
make_archive "$TMP/lib/pie" libdl.a dlfcn

# --- shared libraries ------------------------------------------------------
# The system's libraries a PIE program may link against (--needs), copied from
# the build, and lib/shared/libraries.txt: each soname and the runtime objects
# it replaces, which build-user.sh leaves out of a program that links it.
LIBRARIES="$SDK_SRC/runtime/libraries.txt"
INPUTS+=("$LIBRARIES")
SHARED="$TMP/lib/shared"
mkdir -p "$SHARED"
: >"$SHARED/libraries.txt"
while read -r name soname units; do
    built="$BUILD_ROOT/diskfs/lib/$soname"
    [[ -f "$built" ]] || {
        echo "make-sdk.sh: $soname is not built yet ($built); run ./build.sh build" >&2
        exit 1
    }
    cp "$built" "$SHARED/$soname"
    INPUTS+=("$built")
    objects=""
    for unit in $units; do
        objects+=" $(basename -- "${unit%.*}").o"
    done
    printf '%s%s\n' "$soname" "$objects" >>"$SHARED/libraries.txt"
done < <(grep -v '^[[:space:]]*\(#\|$\)' "$LIBRARIES")

# --- tools -----------------------------------------------------------------
SHARE="$TMP/share/sdk"
mkdir -p "$SHARE/lib" "$SHARE/icons"
for tool in build-user.sh sxcc savanxp-toolchain.cmake gen_sxe_resources.py stamp_sxe.py stamp_osabi.py sxfs_sync.py; do
    cp "$ROOT/tools/$tool" "$SHARE/$tool"
    INPUTS+=("$ROOT/tools/$tool")
done
cp "$ROOT/tools/lib/sdk_flags.sh" "$SHARE/lib/sdk_flags.sh"
INPUTS+=("$ROOT/tools/lib/sdk_flags.sh" "$ROOT/tools/lib/runtime_units.sh")
# The system icon catalogue a .sxres names with icon=.
for size in 16x16 32x32; do
    cp -R "$ROOT/assets/desktop/icons/$size" "$SHARE/icons/$size"
    while IFS= read -r -d '' entry; do
        INPUTS+=("$entry")
    done < <(find "$ROOT/assets/desktop/icons/$size" -print0)
done

# --- record the inputs and publish -----------------------------------------
# Every file a compile read, from its dependency file, joins the inputs.
while read -r dep; do
    INPUTS+=("$dep")
done < <(cat "$DEPS"/*.d | tr ' \\' '\n\n' | sed -e '/^$/d' -e '/:$/d' | sort -u)
rm -rf -- "$DEPS"
printf '%s\n' "${INPUTS[@]}" | sort -u >"$TMP/.inputs"

rm -rf -- "$OUT.old"
[[ ! -d "$OUT" ]] || mv -- "$OUT" "$OUT.old"
mv -- "$TMP" "$OUT"
rm -rf -- "$OUT.old"
((QUIET)) || echo "make-sdk.sh: built $OUT"
