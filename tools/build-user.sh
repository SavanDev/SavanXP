#!/usr/bin/env bash
# Build and optionally install a freestanding POSIX user program.
# This is intentionally separate from the in-tree CMake userland registry.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SDK="$ROOT/subsystems/posix/sdk/v1"
BUILD_ROOT=${SAVANXP_OUTPUT_ROOT:-"$ROOT/build"}
EXTERNAL_ROOT="$BUILD_ROOT/external"
NAME=""
SOURCE=""
NO_INSTALL=0
NO_COMPACT=0
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
  --output PATH     override the output ELF path
  --destination PATH install under /disk (default: /disk/bin/NAME)
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
if [[ -z "$DESTINATION" ]]; then
    DESTINATION="/disk/bin/$NAME"
fi
case "$DESTINATION" in
    /disk/*) DESTINATION_RELATIVE=${DESTINATION#/disk/} ;;
    *)
        echo "build-user.sh: destination must be under /disk" >&2
        exit 2
        ;;
esac
[[ -n "$DESTINATION_RELATIVE" && "$DESTINATION_RELATIVE" != *..* && "$DESTINATION_RELATIVE" != /* ]] || {
    echo "build-user.sh: destination must name a file below /disk" >&2
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

compile_c "$SDK/runtime/libc.c" "$OBJ_ROOT/libc.o"
POSIX_EXTRA=()
if ((HEAP_MIB > 0)); then
    POSIX_EXTRA=("-DSX_HEAP_SIZE=(${HEAP_MIB}u*1024u*1024u)")
fi
compile_c "$SDK/runtime/posix.c" "$OBJ_ROOT/posix.o" "${POSIX_EXTRA[@]}"
compile_c "$SDK/runtime/gfx.c" "$OBJ_ROOT/gfx.o"
compile_c "$SDK/runtime/gfx2d.c" "$OBJ_ROOT/gfx2d.o"
compile_asm "$SDK/runtime/setjmp.S" "$OBJ_ROOT/setjmp.o"
compile_asm "$SDK/runtime/crt0.S" "$OBJ_ROOT/crt0.o"
RUNTIME_OBJECTS=("$OBJ_ROOT/crt0.o" "$OBJ_ROOT/libc.o" "$OBJ_ROOT/posix.o" "$OBJ_ROOT/gfx.o" "$OBJ_ROOT/gfx2d.o" "$OBJ_ROOT/setjmp.o")
if ((SSE)); then
    compile_c "$SDK/runtime/math.c" "$OBJ_ROOT/math.o" -fno-builtin
    RUNTIME_OBJECTS+=("$OBJ_ROOT/math.o")
fi
if ((AUDIO)); then
    compile_c "$SDK/runtime/audio.c" "$OBJ_ROOT/audio.o"
    RUNTIME_OBJECTS+=("$OBJ_ROOT/audio.o")
fi
if ((GUI)); then
    compile_c "$SDK/runtime/sxgui.c" "$OBJ_ROOT/sxgui.o"
    compile_c "$SDK/runtime/sxgui_app.c" "$OBJ_ROOT/sxgui_app.o"
    RUNTIME_OBJECTS+=("$OBJ_ROOT/sxgui.o" "$OBJ_ROOT/sxgui_app.o")
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
    SYNC_ARGS=(--image "$IMAGE" --source "$STAGE" --cli "$CLI" --sectors "${SAVANXP_SXFS_SECTORS:-131072}")
    ((NO_COMPACT == 0)) || SYNC_ARGS+=(--no-compact)
    "$PYTHON" "$ROOT/tools/sxfs_sync.py" "${SYNC_ARGS[@]}"
    echo "build-user.sh: installed $DESTINATION"
fi
echo "build-user.sh: ELF $RAW_OUTPUT"
