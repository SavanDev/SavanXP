#!/bin/bash
# Download and verify the pinned OpenSSL release and CA store, unpack the
# release and apply the port's patches.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

mkdir -p "$WORK"
cd "$WORK"

# fetch FILE URL SIZE SHA256
fetch() {
    if [ ! -f "$1" ]; then
        echo "== downloading $2"
        curl -fsSL -o "$1.part" "$2"
        mv "$1.part" "$1"
    fi
    local size sha
    size="$(wc -c < "$1" | tr -d ' ')"
    sha="$(sha256sum "$1" | cut -d' ' -f1)"
    if [ "$size" != "$3" ] || [ "$sha" != "$4" ]; then
        echo "openssl: $1 does not match UPSTREAM" >&2
        echo "         expected $3 bytes, sha256 $4" >&2
        echo "         got      $size bytes, sha256 $sha" >&2
        exit 1
    fi
    echo "== $1: sha256 $sha"
}

TARBALL="openssl-${OPENSSL_VERSION}.tar.gz"
fetch "$TARBALL" "$OPENSSL_ARCHIVE_URL" "$OPENSSL_SIZE" "$OPENSSL_SHA256"
if [ "${SAVANXP_VERIFY_SIGNATURE:-0}" = 1 ]; then
    command -v gpg >/dev/null 2>&1 || { echo "openssl: SAVANXP_VERIFY_SIGNATURE=1 needs gpg" >&2; exit 1; }
    [ -f "$TARBALL.asc" ] || curl -fsSL -o "$TARBALL.asc" "$(metadata signature_url)"
    gpg --batch --verify "$TARBALL.asc" "$TARBALL"
fi
fetch "cacert-${CA_DATE}.pem" "$CA_URL" "$CA_SIZE" "$CA_SHA256"
count="$(grep -c 'BEGIN CERTIFICATE' "$CA_BUNDLE")"
if [ "$count" != "$CA_CERTIFICATES" ]; then
    echo "openssl: the CA store holds $count certificates, UPSTREAM says $CA_CERTIFICATES" >&2
    exit 1
fi

# A tree is patched once: the stamp names the patches applied to it.
patches_id="$(cat "$PORT"/patches/*.patch | sha256sum | cut -d' ' -f1)"
if [ ! -f "$SRC/.savanxp-patches" ] || [ "$(cat "$SRC/.savanxp-patches")" != "$patches_id" ]; then
    echo "== unpacking $TARBALL"
    rm -rf "$SRC"
    tar xzf "$TARBALL"
    for patch in "$PORT"/patches/*.patch; do
        echo "== applying $(basename "$patch")"
        patch -s -p1 -d "$SRC" < "$patch"
    done
    printf '%s\n' "$patches_id" > "$SRC/.savanxp-patches"
fi
echo "== source: $SRC"
