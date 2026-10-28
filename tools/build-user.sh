#!/usr/bin/env bash
# Build and optionally install a freestanding user program (POSIX by default,
# native Sx with --native, which stamps EI_OSABI from the neutral ABI header).
# This is intentionally separate from the in-tree CMake userland registry.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK="$ROOT/subsystems/posix/sdk/v1"
# shellcheck source=lib/runtime_units.sh
. "$ROOT/tools/lib/runtime_units.sh"
BUILD_ROOT=${SAVANXP_OUTPUT_ROOT:-"$ROOT/build"}
EXTERNAL_ROOT="$BUILD_ROOT/external"
NAME=""
SOURCE=""
NO_INSTALL=0
NO_COMPACT=0
NATIVE=0
SSE=0
GUI=0
AUDIO=0
HEAP_MIB=0
OUTPUT_PATH=""
DESTINATION=""
CC=${SAVANXP_CLANG:-clang}
LD=${SAVANXP_LD:-ld.lld}
PYTHON=${SAVANXP_PYTHON:-python3}
OBJCOPY=${SAVANXP_OBJCOPY:-llvm-objcopy}
READELF=${SAVANXP_READELF:-llvm-readelf}

usage() {
    cat <<'EOF'
Usage: tools/build-user.sh --source PATH --name NAME [options]

Options:
  --sse             compile the external ABI with SSE/SSE2 enabled
  --gui             link the SXGUI runtime
  --audio           link the PCM audio runtime
  --heap-mib N      set the fixed heap size in MiB
  --no-install      build only
  --no-compact      pass --no-compact to SxFS installation
  --native          build a SUN program: stamp EI_OSABI as a native subsystem
                    binary and install it as NAME.sxe
  --output PATH     override the output ELF path
  --destination PATH install on the disk (default: /bin/NAME, or
                    /bin/NAME.sxe with --native)
EOF
}

while (($#)); do
    case "$1" in
        --source) SOURCE=${2:?--source requires a value}; shift 2 ;;
        --name) NAME=${2:?--name requires a value}; shift 2 ;;
        --sse) SSE=1; shift ;;
        --gui) GUI=1; shift ;;
        --audio) AUDIO=1; shift ;;
        --heap-mib) HEAP_MIB=${2:?--heap-mib requires a value}; shift 2 ;;
        --no-install) NO_INSTALL=1; shift ;;
        --no-compact) NO_COMPACT=1; shift ;;
        --native) NATIVE=1; shift ;;
        --output) OUTPUT_PATH=${2:?--output requires a value}; shift 2 ;;
        --destination) DESTINATION=${2:?--destination requires a value}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "build-user.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done
[[ -n "$SOURCE" ]] || { echo "build-user.sh: --source is required" >&2; exit 2; }
SOURCE=$(realpath "$SOURCE")
[[ -e "$SOURCE" ]] || { echo "build-user.sh: source does not exist: $SOURCE" >&2; exit 2; }
if [[ -z "$NAME" ]]; then
    NAME=$(basename "$SOURCE")
    [[ "$NAME" != *.c && "$NAME" != *.S ]] || NAME=${NAME%.*}
