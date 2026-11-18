#!/bin/bash
# Bring the SDK sysroot up to date, record its fingerprint, and check that the
# toolchain this port uses (the SDK's sxcc) links a program against it.
#
# The port used to build a sysroot of its own here: a copy of every header,
# the runtime compiled into libsavanxp.a, and the loader. The SDK sysroot is
# that now (docs/roadmaps/done/SDK_V2_ROADMAP.md, phase 5), so nothing is
# compiled here.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

echo "== SDK sysroot at $SYSROOT"
SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$REPO/tools/make-sdk.sh" --quiet

mkdir -p "$RUNTIME"
cd "$RUNTIME"

# configure.sh and dav1d.sh reconfigure when this changes: the sysroot's
# headers, the static SSE runtime and the driver, by content, so a sysroot that
# was only reassembled does not force a rebuild of FFmpeg.
sdk_fingerprint=$( {
    find "$SYSROOT/include" "$SYSROOT/lib/sse" "$SYSROOT/share/sdk/sxcc" "$SYSROOT/share/sdk/lib" \
        "$SYSROOT/linker.ld" -type f -exec sha256sum {} + | sed "s|$SYSROOT/||"
} | sort | sha256sum | cut -d' ' -f1 )
printf '%s\n' "$FFMPEG_COMMIT" "$SX_CC" "$SX_TARGET_CFLAGS" "$sdk_fingerprint" > .savanxp-runtime-fingerprint

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
$SX_LD -o probe.elf probe.o
echo "== test link: ok ($(du -h probe.elf | cut -f1))"
$NM_CMD -g probe.elf >/dev/null
