# Xash3D FWGS

The [dedicated server port](../../docs/roadmaps/open/XASH3D_ROADMAP.md#step-2--the-engine-with-ref_soft-and-a-savanxp-backend)
builds the Xash3D dedicated server, HLSDK Portable's Half-Life game DLL
and Xash3D's filesystem module for amd64 SavanXP POSIX. `UPSTREAM` pins the
source archives by revision and SHA-256. The archives and built files are
kept under `build/ports/xash3d/`.

```sh
./build.sh build
./ports/xash3d/build.sh
./build.sh smoke xash3d-module
./build.sh smoke xash3d-filesystem
```

The standalone build installs the probe as `/bin/xash3d-probe` and the module
as `/games/xash3d/valve/dlls/hl_savanxp_amd64.so`. Use `--no-install` to
produce the files without updating the SxFS image. The probe loads the module
with `dlopen`, checks C++ destructor registration from the module's
constructors, resolves `worldspawn` with `dladdr` and calls the real
`GameDLLInit` with the engine callbacks it needs for initialization. The
smoke scenario checks that the probe exits successfully.

The build also installs `/games/xash3d/filesystem_stdio.so` and
`/bin/xash3d-fsprobe`. The filesystem smoke uses synthetic data and verifies
the filesystem interface, game metadata, case-insensitive lookup of a loose
file and the contents of a PAK entry. For a staged `valve/` tree, the probe
takes the absolute path of its parent and one or more game-relative paths and
prints the size and FNV-1a hash of each file it reads.

```sh
/bin/xash3d-fsprobe /games/xash3d maps/c0a0.bsp
```

The modules use a small C++ ABI runtime that uses the program's allocator and
runs module destructors on normal process exit.

## Dedicated server

With a writable game tree under `/games/xash3d/valve`, run the following in
SavanXP. Use `-basedir` to select another directory containing `valve` and its
SavanXP game DLL under `valve/dlls/`.

```sh
/bin/xash3d-dedicated +sv_lan 1 +map c0a0
```

The server listens on IPv4 UDP port 27015 by default. Use `-port` for another
fixed port and `-ip` for a specific numeric IPv4 bind address. The POSIX
stack currently limits UDP payloads to 1 KiB. IPv6, DNS, HTTP downloads and
XRCON are pending. The smoke covers map activation and server discovery.
Joining with a client and running a session are future checks.

The host smoke takes your own unpacked Half-Life `valve/` directory. It copies
the files used by the map to a disposable SxFS image and packages `sound/`
into one PAK to fit the inode budget. The host sends a UDP server-info query
and verifies the map and dedicated-server mode.

```sh
SAVANXP_XASH3D_VALVE=/path/to/valve ./build.sh smoke xash3d-dedicated
```
