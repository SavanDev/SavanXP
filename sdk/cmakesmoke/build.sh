#!/usr/bin/env bash
# Build sdk/cmakesmoke with CMake and the SDK's toolchain file, stamp its SXE
# resources with the sysroot's tools, and install it as /bin/cmakesmoke.
#
#   sdk/cmakesmoke/build.sh [--no-install]
#
# The steps are the ones any CMake project takes against the SDK: configure
# with share/sdk/savanxp-toolchain.cmake, build, then stamp the resources as
# build-user.sh does.
set -euo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$HERE/../.." && pwd)
OUTPUT_ROOT=$(realpath -m -- "${SAVANXP_OUTPUT_ROOT:-$ROOT/build}")
PYTHON=${SAVANXP_PYTHON:-python3}
NO_INSTALL=0
[[ "${1:-}" != --no-install ]] || NO_INSTALL=1

SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$ROOT/tools/make-sdk.sh" --quiet
SYSROOT=$(SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$ROOT/tools/make-sdk.sh" --print-path)
TOOLS="$SYSROOT/share/sdk"
BUILD="$OUTPUT_ROOT/external/cmakesmoke"

cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLS/savanxp-toolchain.cmake"
cmake --build "$BUILD"

ELF="$BUILD/cmakesmoke"
RESOURCES="$BUILD/sxe"
mkdir -p "$RESOURCES"
"$PYTHON" "$TOOLS/gen_sxe_resources.py" --project-root "$SYSROOT" --icon-root "$TOOLS/icons" \
    --manifest-dir "$HERE" --output-dir "$RESOURCES" --program cmakesmoke
"$PYTHON" "$TOOLS/stamp_sxe.py" --binary "$ELF" --name cmakesmoke --resource-dir "$RESOURCES" \
    --objcopy "${SAVANXP_OBJCOPY:-llvm-objcopy}" --readelf "${SAVANXP_READELF:-llvm-readelf}"
echo "cmakesmoke: ELF $ELF"
((NO_INSTALL == 0)) || exit 0

STAGE="$OUTPUT_ROOT/external/install-stage-cmakesmoke"
rm -rf -- "$STAGE"
mkdir -p "$STAGE/bin"
cp "$ELF" "$STAGE/bin/cmakesmoke"
"$PYTHON" "$TOOLS/sxfs_sync.py" --image "${SAVANXP_DISK_IMAGE:-$OUTPUT_ROOT/disk.img}" --source "$STAGE" \
    --cli "${SAVANXP_SXFS_CLI:-$OUTPUT_ROOT/tools/sxfs-cli}" --sectors "${SAVANXP_SXFS_SECTORS:-2097152}"
rm -rf -- "$STAGE"
echo "cmakesmoke: installed /bin/cmakesmoke"
