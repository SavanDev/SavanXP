#!/usr/bin/env bash
# Standalone optional Haxe -> native ELF build. This is intentionally not a
# dependency of the root CMake target.
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$HERE/../.." && pwd)"
SDK="$ROOT/subsystems/posix/sdk/v1"
NATIVE_SDK="$HERE/sdk"
UPSTREAM_FILE="$HERE/UPSTREAM"
OUTPUT_ROOT="${SAVANXP_OUTPUT_ROOT:-$ROOT/build}"
OUT_ROOT="${SAVANXP_NATIVE_BUILD_ROOT:-$OUTPUT_ROOT/native}"
DEPS_ROOT="${SAVANXP_HAXE_DEPS_ROOT:-$OUTPUT_ROOT/ports/haxe/deps}"
EXTERNAL_DIR="${SAVANXP_EXTERNAL_ROOT:-$OUTPUT_ROOT/external}"
IMAGE="${SAVANXP_DISK_IMAGE:-$OUTPUT_ROOT/disk.img}"
SXFS_CLI="${SAVANXP_SXFS_CLI:-$OUTPUT_ROOT/tools/sxfs-cli}"

NAME="nativehello"
SOURCE="haxe"
ALL=0
INSTALL=0
NO_COMPACT=0
NO_RESOURCES=0
FORCE=0

usage() {
    cat <<'EOF'
Usage: ./subsystems/native/build.sh [options]

Options:
  --name NAME          Build one application (default: nativehello).
  --source SOURCE      Source directory relative to subsystems/native (default: haxe).
  --all                Build nativehello, nativegui, and sxguiapp.
  --install            Install the resulting ELF(s) through the SxFS candidate flow.
  --no-install         Explicitly do not install (the default).
  --no-compact         Disable automatic SxFS compaction during installation.
  --no-resources       Do not generate/stamp SXE metadata.
  --force              Remove generated C++ and object directories first.
  -h, --help           Show this help.
EOF
}

while (($#)); do
    case "$1" in
        --name)
            (($# >= 2)) || { echo "native: --name requires a value" >&2; exit 2; }
            NAME=$2; shift 2 ;;
        --source)
            (($# >= 2)) || { echo "native: --source requires a value" >&2; exit 2; }
            SOURCE=$2; shift 2 ;;
        --all) ALL=1; shift ;;
        --install) INSTALL=1; shift ;;
        --no-install) INSTALL=0; shift ;;
        --no-compact) NO_COMPACT=1; shift ;;
        --no-resources) NO_RESOURCES=1; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "native: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

metadata() {
    sed -n "s/^$1=//p" "$UPSTREAM_FILE" | head -n 1
}

HAXE_VERSION=$(metadata haxe_version)
REFLAXE_URL=$(metadata reflaxe_repository)
REFLAXE_COMMIT=$(metadata reflaxe_commit)
REFLAXE_CPP_URL=$(metadata reflaxe_cpp_repository)
REFLAXE_CPP_COMMIT=$(metadata reflaxe_cpp_commit)
OSABI_HEX=$(sed -n 's/^#define SXN_ELF_OSABI_NATIVE[[:space:]]\{1,\}0x\([0-9A-Fa-f][0-9A-Fa-f]*\).*/\1/p' "$ROOT/include/abi/savanxp_native_abi.h" | head -n 1)

HAXE="${SAVANXP_HAXE:-haxe}"
CLANG="${SAVANXP_CLANG:-clang}"
CLANGXX="${SAVANXP_CLANGXX:-clang++}"
LD="${SAVANXP_LD:-ld.lld}"
PYTHON="${SAVANXP_PYTHON:-python3}"
OBJCOPY="${SAVANXP_OBJCOPY:-llvm-objcopy}"
READELF="${SAVANXP_READELF:-llvm-readelf}"

for tool in "$HAXE" "$CLANG" "$CLANGXX" "$LD" git; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "native: required tool not found: $tool" >&2
        exit 1
    }
done
command -v "$PYTHON" >/dev/null 2>&1 || {
    echo "native: Python not found: $PYTHON" >&2
    exit 1
}
if ((NO_RESOURCES == 0)); then
    for tool in "$OBJCOPY" "$READELF"; do
        command -v "$tool" >/dev/null 2>&1 || {
            echo "native: resource tool not found: $tool" >&2
            exit 1
        }
    done
    "$PYTHON" -c 'import PIL' >/dev/null 2>&1 || {
        echo "native: Python Pillow is required for SXE resources" >&2
        exit 1
    }
fi

