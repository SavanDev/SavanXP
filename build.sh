#!/usr/bin/env bash
# Linux build entry point. CMake owns the build graph; this script only
# resolves the preset-like options and prepares Linux-only external tools.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${SAVANXP_BUILD_DIR:-"$ROOT/build/linux"}
OUTPUT_ROOT=${SAVANXP_OUTPUT_ROOT:-"$ROOT/build"}
BUILD_TYPE=${SAVANXP_BUILD_TYPE:-Debug}
INCLUDE_TEST_APPS=${SAVANXP_INCLUDE_TEST_APPS:-ON}
ACCEL=${SAVANXP_ACCEL:-tcg}
SMP=${SAVANXP_SMP:-1}
VIRTIO=${SAVANXP_VIRTIO:-OFF}
HEADLESS=${SAVANXP_HEADLESS:-OFF}
JOBS=${SAVANXP_JOBS:-}
SMOKE_SCENARIO=""
SMOKE_TIMEOUT=120
SMOKE_TIMEOUT_SET=0
SMOKE_COMMAND_OVERRIDE=""
SMOKE_GPU_SOAK_ITERATIONS=${SAVANXP_GPU_SOAK_ITERATIONS:-}
SMOKE_LIST=0

COMMAND=build
if [[ $# -gt 0 && "$1" != -* ]]; then
    COMMAND=$1
    shift
fi
if [[ "$COMMAND" == smoke && $# -gt 0 && "$1" != -* ]]; then
    SMOKE_SCENARIO=$1
    shift
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-test-apps)
            INCLUDE_TEST_APPS=OFF
            shift
            ;;
        --debug)
            BUILD_TYPE=Debug
            shift
            ;;
        --release)
            BUILD_TYPE=RelWithDebInfo
            shift
            ;;
        --jobs)
            [[ $# -ge 2 ]] || { echo "build.sh: --jobs requires a value" >&2; exit 2; }
            JOBS=$2
            shift 2
            ;;
        --accel)
            [[ $# -ge 2 ]] || { echo "build.sh: --accel requires tcg or kvm" >&2; exit 2; }
            ACCEL=$2
            shift 2
            ;;
        --smp)
            [[ $# -ge 2 ]] || { echo "build.sh: --smp requires a value" >&2; exit 2; }
            SMP=$2
            shift 2
            ;;
        --virtio)
            VIRTIO=ON
            shift
            ;;
        --headless)
            HEADLESS=ON
            shift
            ;;
        --scenario)
            [[ $# -ge 2 ]] || { echo "build.sh: --scenario requires a value" >&2; exit 2; }
            SMOKE_SCENARIO=$2
            shift 2
            ;;
        --smoke-command)
            [[ $# -ge 2 ]] || { echo "build.sh: --smoke-command requires a value" >&2; exit 2; }
            SMOKE_COMMAND_OVERRIDE=$2
            shift 2
            ;;
        --timeout)
            [[ $# -ge 2 ]] || { echo "build.sh: --timeout requires a value" >&2; exit 2; }
            SMOKE_TIMEOUT=$2
            SMOKE_TIMEOUT_SET=1
            shift 2
            ;;
        --gpu-soak-iterations)
            [[ $# -ge 2 ]] || { echo "build.sh: --gpu-soak-iterations requires a value" >&2; exit 2; }
            SMOKE_GPU_SOAK_ITERATIONS=$2
            shift 2
            ;;
        --list)
            SMOKE_LIST=1
            shift
            ;;
        -h|--help)
            cat <<'EOF'
Usage: ./build.sh [build|configure|kernel|userland|iso|run|debug|smoke|test|clean] [options]

Options:
  --no-test-apps   omit diagnostic and test applications
  (Catalog scenarios may also be used as direct commands, e.g. windowd-smoke.)
  --debug           configure a Debug build (default)
  --release         configure a RelWithDebInfo build
  --accel tcg|kvm   select the QEMU accelerator
  --smp N           set the number of virtual CPUs
  --virtio          use QEMU virtio hardware
  --headless        do not open a graphical QEMU window
  --jobs N          pass a parallelism limit to Ninja
  --scenario NAME   smoke scenario (for the smoke command)
  --smoke-command   override the scenario init command
  --timeout SEC     smoke timeout in seconds (default: 120)
  --gpu-soak-iterations N  iterations for the gpu-soak scenario
  --list            list smoke scenarios and exit
EOF
            exit 0
            ;;
        *)
            echo "build.sh: unknown option '$1'" >&2
            exit 2
            ;;
    esac
done

# Keep the one-word smoke targets available on the native path too:
# `./build.sh windowd-smoke` is equivalent to `./build.sh smoke windowd-smoke`.
if [[ "$COMMAND" != smoke && -z "$SMOKE_SCENARIO" ]]; then
    if python3 "$ROOT/tools/smoke_catalog.py" "$COMMAND" --field command >/dev/null 2>&1; then
        SMOKE_SCENARIO="$COMMAND"
        COMMAND=smoke
    fi
fi

if ((SMOKE_LIST)); then
    [[ "$COMMAND" == smoke ]] || {
        echo "build.sh: --list is only valid with the smoke command" >&2
        exit 2
    }
    exec python3 "$ROOT/tools/smoke_catalog.py" --list
fi

case "$ACCEL" in
    tcg|kvm) ;;
    *)
        echo "build.sh: --accel must be tcg or kvm" >&2
        exit 2
        ;;
esac
[[ "$SMP" =~ ^[0-9]+$ ]] && ((SMP >= 1 && SMP <= 32)) || {
    echo "build.sh: --smp must be an integer between 1 and 32" >&2
    exit 2
}

SMOKE_SUCCESS="SMOKE PASS"
SMOKE_FAILURE="SMOKE FAIL"
SMOKE_PREPARE=""
SMOKE_REMOVE_PATHS=""
SMOKE_QMP_CALLBACK=""
SMOKE_PORT_COMMAND=""
SMOKE_READY=""
SMOKE_QMP_DRIVER=""
SMOKE_AUDIO_DEVICE="auto"
SMOKE_WAV_NAME=""
SMOKE_COMPLETION="serial"
SMOKE_READY_WAIT=0
SMOKE_HOST_SERVER=""
SMOKE_HOST_TEST=""
if [[ "$COMMAND" == smoke ]]; then
    [[ -n "$SMOKE_SCENARIO" ]] || SMOKE_SCENARIO=smoke
    if [[ -n "$SMOKE_COMMAND_OVERRIDE" ]]; then
        SMOKE_SPEC=$SMOKE_COMMAND_OVERRIDE
    else
        SMOKE_SPEC=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field command)
        SMOKE_SUCCESS=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field success)
        SMOKE_FAILURE=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field failure)
        if ((SMOKE_TIMEOUT_SET == 0)); then
            SMOKE_TIMEOUT=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field timeout)
        fi
        SMOKE_PREPARE=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field prepare)
        SMOKE_REMOVE_PATHS=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field remove_paths)
        SMOKE_QMP_CALLBACK=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field qmp_callback)
        SMOKE_PORT_COMMAND=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field port_command)
        SMOKE_READY=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field ready)
        SMOKE_QMP_DRIVER=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field qmp_driver)
        SMOKE_AUDIO_DEVICE=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field audio_device)
        SMOKE_WAV_NAME=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field wav_name)
        SMOKE_COMPLETION=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field completion)
        SMOKE_READY_WAIT=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field ready_wait)
        SMOKE_HOST_SERVER=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field host_server)
        SMOKE_HOST_TEST=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field host_test)
        SMOKE_UNSUPPORTED=$(python3 "$ROOT/tools/smoke_catalog.py" "$SMOKE_SCENARIO" --field unsupported)
        [[ -z "$SMOKE_UNSUPPORTED" ]] || {
            echo "build.sh: smoke scenario '$SMOKE_SCENARIO' is not Linux-ready: $SMOKE_UNSUPPORTED" >&2
            exit 1
        }
        [[ -z "$SMOKE_HOST_TEST" ]] || SMOKE_SPEC=""
        if [[ "$SMOKE_SCENARIO" == gpu-soak && -n "$SMOKE_GPU_SOAK_ITERATIONS" ]]; then
            [[ "$SMOKE_GPU_SOAK_ITERATIONS" =~ ^[1-9][0-9]*$ ]] || {
                echo "build.sh: --gpu-soak-iterations must be a positive integer" >&2
                exit 2
            }
            SMOKE_SPEC="gputest --soak $SMOKE_GPU_SOAK_ITERATIONS"
            if ((SMOKE_TIMEOUT_SET == 0)); then
                SMOKE_TIMEOUT=$(( (SMOKE_GPU_SOAK_ITERATIONS + 15) / 16 * 60 ))
                ((SMOKE_TIMEOUT >= 360)) || SMOKE_TIMEOUT=360
            fi
        fi
    fi
    if [[ -n "$SMOKE_GPU_SOAK_ITERATIONS" && "$SMOKE_SCENARIO" != gpu-soak ]]; then
        echo "build.sh: --gpu-soak-iterations is only valid for gpu-soak" >&2
        exit 2
    fi
    INCLUDE_TEST_APPS=ON
    HEADLESS=ON
    [[ "$SMOKE_TIMEOUT" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
        echo "build.sh: smoke timeout must be a positive number" >&2
        exit 2
    }
elif [[ -n "$SMOKE_SCENARIO" || -n "$SMOKE_COMMAND_OVERRIDE" ]]; then
    echo "build.sh: --scenario/--smoke-command require the smoke command" >&2
    exit 2
fi
if [[ "$COMMAND" != smoke && -n "$SMOKE_GPU_SOAK_ITERATIONS" ]]; then
    echo "build.sh: --gpu-soak-iterations requires the gpu-soak smoke" >&2
    exit 2
fi

case "$COMMAND" in
    build|configure|kernel|userland|iso|run|debug|smoke|test|clean) ;;
    *)
        echo "build.sh: unknown command '$COMMAND'" >&2
        exit 2
        ;;
