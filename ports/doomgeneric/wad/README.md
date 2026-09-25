# IWADs

No WAD is committed to the repository. This directory is the default local
input location for the port.

Freedoom is the default, because it is a free IWAD under BSD-3-Clause and
allows the image to be played without proprietary or shareware content. Download
it from https://freedoom.github.io/download.html and place it here:

```text
ports/doomgeneric/wad/freedoom1.wad
```

A different local WAD can be selected explicitly:

```bash
./ports/doomgeneric/build.sh --wad /path/to/doom1.wad
```

The selected file is installed under `/disk/games/doom` in the persistent
image. The WAD is not copied into the port source tree or committed to Git.
Its provenance is recorded in `docs/THIRD_PARTY_PROVENANCE.md`. A missing WAD
is non-fatal: the binary is still built and installed.
