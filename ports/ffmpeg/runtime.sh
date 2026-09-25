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

echo "== compilando el runtime con $($CLANG --version | head -1)"
$SX_CC -c -x assembler-with-cpp "$SDK/runtime/crt0.S" -o crt0.o $SX_TARGET_CFLAGS
$SX_CC -c -x assembler-with-cpp "$SDK/runtime/setjmp.S" -o setjmp.o $SX_TARGET_CFLAGS
for unit in libc posix gfx gfx2d sxgui sxgui_app sxchrome; do
    $SX_CC -c -x c "$SDK/runtime/$unit.c" -o "$unit.o" $SX_TARGET_CFLAGS
done
$SX_CC -c -x c "$SDK/runtime/math.c" -o math.o -fno-builtin $SX_TARGET_CFLAGS

rm -f libsavanxp.a libsxgui.a
$AR_CMD rcs libsavanxp.a libc.o posix.o gfx.o gfx2d.o math.o setjmp.o
$AR_CMD rcs libsxgui.a sxgui.o sxgui_app.o sxchrome.o
$RANLIB_CMD libsavanxp.a libsxgui.a
sdk_fingerprint=$( {
    find "$SDK/include" "$REPO/include" "$SDK/runtime" -type f -exec sha256sum {} +
    sha256sum "$SDK/linker.ld"
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