esac

# Some ports own a multi-phase host smoke (build/install + more than one QEMU
# boot).  The catalog only names the port entry point; the port keeps the
# workflow and its cleanup rules next to the code it tests.
if [[ "$COMMAND" == smoke && -n "$SMOKE_PORT_COMMAND" && -z "$SMOKE_COMMAND_OVERRIDE" ]]; then
    export SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT"
    export SAVANXP_BUILD_DIR="$BUILD_DIR"
    export SAVANXP_ACCEL="$ACCEL"
    export SAVANXP_SMP="$SMP"
    export SAVANXP_VIRTIO="$VIRTIO"
    export SAVANXP_HEADLESS="$HEADLESS"
    ((SMOKE_TIMEOUT_SET == 0)) || export SAVANXP_SMOKE_TIMEOUT="$SMOKE_TIMEOUT"
    [[ -z "$JOBS" ]] || export SAVANXP_JOBS="$JOBS"
    (cd "$ROOT" && bash -c "$SMOKE_PORT_COMMAND")
    exit $?
fi

require_tool() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "build.sh: required tool '$1' was not found in PATH" >&2
        exit 1
    }
}

for tool in cmake ninja ctest clang clang++ ld.lld llvm-objcopy llvm-readelf python3 git; do
    require_tool "$tool"
