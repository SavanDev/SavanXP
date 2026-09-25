#!/usr/bin/env bash
# Headless visual verification for the SavanXP desktop.
# The scenario driver is shared with the visual test tooling; this wrapper owns
# the native QEMU launch and uses a disposable disk copy.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT_ROOT="${SAVANXP_OUTPUT_ROOT:-$ROOT/build}"
case "$OUTPUT_ROOT" in
    /*) ;;
    *) OUTPUT_ROOT="$ROOT/$OUTPUT_ROOT" ;;
esac
IMAGE_ROOT="$OUTPUT_ROOT/image"
DISK_IMAGE="${SAVANXP_DISK_IMAGE:-$OUTPUT_ROOT/disk.img}"
SXFS_CLI="${SAVANXP_SXFS_CLI:-$OUTPUT_ROOT/tools/sxfs-cli}"
case "$DISK_IMAGE" in
    /*) ;;
    *) DISK_IMAGE="$ROOT/$DISK_IMAGE" ;;
esac
case "$SXFS_CLI" in
    /*) ;;
    *) SXFS_CLI="$ROOT/$SXFS_CLI" ;;
esac
OUT_DIR="$OUTPUT_ROOT/shots"
SCENARIO="desktop"
ACCEL="tcg"
VIRTIO=0
BOOT_WAIT=45
KEEP_RUNNING=0
SCENARIOS="desktop alttab clipboard calc mines files shell appwiz system taskbar kbdlayout wheel notepadwheel bench saturate spin gears mediaplayer"

usage() {
    cat <<'EOF'
Usage: tools/shoot.sh [options]

Options:
  --scenario NAME     Visual scenario (default: desktop).
  --out-dir PATH      Capture directory (default: build/shots).
  --accel tcg|kvm     QEMU accelerator (default: tcg).
  --virtio            Use virtio graphics/input/storage.
  --boot-wait SEC     Wait after the init handoff before driving the session.
  --keep-running      Leave QEMU running after the scenario succeeds.
  -h, --help          Show this help.
EOF
}

while (($#)); do
    case "$1" in
        --scenario)
            (($# >= 2)) || { echo "shoot.sh: --scenario requires a value" >&2; exit 2; }
            SCENARIO=$2
            shift 2
            ;;
        --out-dir)
            (($# >= 2)) || { echo "shoot.sh: --out-dir requires a value" >&2; exit 2; }
            OUT_DIR=$2
            shift 2
            ;;
        --accel)
            (($# >= 2)) || { echo "shoot.sh: --accel requires a value" >&2; exit 2; }
            ACCEL=$2
            shift 2
            ;;
        --virtio) VIRTIO=1; shift ;;
        --boot-wait)
            (($# >= 2)) || { echo "shoot.sh: --boot-wait requires a value" >&2; exit 2; }
            BOOT_WAIT=$2
            shift 2
            ;;
        --keep-running) KEEP_RUNNING=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "shoot.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

case " $SCENARIOS " in
    *" $SCENARIO "*) ;;
    *) echo "shoot.sh: unknown scenario '$SCENARIO'" >&2; usage >&2; exit 2 ;;
esac
case "$ACCEL" in
    tcg|kvm) ;;
    *) echo "shoot.sh: --accel must be tcg or kvm" >&2; exit 2 ;;
esac
[[ "$BOOT_WAIT" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
    echo "shoot.sh: --boot-wait must be a non-negative number" >&2
    exit 2
}
[[ -d "$IMAGE_ROOT" ]] || { echo "shoot.sh: missing $IMAGE_ROOT; run ./build.sh build first" >&2; exit 1; }
[[ -f "$DISK_IMAGE" ]] || { echo "shoot.sh: missing $DISK_IMAGE; run ./build.sh build first" >&2; exit 1; }
[[ -x "$SXFS_CLI" ]] || { echo "shoot.sh: missing $SXFS_CLI; run ./build.sh build first" >&2; exit 1; }
[[ ! -e "$OUTPUT_ROOT/rootfs/SMOKE" ]] || {
    echo "shoot.sh: a smoke specification is planted; run ./build.sh build first" >&2
    exit 1
}
command -v qemu-system-x86_64 >/dev/null 2>&1 || {
    echo "shoot.sh: qemu-system-x86_64 was not found in PATH" >&2
    exit 1
}
command -v flock >/dev/null 2>&1 || {
    echo "shoot.sh: flock was not found in PATH" >&2
    exit 1
}
command -v setsid >/dev/null 2>&1 || {
    echo "shoot.sh: setsid was not found in PATH" >&2
    exit 1
}
exec 8>"$DISK_IMAGE.lock"
flock -n 8 || {
    echo "shoot.sh: persistent image is busy" >&2
    exit 1
}

mkdir -p "$OUT_DIR"
OUT_DIR=$(cd -- "$OUT_DIR" && pwd)
run_tag="shoot.$$"
run_disk="$OUT_DIR/$run_tag.disk.img"
serial_log="$OUT_DIR/shoot-serial.log"
qmp_path="$OUT_DIR/$run_tag.qmp"
hmp_path="$OUT_DIR/$run_tag.hmp"
vars_copy="$OUT_DIR/shoot-vars.fd"
rm -f "$run_disk" "$run_disk.lock" "$qmp_path" "$hmp_path" "$vars_copy" "$serial_log"

cp -- "$DISK_IMAGE" "$run_disk"
"$SXFS_CLI" check "$run_disk" >/dev/null

qemu_pid=""
cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" 2>/dev/null; then
        kill -- "-$qemu_pid" 2>/dev/null || kill "$qemu_pid" 2>/dev/null || true
        wait "$qemu_pid" 2>/dev/null || true
    fi
    rm -f "$run_disk" "$run_disk.lock" "$qmp_path" "$hmp_path" "$vars_copy"
    exit "$status"
}
trap cleanup EXIT

qemu_args=(
    python3 "$ROOT/tools/run_qemu.py"
    --image-root "$IMAGE_ROOT"
    --disk-image "$run_disk"
    --headless
    --accel "$ACCEL"
    --smp 1
    --qmp-path "$qmp_path"
    --monitor-path "$hmp_path"
    --serial-log "$serial_log"
    --vars-copy "$vars_copy"
)
if ((VIRTIO)); then qemu_args+=(--virtio); fi
setsid "${qemu_args[@]}" >"$OUT_DIR/shoot-qemu-out.log" 2>"$OUT_DIR/shoot-qemu-err.log" &
qemu_pid=$!

for _ in {1..150}; do
    [[ -S "$qmp_path" ]] && break
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        cat "$OUT_DIR/shoot-qemu-err.log" >&2 || true
        echo "shoot.sh: QEMU exited before opening QMP" >&2
        exit 1
    fi
    sleep 0.2
done
[[ -S "$qmp_path" ]] || { echo "shoot.sh: QMP socket did not appear" >&2; exit 1; }

session_args=(
    python3 "$ROOT/tools/shoot_session.py"
    --socket "$qmp_path"
    --serial "$serial_log"
    --out "$OUT_DIR"
    --scenario "$SCENARIO"
    --boot-wait "$BOOT_WAIT"
)
if ((VIRTIO)); then session_args+=(--abs-pointer); fi
"${session_args[@]}"
if ((KEEP_RUNNING)); then
    trap - EXIT
    echo "shoot.sh: QEMU still running (pid $qemu_pid); press Ctrl-C to stop it" >&2
    wait "$qemu_pid"
else
    exit 0
fi
