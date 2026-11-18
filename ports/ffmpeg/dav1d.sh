#!/bin/bash
# Build dav1d, the AV1 decoder, as a static archive for libffmpeg.so.0.4.
#
# dav1d builds with Meson. This writes a cross file for the SavanXP target
# naming the same compiler driver FFmpeg's configure uses, the SDK's sxcc, and
# installs headers, libdav1d.a and dav1d.pc under $DAV1D_PREFIX, where
# configure.sh finds them through pkg-config.
#
# Assembly is off, as it is for FFmpeg (--disable-asm): dav1d's x86 assembly
# needs nasm in the toolchain, and the C code is the reference it checks
# itself against. 8- and 16-bit depths are both built: 10-bit AV1 is common.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

MESON="${SAVANXP_MESON:-meson}"
command -v "$MESON" >/dev/null 2>&1 || {
    echo "dav1d: '$MESON' not found; install Meson or set SAVANXP_MESON" >&2
    exit 1
}
[ -f "$DAV1D_SRC/meson.build" ] || {
    echo "dav1d: missing $DAV1D_SRC; run fetch.sh first" >&2
    exit 1
}
[ -f "$RUNTIME/.savanxp-runtime-fingerprint" ] || {
    echo "dav1d: the SDK sysroot is not checked yet; run runtime.sh first" >&2
    exit 1
}

meson_list() {
    local out="" item
    for item in "$@"; do
        out+="${out:+, }'$item'"
    done
    printf '[%s]' "$out"
}

read -r -a target_cflags <<< "$SX_TARGET_CFLAGS"
CROSS="$WORK/dav1d-cross.ini"
cat > "$CROSS.new" <<EOF
[binaries]
c = $(meson_list $SX_CC)
ar = '$AR_CMD'
strip = '$STRIP_CMD'

[built-in options]
c_args = $(meson_list "${target_cflags[@]}")

[host_machine]
system = 'savanxp'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

OPTIONS=(
    --cross-file "$CROSS"
    --prefix "$DAV1D_PREFIX"
    --libdir lib
    --buildtype release
    --default-library static
    -Denable_asm=false
    -Denable_tools=false
    -Denable_examples=false
    -Denable_tests=false
    -Denable_docs=false
    -Dlogging=false
    -Dbitdepths=8,16
)

# Reconfigure only when something that shapes the build changed.
WANTED="$(printf '%s\n' "$DAV1D_SHA256" "$($CLANG --version | head -n 1)" \
    "$(cat "$RUNTIME/.savanxp-runtime-fingerprint" 2>/dev/null || echo missing)" "${OPTIONS[@]}" \
    "$(cat "$CROSS.new")")"
STAMP="$DAV1D_BUILD/.savanxp-dav1d"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$WANTED" ]; then
    rm -rf "$DAV1D_BUILD" "$DAV1D_PREFIX"
    mv "$CROSS.new" "$CROSS"
    "$MESON" setup "$DAV1D_BUILD" "$DAV1D_SRC" "${OPTIONS[@]}"
    printf '%s' "$WANTED" > "$STAMP"
else
    rm -f "$CROSS.new"
    echo "== dav1d already configured with these options"
fi

"$MESON" install -C "$DAV1D_BUILD" --quiet
[ -f "$DAV1D_PREFIX/lib/libdav1d.a" ] || {
    echo "dav1d: the build did not produce libdav1d.a" >&2
    exit 1
}
echo "== libdav1d.a: $(du -h "$DAV1D_PREFIX/lib/libdav1d.a" | cut -f1)"
