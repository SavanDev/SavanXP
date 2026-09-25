#!/usr/bin/env bash
# Verify that a normal Linux build preserves an installed Doom port and data.
# The default WAD is a deterministic fixture; pass --wad for a real IWAD.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SOURCE_ROOT="${SAVANXP_OUTPUT_ROOT:-$ROOT/build}"
case "$SOURCE_ROOT" in
    /*) ;;
    *) SOURCE_ROOT="$ROOT/$SOURCE_ROOT" ;;
esac
PYTHON="${SAVANXP_PYTHON:-python3}"
WAD_PATH=""
JOBS="${SAVANXP_JOBS:-2}"

usage() {
    cat <<'EOF'
Usage: tools/verify_doom_persistence.sh [options]

Options:
  --wad PATH   Use a real WAD instead of the deterministic fixture.
  --jobs N     Parallelism for the isolated base build (default: 2).
  -h, --help   Show this help.
EOF
}

while (($#)); do
    case "$1" in
        --wad)
            (($# >= 2)) || { echo "verify-doom: --wad requires a value" >&2; exit 2; }
            WAD_PATH=$2
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || { echo "verify-doom: --jobs requires a value" >&2; exit 2; }
            JOBS=$2
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "verify-doom: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

[[ -f "$SOURCE_ROOT/disk.img" ]] || {
    echo "verify-doom: missing $SOURCE_ROOT/disk.img; run ./build.sh build first" >&2
    exit 1
}
[[ -x "$SOURCE_ROOT/tools/sxfs-cli" ]] || {
    echo "verify-doom: missing sxfs-cli; run ./build.sh build first" >&2
    exit 1
}
command -v flock >/dev/null 2>&1 || {
    echo "verify-doom: flock was not found in PATH" >&2
    exit 1
}
exec 8>"$SOURCE_ROOT/disk.img.lock"
flock -n 8 || {
    echo "verify-doom: persistent image is busy" >&2
    exit 1
}

TEMP=$(mktemp -d /tmp/opencode/doom-persistence.XXXXXX)
cleanup() {
    local status=$?
    trap - EXIT
    rm -rf "$TEMP"
    exit "$status"
}
trap cleanup EXIT
mkdir -p "$TEMP/tools" "$TEMP/external" "$TEMP/ports/doomgeneric"
cp "$SOURCE_ROOT/disk.img" "$TEMP/disk.img"
cp "$SOURCE_ROOT/tools/sxfs-cli" "$TEMP/tools/sxfs-cli"
chmod +x "$TEMP/tools/sxfs-cli"

if [[ -z "$WAD_PATH" ]]; then
    WAD_PATH="$TEMP/freedoom1.wad"
    printf 'SAVANXP-DOOM-PERSISTENCE-FIXTURE\n' > "$WAD_PATH"
elif [[ ! -f "$WAD_PATH" ]]; then
    echo "verify-doom: WAD does not exist: $WAD_PATH" >&2
    exit 1
else
    WAD_PATH=$(realpath "$WAD_PATH")
fi

export SAVANXP_OUTPUT_ROOT="$TEMP"
export SAVANXP_BUILD_DIR="$TEMP/linux"
export SAVANXP_DOOM_BUILD_ROOT="$TEMP/ports/doomgeneric"
export SAVANXP_EXTERNAL_ROOT="$TEMP/external"
export SAVANXP_DISK_IMAGE="$TEMP/disk.img"
export SAVANXP_SXFS_CLI="$TEMP/tools/sxfs-cli"

"$ROOT/ports/doomgeneric/build.sh" --wad "$WAD_PATH"

# Add data that the normal internal build must not replace.
DATA_STAGE="$TEMP/doom-data"
mkdir -p "$DATA_STAGE/games/doom/savegames"
printf 'default configuration\n' > "$DATA_STAGE/games/doom/default.cfg"
printf 'doom configuration\n' > "$DATA_STAGE/games/doom/doomgenericdoom.cfg"
printf 'savegame payload\n' > "$DATA_STAGE/games/doom/savegames/regression.sav"
"$PYTHON" "$ROOT/tools/sxfs_sync.py" \
    --image "$TEMP/disk.img" \
    --source "$DATA_STAGE" \
    --cli "$TEMP/tools/sxfs-cli" \
    --sectors "${SAVANXP_SXFS_SECTORS:-131072}"

EXTRACT="$TEMP/extracted"
"$TEMP/tools/sxfs-cli" extract "$TEMP/disk.img" "$EXTRACT" >/dev/null
for path in bin/doomgeneric games/doom/"$(basename -- "$WAD_PATH")" games/doom/default.cfg games/doom/doomgenericdoom.cfg games/doom/savegames/regression.sav; do
    [[ -f "$EXTRACT/$path" ]] || { echo "verify-doom: missing $path after install" >&2; exit 1; }
done
before_doom=$(sha256sum "$EXTRACT/bin/doomgeneric" | cut -d' ' -f1)
before_wad=$(sha256sum "$EXTRACT/games/doom/$(basename -- "$WAD_PATH")" | cut -d' ' -f1)
before_cfg=$(sha256sum "$EXTRACT/games/doom/default.cfg" | cut -d' ' -f1)
before_doom_cfg=$(sha256sum "$EXTRACT/games/doom/doomgenericdoom.cfg" | cut -d' ' -f1)
before_save=$(sha256sum "$EXTRACT/games/doom/savegames/regression.sav" | cut -d' ' -f1)

"$ROOT/build.sh" build --jobs "$JOBS"
"$TEMP/tools/sxfs-cli" extract "$TEMP/disk.img" "$EXTRACT" >/dev/null
[[ "$(sha256sum "$EXTRACT/bin/doomgeneric" | cut -d' ' -f1)" == "$before_doom" ]]
[[ "$(sha256sum "$EXTRACT/games/doom/$(basename -- "$WAD_PATH")" | cut -d' ' -f1)" == "$before_wad" ]]
[[ "$(sha256sum "$EXTRACT/games/doom/default.cfg" | cut -d' ' -f1)" == "$before_cfg" ]]
[[ "$(sha256sum "$EXTRACT/games/doom/doomgenericdoom.cfg" | cut -d' ' -f1)" == "$before_doom_cfg" ]]
[[ "$(sha256sum "$EXTRACT/games/doom/savegames/regression.sav" | cut -d' ' -f1)" == "$before_save" ]]
echo "Doom persistence: PASS"