haxe_version_output=$("$HAXE" -version 2>&1 | head -n 1)
if [[ "$haxe_version_output" != *"$HAXE_VERSION"* ]]; then
    echo "native: Haxe $HAXE_VERSION is required; found: $haxe_version_output" >&2
    exit 1
fi
[[ -n "$OSABI_HEX" ]] || {
    echo "native: SXN_ELF_OSABI_NATIVE is missing from the neutral ABI header" >&2
    exit 1
}

case "$OUT_ROOT$DEPS_ROOT" in
    *[[:space:]]*)
        echo "native: build and dependency paths must not contain whitespace" >&2
        exit 1
        ;;
esac
mkdir -p "$OUT_ROOT" "$DEPS_ROOT" "$EXTERNAL_DIR"

ensure_checkout() {
    local url=$1
    local commit=$2
    local path=$3
    if [[ ! -d "$path/.git" ]]; then
        rm -rf "$path"
        git clone "$url" "$path"
    fi
    if [[ "$(git -C "$path" rev-parse HEAD 2>/dev/null || true)" != "$commit" ]]; then
        git -C "$path" fetch --depth 1 origin "$commit"
        git -C "$path" checkout --quiet --detach "$commit"
    fi
    [[ -z "$(git -C "$path" status --porcelain)" ]] || {
        echo "native: dependency checkout is dirty: $path" >&2
        exit 1
    }
}

ensure_checkout "$REFLAXE_URL" "$REFLAXE_COMMIT" "$DEPS_ROOT/reflaxe"
ensure_checkout "$REFLAXE_CPP_URL" "$REFLAXE_CPP_COMMIT" "$DEPS_ROOT/reflaxe.CPP"

REFLAXE="$DEPS_ROOT/reflaxe"
REFLAXE_CPP="$DEPS_ROOT/reflaxe.CPP"
STD_CROSS="$OUT_ROOT/std-cross"
rm -rf "$STD_CROSS"
mkdir -p "$STD_CROSS"
std_source="$REFLAXE_CPP/std/cxx/_std"
while IFS= read -r -d '' source; do
    relative="${source#"$std_source/"}"
    target="$STD_CROSS/${relative%.hx}.cross.hx"
    mkdir -p "$(dirname "$target")"
    cp "$source" "$target"