fi
[[ -n "$NAME" ]] || { echo "build-user.sh: --name is required" >&2; exit 2; }
[[ "$NAME" != */* && "$NAME" != *..* ]] || {
    echo "build-user.sh: name must be a simple file name" >&2
    exit 2
}
# A SUN program is <name>.sxe on disk and a POSIX one has no extension
# (docs/userland/SXE_FORMAT.md, "SUN programs are named .sxe"). NAME is the
# program's name, which finds its .sxres, so it never keeps the extension.
if ((NATIVE)); then
    NAME=${NAME%.sxe}
    [[ -n "$NAME" ]] || { echo "build-user.sh: --name is required" >&2; exit 2; }
elif [[ "$NAME" == *.sxe ]]; then
    echo "build-user.sh: .sxe names a SUN program; build it with --native" >&2
    exit 2
fi
if [[ -z "$DESTINATION" ]]; then
    if ((NATIVE)); then
        DESTINATION="/bin/$NAME.sxe"
    else
        DESTINATION="/bin/$NAME"
    fi
fi
if ((NATIVE)) && [[ "$DESTINATION" != *.sxe ]]; then
    echo "build-user.sh: a SUN program installs as <name>.sxe, not $DESTINATION" >&2
    exit 2
fi
if ((NATIVE == 0)) && [[ "$DESTINATION" == *.sxe ]]; then
    echo "build-user.sh: .sxe names a SUN program; build it with --native" >&2
    exit 2
fi
# The disk is the guest's root. /disk/... is still accepted, as the guest
# accepts it, and /dev and /rescue are the in-memory tree, not the disk.
case "$DESTINATION" in
    /disk/*) DESTINATION_RELATIVE=${DESTINATION#/disk/} ;;
    /dev|/dev/*|/rescue|/rescue/*)
        echo "build-user.sh: $DESTINATION is in memory, not on the disk" >&2
        exit 2
        ;;
    /*) DESTINATION_RELATIVE=${DESTINATION#/} ;;
    *)
        echo "build-user.sh: destination must be an absolute path" >&2
        exit 2
        ;;
esac
[[ -n "$DESTINATION_RELATIVE" && "$DESTINATION_RELATIVE" != *..* && "$DESTINATION_RELATIVE" != /* ]] || {
    echo "build-user.sh: destination must name a file on the disk" >&2
    exit 2
}

for tool in "$CC" "$LD" "$PYTHON" "$OBJCOPY" "$READELF"; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "build-user.sh: required tool not found: $tool" >&2
        exit 1
    }
done
"$PYTHON" -c 'import PIL' >/dev/null 2>&1 || {
    echo "build-user.sh: Python Pillow is required" >&2
    exit 1
}

OBJ_ROOT="$BUILD_ROOT/sdk/$NAME"
RAW_OUTPUT=${OUTPUT_PATH:-"$EXTERNAL_ROOT/$NAME.elf"}
mkdir -p "$OBJ_ROOT" "$EXTERNAL_ROOT" "$(dirname "$RAW_OUTPUT")"

SOURCE_ROOT="$SOURCE"
SOURCE_FILES=()
if [[ -f "$SOURCE" ]]; then
    SOURCE_ROOT=$(dirname "$SOURCE")
    SOURCE_FILES=("$SOURCE")
else
    mapfile -t SOURCE_FILES < <(find "$SOURCE" -type f \( -name '*.c' -o -name '*.S' \) -print | sort)
    if [[ -f "$SOURCE/compile-exclude.txt" ]]; then
        mapfile -t EXCLUDED < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$SOURCE/compile-exclude.txt")
        filtered=()
        for file in "${SOURCE_FILES[@]}"; do
            skip=0
            for relative in "${EXCLUDED[@]}"; do
                [[ "$file" == "$SOURCE/$relative" ]] && { skip=1; break; }
            done
            ((skip == 0)) && filtered+=("$file")
        done
        SOURCE_FILES=("${filtered[@]}")
    fi
fi
((${#SOURCE_FILES[@]} > 0)) || {
    echo "build-user.sh: no .c or .S sources found in $SOURCE" >&2
    exit 1
}

COMMON_FLAGS=(
    -target x86_64-unknown-none-elf
    -ffreestanding
    -fstack-protector-strong
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mcmodel=small
    -mno-mmx
    -Wall
    -Wextra
    -Wpedantic
    -Wno-language-extension-token
    -Wno-c23-extensions
    -I "$SOURCE_ROOT"
    -I "$SDK/include"
    -I "$ROOT/include"
    -I "$ROOT/subsystems/posix/userland"
    -I "$BUILD_ROOT/generated"
)
if ((SSE)); then
    COMMON_FLAGS+=(-msse -msse2)
else
    COMMON_FLAGS+=(-mno-sse -mno-sse2 -mgeneral-regs-only)
fi
if [[ -d "$SOURCE_ROOT/include" ]]; then
    COMMON_FLAGS+=(-I "$SOURCE_ROOT/include")
fi

OBJECTS=()
compile_c() {
    local source=$1 object=$2
    shift 2
    "$CC" -c -x c "$source" -o "$object" "${COMMON_FLAGS[@]}" "$@"
}
compile_asm() {
    local source=$1 object=$2
    "$CC" -c "$source" -o "$object" "${COMMON_FLAGS[@]}"
}

index=0
for source in "${SOURCE_FILES[@]}"; do
    object="$OBJ_ROOT/app-$index.o"
    if [[ "$source" == *.S ]]; then
        compile_asm "$source" "$object"
    else
        compile_c "$source" "$object"
    fi
    OBJECTS+=("$object")
    index=$((index + 1))
done

# The runtime's units come from its manifest
# (subsystems/posix/sdk/v1/runtime/units.txt); this script names groups only.
RUNTIME_GROUPS=(core setjmp)
((SSE)) && RUNTIME_GROUPS+=(math)
((AUDIO)) && RUNTIME_GROUPS+=(audio)
((GUI)) && RUNTIME_GROUPS+=(gui)
RUNTIME_UNITS_TEXT=$(savanxp_runtime_units "$ROOT" "${RUNTIME_GROUPS[@]}")
mapfile -t RUNTIME_UNITS <<<"$RUNTIME_UNITS_TEXT"
RUNTIME_OBJECTS=()
for unit in "${RUNTIME_UNITS[@]}"; do
    object=$(savanxp_runtime_object "$OBJ_ROOT" "$unit")
    case "${unit##*/}" in
        *.S) compile_asm "$unit" "$object" ;;
        posix.c)
            if ((HEAP_MIB > 0)); then
                compile_c "$unit" "$object" "-DSX_HEAP_SIZE=(${HEAP_MIB}u*1024u*1024u)"
            else
                compile_c "$unit" "$object"
            fi
            ;;
        math.c) compile_c "$unit" "$object" -fno-builtin ;;
        *) compile_c "$unit" "$object" ;;
    esac
    RUNTIME_OBJECTS+=("$object")
