#!/usr/bin/env bash
# Linux entry point for the progressive i386 port. CMake owns the build graph.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
. "$ROOT/tools/arch-env.sh"
export SAVANXP_ARCH
BUILD_DIR=${SAVANXP_BUILD_DIR:-"$ROOT/build/linux-i386"}
OUTPUT_ROOT=${SAVANXP_OUTPUT_ROOT:-"$ROOT/build"}
BUILD_TYPE=${SAVANXP_BUILD_TYPE:-Debug}
INCLUDE_TEST_APPS=${SAVANXP_INCLUDE_TEST_APPS:-ON}
ACCEL=${SAVANXP_ACCEL:-tcg}
SMP=${SAVANXP_SMP:-1}
VIRTIO=${SAVANXP_VIRTIO:-OFF}
HEADLESS=${SAVANXP_HEADLESS:-OFF}
JOBS=${SAVANXP_JOBS:-}
PYTHON=${SAVANXP_PYTHON:-python3}
COMMAND=build
SCENARIO=smoke
TIMEOUT=360
LIST=0
if [[ $# -gt 0 && "$1" != -* ]]; then COMMAND=$1; shift; fi
if [[ "$COMMAND" == smoke && $# -gt 0 && "$1" != -* ]]; then SCENARIO=$1; shift; fi
while (($#)); do
    case "$1" in
        --no-test-apps) INCLUDE_TEST_APPS=OFF; shift ;;
        --debug) BUILD_TYPE=Debug; shift ;;
        --release) BUILD_TYPE=RelWithDebInfo; shift ;;
        --jobs) JOBS=${2:?--jobs requires a value}; shift 2 ;;
        --accel) ACCEL=${2:?--accel requires a value}; shift 2 ;;
        --smp) SMP=${2:?--smp requires a value}; shift 2 ;;
        --headless) HEADLESS=ON; shift ;;
        --legacy) shift ;; # Compatibility alias: i386 is always the default.
        --scenario) SCENARIO=${2:?--scenario requires a value}; shift 2 ;;
        --timeout) TIMEOUT=${2:?--timeout requires a value}; shift 2 ;;
        --list) LIST=1; shift ;;
        --virtio) VIRTIO=ON; shift ;;
        -h|--help)
            cat <<'EOF'
Usage: ./build.sh [build|configure|kernel|userland|iso|live-iso|run|debug|smoke|test|clean] [options]

This branch builds i386 ELF32 and boots through GRUB2/BIOS.
Options:
  --no-test-apps   omit keyecho and multitest32 from new staging
  --debug         Debug build (default)
  --release       RelWithDebInfo build
  --jobs N        build parallelism
  --accel tcg|kvm QEMU accelerator (default: tcg)
  --smp N         1..32 virtual CPUs; preemptive scheduling on every available CPU
  --virtio        virtio disk, network, sound, GPU, tablet and keyboard
  --headless      disable the graphical QEMU window
  --legacy        compatibility alias; i386 is already selected
  --scenario NAME smoke scenario: smp-boot, smp, idle, kernel-preempt, wakeup, hardening, smoke, boot, boot-screen, processes, memory, posix, shared, shared-memory, desktop, sxgui-smoke, windowd-smoke, taskbar, calc-smoke, services, device-smoke, clock-smoke, audio-smoke, audio-record, virtio-smoke, net-smoke, tcp-smoke, media-smoke, power-smoke, native-smoke, storage-smoke, partition-smoke, live-desktop, doom, ccleste, legacy-cpu,
                  progman-smoke, appwiz-smoke, taskmgr-smoke, mines-smoke, filesapp-smoke, sxe-smoke, cursor-repro, gpu-soak, audiod-smoke, volume-smoke, float-smoke, sxfs-smoke, sxmidi-smoke, gfx2d-test, audio-test,
                  ac97-stream, ac97-stream-quiet, ac97-count, virtio-count, virtio-stream, virtio-record, kbd-smoke, pointer-smoke
  --timeout SEC   smoke timeout (default: 360)
  --list          list supported smoke scenarios
EOF
            exit 0 ;;
        *) echo "build.sh: unknown option '$1'" >&2; exit 2 ;;
    esac
done
case "$COMMAND" in
    build|configure|kernel|userland|iso|live-iso|run|debug|smoke|test|clean) ;;
    *)
        if [[ "$COMMAND" == *-smoke || "$COMMAND" == legacy-cpu ]]; then
            SCENARIO=$COMMAND; COMMAND=smoke
        else
            echo "build.sh: unknown command '$COMMAND'" >&2; exit 2
        fi ;;
esac
case "$ACCEL" in tcg|kvm) ;; *) echo "build.sh: --accel must be tcg or kvm" >&2; exit 2 ;; esac
[[ "$SMP" =~ ^([1-9]|[12][0-9]|3[0-2])$ ]] || { echo "build.sh: --smp must be between 1 and 32" >&2; exit 2; }
case "$VIRTIO" in ON|OFF) ;; *) echo "build.sh: SAVANXP_VIRTIO must be ON or OFF" >&2; exit 2 ;; esac
if ((LIST)); then
    [[ "$COMMAND" == smoke ]] || { echo "build.sh: --list requires smoke" >&2; exit 2; }
    "$PYTHON" "$ROOT/tools/smoke_catalog.py" --list
    exit 0
