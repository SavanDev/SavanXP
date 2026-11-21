#!/bin/bash
# Install the libraries, the CA store and the notices through SxFS.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

export SAVANXP_OUTPUT_ROOT="$OUTPUT_ROOT"
NO_INSTALL=0
NO_COMPACT=0
while (($#)); do
    case "$1" in
        --no-install) NO_INSTALL=1; shift ;;
        --no-compact) NO_COMPACT=1; shift ;;
        *) echo "openssl install: unknown option '$1'" >&2; exit 2 ;;
    esac
done
PYTHON="${SAVANXP_PYTHON:-python3}"

STAGE="$OUTPUT_ROOT/ports/openssl/install-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/lib" "$STAGE/system/ssl"
for library in "$LIBCRYPTO" "$LIBSSL"; do
    [ -f "$OUTPUT_ROOT/external/$library" ] || {
        echo "openssl: missing $OUTPUT_ROOT/external/$library; run ports/openssl/build.sh" >&2
        exit 1
    }
    cp "$OUTPUT_ROOT/external/$library" "$STAGE/lib/$library"
done
# The trust store: Mozilla's roots, as OpenSSL's default file under
# --openssldir. Test CAs never go here: a scenario that needs one brings it.
cp "$CA_BUNDLE" "$STAGE/system/ssl/cert.pem"
"$PYTHON" "$REPO/tools/stage_licenses.py" stage --root "$REPO" \
    --out "$STAGE/system/licenses" "$PORT/licenses/openssl.notice" "$PORT/licenses/cacert.notice"
echo "openssl: staged /lib/$LIBCRYPTO, /lib/$LIBSSL, /system/ssl/cert.pem ($CA_CERTIFICATES roots, $CA_DATE)"
if ((NO_INSTALL)); then
    exit 0
fi

IMAGE="${SAVANXP_DISK_IMAGE:-$OUTPUT_ROOT/disk.img}"
CLI="${SAVANXP_SXFS_CLI:-$OUTPUT_ROOT/tools/sxfs-cli}"
[[ -f "$IMAGE" ]] || { echo "openssl: missing $IMAGE; run ./build.sh build first" >&2; exit 1; }
[[ -x "$CLI" ]] || { echo "openssl: missing $CLI; run ./build.sh build first" >&2; exit 1; }
SYNC_ARGS=(--image "$IMAGE" --source "$STAGE" --cli "$CLI" --sectors "${SAVANXP_SXFS_SECTORS:-2097152}")
((NO_COMPACT == 0)) || SYNC_ARGS+=(--no-compact)
"$PYTHON" "$REPO/tools/sxfs_sync.py" "${SYNC_ARGS[@]}"
echo "openssl: synced the volume"