done < <(find "$std_source" -type f -name '*.hx' -print0)
for source in "$HERE"/haxe-std-fixes/*.hx; do
    [[ -f "$source" ]] || continue
    cp "$source" "$STD_CROSS/$(basename "${source%.hx}").cross.hx"
done

CFLAGS=(
    -target x86_64-unknown-none-elf
    -ffreestanding
    -fstack-protector-strong
    -fno-pic
    -fno-pie
    -mno-red-zone
    -mcmodel=small
    -I "$NATIVE_SDK/include"
    -I "$SDK/include"
    -I "$ROOT/include"
)
CXXFLAGS=(
    "${CFLAGS[@]}"
    -std=c++17
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -fno-use-cxa-atexit
    -nostdinc++
    -isystem "$NATIVE_SDK/include/cxxstd"
    -include "$NATIVE_SDK/include/savanxp_native.h"
)

compile_c() {
    local source=$1 object=$2
    shift 2
    "$CLANG" -c -x c "$source" -o "$object" "${CFLAGS[@]}" "$@"
}

compile_cxx() {
    local source=$1 object=$2
    shift 2
    "$CLANGXX" -c "$source" -o "$object" "${CXXFLAGS[@]}" "$@"
}

build_one() {
    local name=$1
    local source_name=$2
    local source_dir="$HERE/$source_name"
    [[ -d "$source_dir" ]] || {
        echo "native: source directory does not exist: $source_dir" >&2
        return 1
    }
    [[ "$source_name" != /* && "$source_name" != *..* ]] || {
        echo "native: source must be a safe relative directory" >&2
        return 1
    }

    local gen_dir="$OUT_ROOT/gen-$name"
    local obj_dir="$OUT_ROOT/obj-$name"
    if ((FORCE)); then
        rm -rf "$gen_dir" "$obj_dir" "$OUT_ROOT/sxe-$name" "$OUT_ROOT/$name" "$OUT_ROOT/$name.elf"
    fi
    mkdir -p "$gen_dir" "$obj_dir"
    local hxml="$OUT_ROOT/generated-$name.hxml"
    cat > "$hxml" <<EOF
--no-opt
-cp $source_dir
-cp $HERE/haxe-toolkit
-cp $HERE/haxe-support
-cp $REFLAXE/src
-cp $REFLAXE_CPP/src
-cp $REFLAXE_CPP/std
-cp $STD_CROSS
-D cxx
-D reflaxe.cpp
-D retain-untyped-meta
-D cpp-output=$gen_dir
--macro nullSafety("reflaxe")
--macro reflaxe.ReflectCompiler.Start()
--macro nullSafety("cxxcompiler")
--macro SxnCompilerInit.Start()
-main Main
EOF
    echo "==> generating $name"
    "$HAXE" "$hxml"
    [[ -f "$gen_dir/src/Main.cpp" ]] || {
        echo "native: Haxe did not generate $gen_dir/src/Main.cpp" >&2
        return 1
    }

    local crt0="$obj_dir/crt0.o"
    local objects=()
    "$CLANG" -c "$SDK/runtime/crt0.S" -o "$crt0" "${CFLAGS[@]}"
    objects+=("$crt0")
    local runtime_c=(sx_native.c sx_gui.c sx_text.c sx_sysinfo.c sx_fs.c)
    for unit in "${runtime_c[@]}"; do
        object="$obj_dir/${unit%.c}.o"
        compile_c "$NATIVE_SDK/runtime/$unit" "$object"
        objects+=("$object")
    done
    local runtime_cxx=(sx_cxx.cpp sx_entry.cpp)
    for unit in "${runtime_cxx[@]}"; do
        object="$obj_dir/${unit%.cpp}.o"
        compile_cxx "$NATIVE_SDK/runtime/$unit" "$object" -I "$gen_dir/include"
        objects+=("$object")
    done

    local index=0
    while IFS= read -r -d '' source; do
        [[ "$(basename "$source")" == "_main_.cpp" ]] && continue
        object="$obj_dir/generated-$index.o"
        compile_cxx "$source" "$object" -I "$gen_dir/include"
        objects+=("$object")
        index=$((index + 1))
    done < <(find "$gen_dir/src" -type f -name '*.cpp' -print0 | sort -z)

    local elf="$OUT_ROOT/$name.elf"
    "$LD" -nostdlib -static -T "$SDK/linker.ld" -o "$elf" "${objects[@]}"
    "$PYTHON" - "$elf" "$OSABI_HEX" <<'PY'
import sys
from pathlib import Path
path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
if len(data) <= 7:
    raise SystemExit("native: generated ELF is too small")
data[7] = int(sys.argv[2], 16)
path.write_bytes(data)
PY
    mkdir -p "$EXTERNAL_DIR"
    cp "$elf" "$EXTERNAL_DIR/$name.elf"

    local stamped="$OUT_ROOT/$name"
    if ((NO_RESOURCES)); then
        cp "$elf" "$stamped"
    else
        local resources="$OUT_ROOT/sxe-$name"
        rm -rf "$resources"
        mkdir -p "$resources"
        local build_id
        build_id=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)
        [[ -n "$build_id" ]] || build_id=unknown
        "$PYTHON" "$ROOT/tools/gen_sxe_resources.py" \
            --project-root "$ROOT" \
            --manifest-dir "$source_dir" \
            --output-dir "$resources" \
            --build-id "$build_id" \
            --program "$name"
        cp "$elf" "$stamped"
        "$PYTHON" "$ROOT/tools/stamp_sxe.py" \
            --binary "$stamped" \
            --name "$name" \
            --resource-dir "$resources" \
            --objcopy "$OBJCOPY" \
            --readelf "$READELF"
    fi

    if ((INSTALL)); then
        [[ -f "$IMAGE" && -x "$SXFS_CLI" ]] || {
            echo "native: ./build.sh build and the sxfs-cli target are required for install" >&2
            return 1
        }
        local stage="$OUTPUT_ROOT/ports/haxe/install-stage-$name"
        rm -rf "$stage"
        mkdir -p "$stage/bin"
        cp "$stamped" "$stage/bin/$name"
        SYNC_ARGS=(
            --image "$IMAGE"
            --source "$stage"
            --cli "$SXFS_CLI"
            --sectors "${SAVANXP_SXFS_SECTORS:-131072}"
        )
        if ((NO_COMPACT)); then
            SYNC_ARGS+=(--no-compact)
        fi
        "$PYTHON" "$ROOT/tools/sxfs_sync.py" "${SYNC_ARGS[@]}"
        echo "native: installed /disk/bin/$name"
    fi
    echo "native: generated $elf (OSABI=0x$OSABI_HEX)"
}

if ((ALL)); then
    build_one nativehello haxe
    build_one nativegui haxe-gui
    build_one sxguiapp haxe-sxgui
else
    build_one "$NAME" "$SOURCE"
fi