done
if ((NATIVE)); then
    # The native runtime (docs/userland/NATIVE_HEAP.md): the heap's page source
    # on SXN, replacing the runtime's weak POSIX one.
    compile_c "$ROOT/system/runtime/sxn_runtime.c" "$OBJ_ROOT/sxn_runtime.o"
    RUNTIME_OBJECTS+=("$OBJ_ROOT/sxn_runtime.o")
fi

"$LD" -nostdlib -static -T "$SDK/linker.ld" \
    -z max-page-size=0x1000 --build-id=none \
    -o "$RAW_OUTPUT" "${RUNTIME_OBJECTS[@]}" "${OBJECTS[@]}"

RESOURCE_ROOT="$OBJ_ROOT/sxe"
rm -rf "$RESOURCE_ROOT"
mkdir -p "$RESOURCE_ROOT"
BUILD_ID=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)
[[ -n "$BUILD_ID" ]] || BUILD_ID=unknown
"$PYTHON" "$ROOT/tools/gen_sxe_resources.py" \
    --project-root "$ROOT" \
    --manifest-dir "$SOURCE_ROOT" \
    --output-dir "$RESOURCE_ROOT" \
    --build-id "$BUILD_ID" \
    --program "$NAME"
"$PYTHON" "$ROOT/tools/stamp_sxe.py" \
    --binary "$RAW_OUTPUT" \
    --name "$NAME" \
    --resource-dir "$RESOURCE_ROOT" \
    --objcopy "$OBJCOPY" \
    --readelf "$READELF"
if ((NATIVE)); then
    # Last, as in the CMake build, so nothing that rewrites the ELF afterwards
    # can drop the mark. It also refuses a SUN program without .sxmeta or .sxe.
    "$PYTHON" "$ROOT/tools/stamp_osabi.py" \
        --binary "$RAW_OUTPUT" \
        --abi-header "$ROOT/include/abi/savanxp_native_abi.h" \
        --installed-name "$(basename -- "$DESTINATION")"
fi
if [[ "$RAW_OUTPUT" != "$EXTERNAL_ROOT/$NAME.elf" ]]; then
    cp "$RAW_OUTPUT" "$EXTERNAL_ROOT/$NAME.elf"
fi

if ((NO_INSTALL == 0)); then
    IMAGE=${SAVANXP_DISK_IMAGE:-"$BUILD_ROOT/disk.img"}
    CLI=${SAVANXP_SXFS_CLI:-"$BUILD_ROOT/tools/sxfs-cli"}
    [[ -f "$IMAGE" && -x "$CLI" ]] || {
        echo "build-user.sh: valid disk.img and sxfs-cli are required for install" >&2
        exit 1
    }
    STAGE="$BUILD_ROOT/external/install-stage-$NAME"
    rm -rf "$STAGE"
    mkdir -p "$STAGE/$(dirname -- "$DESTINATION_RELATIVE")"
    cp "$RAW_OUTPUT" "$STAGE/$DESTINATION_RELATIVE"
    SYNC_ARGS=(--image "$IMAGE" --source "$STAGE" --cli "$CLI" --sectors "${SAVANXP_SXFS_SECTORS:-2097152}")
    ((NO_COMPACT == 0)) || SYNC_ARGS+=(--no-compact)
    "$PYTHON" "$ROOT/tools/sxfs_sync.py" "${SYNC_ARGS[@]}"
    echo "build-user.sh: installed $DESTINATION"
fi
echo "build-user.sh: ELF $RAW_OUTPUT"