done

if [[ "$COMMAND" == smoke ]]; then
    require_tool flock
    mkdir -p "$OUTPUT_ROOT"
    exec 9>"$OUTPUT_ROOT/smoke-runner.lock"
    flock -n 9 || {
        echo "build.sh: another smoke scenario is already running" >&2
        exit 1
    }
fi

if ! python3 -c 'import PIL' >/dev/null 2>&1; then
    echo "build.sh: Python Pillow is required (python3 -m pip install Pillow)" >&2
    exit 1
fi

LIMINE_ROOT="$ROOT/tools/limine"
if [[ ! -f "$LIMINE_ROOT/BOOTX64.EFI" ]]; then
    echo "==> Fetching Limine"
    rm -rf "$LIMINE_ROOT"
    git clone --depth 1 --branch v10.x-binary \
        https://github.com/limine-bootloader/limine.git "$LIMINE_ROOT"
fi

# The ISO deployer is built from the checked-out Limine source when needed.
if [[ "$COMMAND" == iso && ! -x "$LIMINE_ROOT/limine" ]]; then
    require_tool make
    echo "==> Building the Limine ISO deployer"
    make -C "$LIMINE_ROOT"
fi

SMOKE_SERVER_PID=""
SMOKE_SERVER_PORT_FILE=""
cleanup_smoke_server() {
    if [[ -n "$SMOKE_SERVER_PID" ]] && kill -0 "$SMOKE_SERVER_PID" 2>/dev/null; then
        kill "$SMOKE_SERVER_PID" 2>/dev/null || true
        wait "$SMOKE_SERVER_PID" 2>/dev/null || true
    fi
    [[ -z "$SMOKE_SERVER_PORT_FILE" ]] || rm -f "$SMOKE_SERVER_PORT_FILE"
}
if [[ "$COMMAND" == smoke && "$SMOKE_HOST_SERVER" == tcp ]]; then
    mkdir -p "$OUTPUT_ROOT/smoke-logs"
    SMOKE_SERVER_PORT_FILE="$OUTPUT_ROOT/smoke-logs/tcp-smoke.port"
    rm -f "$SMOKE_SERVER_PORT_FILE"
    python3 "$ROOT/tools/tcp_echo_server.py" \
        --port 0 \
        --port-file "$SMOKE_SERVER_PORT_FILE" \
        >"$OUTPUT_ROOT/smoke-logs/tcp-smoke.server.log" 2>&1 &
    SMOKE_SERVER_PID=$!
    trap cleanup_smoke_server EXIT
    for _ in {1..100}; do
        [[ -s "$SMOKE_SERVER_PORT_FILE" ]] && break
        kill -0 "$SMOKE_SERVER_PID" 2>/dev/null || {
            cat "$OUTPUT_ROOT/smoke-logs/tcp-smoke.server.log" >&2
            exit 1
        }
        sleep 0.1
    done
    [[ -s "$SMOKE_SERVER_PORT_FILE" ]] || {
        echo "build.sh: TCP echo server did not publish a port" >&2
        exit 1
    }
    SMOKE_SERVER_PORT=$(<"$SMOKE_SERVER_PORT_FILE")
    SMOKE_SPEC=${SMOKE_SPEC//\{port\}/$SMOKE_SERVER_PORT}
fi

CONFIGURE_ARGS=(
    -S "$ROOT"
    -B "$BUILD_DIR"
    -G Ninja
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-DSAVANXP_OUTPUT_ROOT=$OUTPUT_ROOT"
    "-DSAVANXP_INCLUDE_TEST_APPS=$INCLUDE_TEST_APPS"
    "-DSAVANXP_ACCEL=$ACCEL"
    "-DSAVANXP_SMP=$SMP"
    "-DSAVANXP_VIRTIO=$VIRTIO"
    "-DSAVANXP_HEADLESS=$HEADLESS"
    "-DSAVANXP_SMOKE_COMMAND=${SMOKE_SPEC:-}"
)
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    CONFIGURE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/toolchain-linux.cmake")
fi

configure() {
    echo "==> Configuring $BUILD_DIR ($BUILD_TYPE)"
    cmake "${CONFIGURE_ARGS[@]}"
}

build_target() {
    local target=$1
    shift
    local args=(--build "$BUILD_DIR" --target "$target")
    [[ -z "$JOBS" ]] || args+=(--parallel "$JOBS")
    cmake "${args[@]}" "$@"
}

case "$COMMAND" in
    configure)
        configure
        ;;
    build)
        configure
        build_target savanxp
        ;;
    kernel)
        configure
        build_target savanxp_kernel
        ;;
    userland)
        configure
        build_target savanxp_userland
        ;;
    iso)
        configure
        build_target savanxp_iso
        ;;
    smoke)
        if [[ -n "$SMOKE_HOST_TEST" ]]; then
            configure
            build_target savanxp_tests
            ctest --test-dir "$BUILD_DIR" --output-on-failure -R "^${SMOKE_HOST_TEST}$"
            exit $?
        fi
        configure
        build_target savanxp_disk
        if [[ -n "$SMOKE_PREPARE" ]]; then
            export SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT"
            export SAVANXP_DISK_IMAGE="$OUTPUT_ROOT/disk.img"
            export SAVANXP_SXFS_CLI="$OUTPUT_ROOT/tools/sxfs-cli"
            export SAVANXP_NATIVE_BUILD_ROOT="$OUTPUT_ROOT/native"
            export SAVANXP_HAXE_DEPS_ROOT="$OUTPUT_ROOT/ports/haxe/deps"
            while IFS= read -r prepare_command; do
                [[ -n "$prepare_command" ]] || continue
                echo "==> Preparing smoke scenario: $prepare_command"
                (cd "$ROOT" && bash -c "$prepare_command")
            done <<< "$SMOKE_PREPARE"
        fi
        build_target savanxp_image
        if [[ -n "$SMOKE_SPEC" ]]; then
            [[ -f "$OUTPUT_ROOT/rootfs/SMOKE" ]] && \
                [[ "$(<"$OUTPUT_ROOT/rootfs/SMOKE")" == "$SMOKE_SPEC" ]] || {
                echo "build.sh: staged smoke specification does not match the catalog" >&2
                exit 1
            }
        else
            [[ ! -e "$OUTPUT_ROOT/rootfs/SMOKE" ]] || {
                echo "build.sh: stale smoke specification remains in rootfs" >&2
                exit 1
            }
        fi
        SMOKE_ARGS=(
            --scenario "$SMOKE_SCENARIO"
            --command "$SMOKE_SPEC"
            --ready-token "$SMOKE_READY"
            --qmp-driver "$SMOKE_QMP_DRIVER"
            --completion "$SMOKE_COMPLETION"
            --ready-wait "$SMOKE_READY_WAIT"
            --audio-device "$SMOKE_AUDIO_DEVICE"
            --image-root "$OUTPUT_ROOT/image"
            --disk-image "$OUTPUT_ROOT/disk.img"
            --sxfs-cli "$OUTPUT_ROOT/tools/sxfs-cli"
            --success-token "$SMOKE_SUCCESS"
            --failure-token "$SMOKE_FAILURE"
            --timeout "$SMOKE_TIMEOUT"
            --accel "$ACCEL"
            --smp "$SMP"
        )
        [[ "$VIRTIO" == ON ]] && SMOKE_ARGS+=(--virtio)
        [[ -z "$SMOKE_WAV_NAME" ]] || SMOKE_ARGS+=(--wav-path "$OUTPUT_ROOT/$SMOKE_WAV_NAME")
        if [[ -n "$SMOKE_QMP_CALLBACK" ]]; then
            if [[ "$SMOKE_QMP_CALLBACK" = /* ]]; then
                SMOKE_ARGS+=(--qmp-callback "$SMOKE_QMP_CALLBACK")
            else
                SMOKE_ARGS+=(--qmp-callback "$ROOT/$SMOKE_QMP_CALLBACK")
            fi
        fi
        if [[ -n "$SMOKE_REMOVE_PATHS" ]]; then
            while IFS= read -r remove_path; do
                [[ -n "$remove_path" ]] || continue
                SMOKE_ARGS+=(--remove-path "$remove_path")
            done <<< "$SMOKE_REMOVE_PATHS"
        fi
        if python3 "$ROOT/tools/run_smoke.py" "${SMOKE_ARGS[@]}"; then
            smoke_status=0
        else
            smoke_status=$?
        fi
        cleanup_smoke_server
        trap - EXIT
        exit "$smoke_status"
        ;;
    run|debug)
        configure
        build_target savanxp
        QEMU_ARGS=(
            --image-root "$OUTPUT_ROOT/image"
            --disk-image "$OUTPUT_ROOT/disk.img"
            --accel "$ACCEL"
            --smp "$SMP"
        )
        [[ "$VIRTIO" == ON ]] && QEMU_ARGS+=(--virtio)
        [[ "$HEADLESS" == ON ]] && QEMU_ARGS+=(--headless)
        [[ "$COMMAND" == debug ]] && QEMU_ARGS+=(--debug)
        exec python3 "$ROOT/tools/run_qemu.py" "${QEMU_ARGS[@]}"
        ;;
    test)
        configure
        build_target savanxp_tests
        ctest --test-dir "$BUILD_DIR" --output-on-failure
        ;;
    clean)
        # CMake removes generated outputs it owns, but the persistent image is
        # deliberately not a cleanable output and external/ stays untouched.
        build_target clean
        ;;
esac