fi
if [[ "$COMMAND" == smoke ]]; then
    case "$SCENARIO" in
        smp-boot|smp|idle|kernel-preempt|wakeup|hardening|smoke|boot|boot-screen|processes|memory|posix|shared|shared-memory|desktop|sxgui-smoke|windowd-smoke|taskbar|calc-smoke|services|device-smoke|clock-smoke|audio-smoke|audio-record|net-smoke|tcp-smoke|media-smoke|power-smoke|native-smoke|storage-smoke|partition-smoke|live-desktop|doom|ccleste|legacy-cpu|virtio-smoke|progman-smoke|appwiz-smoke|taskmgr-smoke|mines-smoke|filesapp-smoke|sxe-smoke|cursor-repro|gpu-soak|audiod-smoke|volume-smoke|float-smoke|sxfs-smoke|sxmidi-smoke|gfx2d-test|audio-test|ac97-stream|ac97-stream-quiet|ac97-count|virtio-count|virtio-stream|virtio-record|kbd-smoke|pointer-smoke) ;;
        *) echo "build.sh: '$SCENARIO' is not ported to i386; use smoke --list (x86_64 scenarios live on master)" >&2; exit 2 ;;
    esac
    INCLUDE_TEST_APPS=ON
fi
CONFIGURE_ARGS=(
    -S "$ROOT" -B "$BUILD_DIR" -G Ninja
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    "-DPython3_EXECUTABLE=$(command -v "$PYTHON")"
    -DSAVANXP_ARCH=i386 "-DSAVANXP_OUTPUT_ROOT=$OUTPUT_ROOT"
    "-DSAVANXP_INCLUDE_TEST_APPS=$INCLUDE_TEST_APPS"
    "-DSAVANXP_ACCEL=$ACCEL" "-DSAVANXP_SMP=$SMP" "-DSAVANXP_HEADLESS=$HEADLESS"
    "-DSAVANXP_VIRTIO=$VIRTIO"
)
[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || CONFIGURE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/toolchain-linux.cmake")
configure() { cmake "${CONFIGURE_ARGS[@]}"; }
build_target() {
    local args=(--build "$BUILD_DIR" --target "$1")
    [[ -z "$JOBS" ]] || args+=(--parallel "$JOBS")
    cmake "${args[@]}"
}
case "$COMMAND" in
    configure) configure ;;
    build) configure; build_target savanxp ;;
    kernel) configure; build_target savanxp_kernel ;;
    userland) configure; build_target savanxp_userland ;;
    live-iso) configure; build_target savanxp_live_iso ;;
    iso) configure; build_target savanxp_disk; build_target savanxp_iso ;;
    run|debug)
        configure
        build_target savanxp_disk
        build_target savanxp_iso
        QEMU_ARGS=(--iso "$OUTPUT_ROOT/SavanXP.iso" --disk-image "$OUTPUT_ROOT/disk.img" --accel "$ACCEL" --smp "$SMP")
        [[ "$HEADLESS" != ON ]] || QEMU_ARGS+=(--headless)
        [[ "$VIRTIO" != ON ]] || QEMU_ARGS+=(--virtio)
        [[ "$COMMAND" != debug ]] || QEMU_ARGS+=(--debug)
        exec "$PYTHON" "$ROOT/tools/run_qemu.py" "${QEMU_ARGS[@]}" ;;
    smoke)
        if [[ "$SCENARIO" == legacy-cpu ]]; then
            exec "$PYTHON" "$ROOT/tools/check_legacy_cpu.py" --accel "$ACCEL"
        fi
        mkdir -p "$OUTPUT_ROOT/smoke-logs"
        exec 9>"$OUTPUT_ROOT/smoke-runner.lock"
        flock -n 9 || { echo "build.sh: another smoke is running" >&2; exit 1; }
        configure
        build_target savanxp_disk
        build_target savanxp_iso
        [[ "$SCENARIO" != storage-smoke && "$SCENARIO" != live-desktop ]] || build_target savanxp_live_iso
        ARTIFACTS=$(mktemp -d "$OUTPUT_ROOT/smoke-logs/$SCENARIO.XXXXXX")
        echo "Smoke artifacts: $ARTIFACTS"
        SMOKE_ARGS=(--smp "$SMP")
        [[ "$VIRTIO" != ON ]] || SMOKE_ARGS+=(--virtio)
        exec "$PYTHON" "$ROOT/tools/run_smoke.py" --scenario "$SCENARIO" \
            --iso "$OUTPUT_ROOT/SavanXP.iso" --disk-image "$OUTPUT_ROOT/disk.img" \
            --desktop-elf "$BUILD_DIR/desktop32" --accel "$ACCEL" \
            --timeout "$TIMEOUT" --artifacts "$ARTIFACTS" "${SMOKE_ARGS[@]}" ;;
    test) configure; build_target savanxp_tests; ctest --test-dir "$BUILD_DIR" --output-on-failure ;;
    clean) configure; build_target clean ;;
esac
