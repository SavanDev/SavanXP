#!/bin/bash
# Build and install the OpenSSL port: fetch -> configure -> make -> link ->
# install (ports/openssl/README.md).
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
NO_INSTALL=0
SKIP_PORT=0
NO_COMPACT=0
usage() {
    cat <<'USAGE'
Usage: ./ports/openssl/build.sh [--no-install] [--skip-port] [--no-compact]

  --no-install  Build and stage without touching build/disk.img.
  --skip-port   Install the libraries already linked.
  --no-compact  Disable automatic SxFS compaction during installation.
USAGE
}
while (($#)); do
    case "$1" in
        --no-install) NO_INSTALL=1; shift ;;
        --skip-port) SKIP_PORT=1; shift ;;
        --no-compact) NO_COMPACT=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "openssl: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done
if ((SKIP_PORT == 0)); then
    for step in fetch configure make link; do
        bash "$HERE/$step.sh"
    done
fi
args=()
((NO_INSTALL == 0)) || args+=(--no-install)
((NO_COMPACT == 0)) || args+=(--no-compact)
bash "$HERE/install.sh" "${args[@]}"
