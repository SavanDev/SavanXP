# Xash3D FWGS

The first part of the [dedicated server port](../../docs/roadmaps/open/XASH3D_ROADMAP.md#step-2--the-engine-with-ref_soft-and-a-savanxp-backend)
builds HLSDK Portable's Half-Life game DLL for amd64 SavanXP POSIX.
`UPSTREAM` pins the source by revision and SHA-256. The sources and built
files are kept under `build/ports/xash3d/`.

```sh
./build.sh build
./ports/xash3d/build.sh
./build.sh smoke xash3d-module
```

The standalone build installs the probe as `/bin/xash3d-probe` and the module
as `/games/xash3d/valve/dlls/hl_savanxp_amd64.so`. Use `--no-install` to
produce the files without updating the SxFS image. The probe loads the module
with `dlopen`, checks C++ destructor registration from the module's
constructors, resolves `worldspawn` with `dladdr` and calls the real
`GameDLLInit` with the engine callbacks it needs for initialization. The
smoke scenario checks that the probe exits successfully.

The build includes a small C++ ABI runtime for this module. It uses the
program's allocator and runs module destructors on normal process exit.
The dedicated engine and its filesystem and UDP paths are the next integration
point. That is where a user-supplied `valve/` tree can drive a real map load.
