#!/bin/bash
# Shared environment for the OpenSSL port (ports/openssl/README.md).
# This file is sourced; it is not executed directly.

set -euo pipefail

PORT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${REPO:-$(cd "$PORT/../.." && pwd)}"
OUTPUT_ROOT="${SAVANXP_OUTPUT_ROOT:-$REPO/build}"
case "$OUTPUT_ROOT" in
    /*) ;;
    *) OUTPUT_ROOT="$REPO/$OUTPUT_ROOT" ;;
esac
UPSTREAM_FILE="$PORT/UPSTREAM"

metadata() {
    sed -n "s/^$1=//p" "$UPSTREAM_FILE" | head -n 1
}

OPENSSL_VERSION=$(metadata version)
OPENSSL_ARCHIVE_URL=$(metadata archive_url)
OPENSSL_SHA256=$(metadata archive_sha256)
OPENSSL_SIZE=$(metadata archive_size)
CA_DATE=$(metadata ca_date)
CA_URL=$(metadata ca_url)
CA_SHA256=$(metadata ca_sha256)
CA_SIZE=$(metadata ca_size)
CA_CERTIFICATES=$(metadata ca_certificates)

WORK="${WORK:-$OUTPUT_ROOT/ports/openssl/work}"
case "$WORK" in
    *[[:space:]]*)
        echo "openssl: the work directory '$WORK' contains whitespace; set WORK" >&2
        exit 1
        ;;
esac
SRC="$WORK/openssl-${OPENSSL_VERSION}"
BUILD="$WORK/build"
OUT="$WORK/out"
CA_BUNDLE="$WORK/cacert-${CA_DATE}.pem"

# The SDK sysroot and its compiler driver, with the static SSE profile and
# -fPIC: the archives go into shared libraries.
SYSROOT=$(SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT" "$REPO/tools/make-sdk.sh" --print-path)
SX_CC="$SYSROOT/share/sdk/sxcc --sx-profile=sse"
CLANG="${SAVANXP_CLANG:-clang}"
LINK_CC="${SAVANXP_LINK_CC:-$CLANG}"
MAKE_CMD="${MAKE:-${SAVANXP_MAKE:-make}}"
AR_CMD="${SAVANXP_AR:-llvm-ar}"
RANLIB_CMD="${SAVANXP_RANLIB:-llvm-ranlib}"
NM_CMD="${SAVANXP_NM:-llvm-nm}"
SIZE_CMD="${SAVANXP_SIZE:-llvm-size}"
PERL_CMD="${SAVANXP_PERL:-perl}"

# The libraries the port produces and the tree consumes.
LIBCRYPTO=libcrypto.so.0.4
LIBSSL=libssl.so.0.4

sx_jobs() {
    if command -v nproc >/dev/null 2>&1; then nproc; else echo 4; fi
}
