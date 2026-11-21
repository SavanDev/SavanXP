#!/bin/bash
# Build libcrypto.a and libssl.a.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

cd "$BUILD"
"$MAKE_CMD" -j"$(sx_jobs)" build_libs
echo "== libcrypto.a $(du -h libcrypto.a | cut -f1), libssl.a $(du -h libssl.a | cut -f1)"
