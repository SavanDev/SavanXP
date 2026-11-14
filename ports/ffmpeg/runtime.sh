#!/bin/bash
# Build the SavanXP runtime archives used by FFmpeg's configure checks and
# final static link.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

echo "== sysroot en $SYSROOT"
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT/include"
cp -R "$SDK/include/." "$SYSROOT/include/"
cp -R "$REPO/include/." "$SYSROOT/include/"
cp "$SDK/linker.ld" "$SYSROOT/linker.ld"

mkdir -p "$RUNTIME"
cd "$RUNTIME"

echo "== compiling the runtime with $($CLANG --version | head -1)"
# The units come from the runtime's manifest
# (subsystems/posix/sdk/v1/runtime/units.txt). libsavanxp.a carries what every
# program links; the toolkit goes in its own archive.
compile_units() {
    local archive=$1 unit object
    shift
    local units_text
    units_text=$(savanxp_runtime_units "$REPO" "$@")
    local -a units objects=()
    mapfile -t units <<<"$units_text"
    for unit in "${units[@]}"; do
        object=$(savanxp_runtime_object "$RUNTIME" "$unit")
        case "${unit##*/}" in
            *.S) $SX_CC -c -x assembler-with-cpp "$unit" -o "$object" $SX_TARGET_CFLAGS ;;
            math.c) $SX_CC -c -x c "$unit" -o "$object" -fno-builtin $SX_TARGET_CFLAGS ;;
            *) $SX_CC -c -x c "$unit" -o "$object" $SX_TARGET_CFLAGS ;;
        esac
        # crt0 stays loose: configure and the final link name it first.
        [[ "$unit" == */crt0.S ]] || objects+=("$object")
    done
    rm -f "$archive"
    $AR_CMD rcs "$archive" "${objects[@]}"
}
compile_units libsavanxp.a core math setjmp
compile_units libsxgui.a gui
$RANLIB_CMD libsavanxp.a libsxgui.a

# The loader goes LOOSE, not inside libsavanxp.a.
#
# crt0 calls sx_start_dynamic, which in libc.c checks whether the weak symbol
# sx_run_interpreter is defined, and only the loader defines it. Without this
# object the check yields zero, the interpreter never runs and DT_NEEDED is
# ignored in silence: the program starts fine and the library's functions do not
# exist. Not a loud failure, which is the worst kind.
#
# And it stays out of libsavanxp.a because there every static program of the port
# would carry it -- and run its startup -- without needing it. <dlfcn.h>, POSIX's
# layer over the loader, goes with it.
echo "== compiling the loader"
$SX_CC -c -x c "$REPO/system/loader/ldso.c" -o ldso.o $SX_TARGET_CFLAGS
$SX_CC -c -x c "$REPO/subsystems/posix/userland/dlfcn.c" -o dlfcn.o $SX_TARGET_CFLAGS

sdk_fingerprint=$( {
    find "$SDK/include" "$REPO/include" "$SDK/runtime" "$REPO/system/gfx" -type f -exec sha256sum {} +
    sha256sum "$SDK/linker.ld"
    sha256sum "$REPO/system/loader/ldso.c" "$REPO/subsystems/posix/userland/dlfcn.c"
} | sort | sha256sum | cut -d' ' -f1 )
printf '%s\n' "$FFMPEG_COMMIT" "$SX_TARGET_CFLAGS" "$SX_TARGET_LDFLAGS" "$sdk_fingerprint" > .savanxp-runtime-fingerprint
echo "== libsavanxp.a: $(du -h libsavanxp.a | cut -f1)  libsxgui.a: $(du -h libsxgui.a | cut -f1)"

# Probe the actual link driver before configure starts its many test links.
cat > probe.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
int main(void) {
    char buffer[64];
    double value = strtod("2.5", 0);
    snprintf(buffer, sizeof(buffer), "%s %.2f %.3f", "probe", value, sqrt(2.0));
    printf("%s\n", buffer);
    return 0;
}
EOF
$SX_CC -c -x c probe.c -o probe.o $SX_TARGET_CFLAGS
$SX_LD $SX_TARGET_LDFLAGS -o probe.elf crt0.o probe.o libsavanxp.a
echo "== link de prueba: ok ($(du -h probe.elf | cut -f1))"
$NM_CMD -g probe.elf >/dev/null
