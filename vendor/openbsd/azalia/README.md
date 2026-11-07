# OpenBSD Azalia source snapshot

Pristine `sys/dev/pci/azalia.c`, `azalia_codec.c` and `azalia.h` from
https://github.com/openbsd/src at the revision in `OPENBSD_COMMIT`.
`SHA256SUMS` records the original bytes. Check with `sha256sum -c SHA256SUMS`
from this directory. `LICENSE` reproduces the NetBSD Foundation notice.

These originals are kept for review; the kernel compiles the selective port
in `kernel/azalia/`. See `docs/kernel/AZALIA.md` for the precise function map
and changes. No network access is needed for the normal build.
