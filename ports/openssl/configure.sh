#!/bin/bash
# Configure OpenSSL for SavanXP (ports/openssl/README.md, "Configuration").
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

echo "== SDK sysroot at $SYSROOT"
SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$REPO/tools/make-sdk.sh" --quiet

# What the library is built without, and why:
#
# - no-asm: the SDK's profile is plain C; assembly is a later measurement.
# - no-threads, no-async: one connection per worker thread in its caller;
#   OpenSSL's locks would need pthread in both runtimes.
# - no-sock, no-dso, no-engine, no-module, no-autoload-config: the caller
#   moves the bytes (memory BIOs) and nothing is loaded at run time.
# - no-stdio and OPENSSL_NO_POSIX_IO (with patches/0001): no file system
#   below the library; certificates arrive in memory.
# - NO_SYSLOG: no syslog BIO.
# - no-quic, no-dtls, no-ssl3, no-comp, and the legacy and national ciphers:
#   the client speaks TLS 1.2 and 1.3 over TCP.
# - no-http, no-ocsp, no-ct, no-cmp, no-cms, no-ts, no-srp: protocols and
#   formats around TLS that nothing here uses yet.
# - no-apps, no-tests, no-docs: the libraries only.
#
# Entropy comes from getentropy() (--with-rand-seed=getrandom: OpenSSL takes
# the program's weak getentropy), which the runtime gives in both
# subsystems from the kernel's generator.
OPTIONS=(
    --with-rand-seed=getrandom
    no-asm no-threads no-async
    no-sock no-dso no-engine no-module no-autoload-config
    no-stdio no-secure-memory no-ui-console
    no-quic no-dtls no-ssl3 no-comp no-legacy no-ktls
    no-afalgeng no-padlockeng no-capieng no-devcryptoeng no-uplink
    no-http no-ocsp no-ct no-cmp no-cms no-ts no-srp
    no-gost no-idea no-mdc2 no-rc2 no-rc4 no-rc5 no-whirlpool no-seed
    no-camellia no-aria no-bf no-cast no-sm2 no-sm3 no-sm4
    no-apps no-tests no-docs
)

fingerprint=$(printf '%s\n' "$OPENSSL_VERSION" "${OPTIONS[@]}" \
    "$(cat "$PORT/overlay/50-savanxp.conf" "$SRC/.savanxp-patches")" \
    "$(find "$SYSROOT/include" "$SYSROOT/share/sdk/sxcc" -type f -exec sha256sum {} + | sort)" |
    sha256sum | cut -d' ' -f1)
if [ -f "$BUILD/Makefile" ] && [ "$(cat "$BUILD/.savanxp-configure" 2>/dev/null)" = "$fingerprint" ]; then
    echo "== configured: $BUILD"
    exit 0
fi

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
CC="$SX_CC" CFLAGS="-fPIC -O2" CPPFLAGS="-DNO_SYSLOG -DOPENSSL_NO_POSIX_IO" \
    AR="$AR_CMD" RANLIB="$RANLIB_CMD" \
    "$PERL_CMD" "$SRC/Configure" --config="$PORT/overlay/50-savanxp.conf" savanxp-x86_64 \
    --prefix=/ --openssldir=/system/ssl "${OPTIONS[@]}"
printf '%s\n' "$fingerprint" > .savanxp-configure
echo "== configured: $BUILD"
