#!/usr/bin/env bash
# Shared freestanding ABI for this branch's in-tree and external builds.
SAVANXP_ARCH="${SAVANXP_ARCH:-i386}"
if [[ "$SAVANXP_ARCH" != i386 ]]; then
    echo "arch-env.sh: this branch supports i386 only; use master for x86_64" >&2
    return 2 2>/dev/null || exit 2
fi
SX_TARGET_TRIPLE=i386-unknown-none-elf
SX_HOST_TRIPLE=i386-unknown-linux-gnu
SX_LINK_EMULATION=elf_i386
SX_MCMODEL_SMALL=
SX_MCMODEL_MEDIUM=
