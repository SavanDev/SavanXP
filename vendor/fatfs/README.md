# FatFs, vendored

FatFs R0.16 by ChaN, the generic FAT module SavanXP's FAT driver is built on
([`docs/roadmaps/open/FAT_ROADMAP.md`](../../docs/roadmaps/open/FAT_ROADMAP.md)). The release and the
zip's checksum are in `FATFS_RELEASE.txt`; the license is `LICENSE.txt`.

The files under `source/` are byte-for-byte the release's, CRLF line endings
included. Do not edit them: a change in behaviour goes in the configuration or
in the driver's glue.

Left out of the release on purpose:

| File | Why |
| --- | --- |
| `ffconf.h` | SavanXP's configuration is `kernel/fatfs/ffconf.h`. With no copy here, `ff.h`'s `#include "ffconf.h"` finds that one through the include path. |
| `diskio.c` | An example. The driver implements `disk_*` over `block::`. |
| `ffsystem.c` | An example. The driver implements `ff_memalloc`/`ff_memfree` over the kernel heap. |
| `documents/` | The HTML manual, at http://elm-chan.org/fsw/ff/. |

`kernel/fatfs/string.h` stands in for the `<string.h>` the freestanding
compiler does not ship.

To update: download the new zip, check its contents, copy the same files over,
compare the new `ffconf.h` with ours option by option, and update
`FATFS_RELEASE.txt` and `docs/third-party/THIRD_PARTY_PROVENANCE.md`.
