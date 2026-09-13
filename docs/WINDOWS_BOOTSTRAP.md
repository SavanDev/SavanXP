# Windows bootstrap: what gets baked and what does not

`tools/bootstrap.ps1` aims for a self-contained `toolchain/` directory so a
clean Windows checkout can go from `git clone` to `.\build.ps1 build` without
hunting for installers. This page covers the parts of that promise that are
not obvious from reading the script: why Python is bootstrapped the way it
is, why Visual Studio Build Tools is the one component that is not baked, and
a path-quoting gotcha that only shows up on some machines.

## Python is embedded, not installed

`build.ps1` shells out to Python (with Pillow) on every build to generate the
desktop art and convert PNGs into C headers. `bootstrap.ps1` bakes it like
every other tool: the official embeddable package from python.org, a portable
zip with no installer, extracted into `toolchain/python/`.

The embeddable package ships with `pip` and `site-packages` disabled on
purpose (that is what makes it embeddable). To make `pip install Pillow` work,
`Install-Python` does two things the other tools do not need:

- Uncomments `import site` in `python312._pth`, the file that controls what
  the embedded interpreter can import.
- Bootstraps `pip` with `get-pip.py`, pinned by commit (same reasoning as
  `xorriso` in `tools/toolchain.lock.json`: a fixed commit, not whatever
  `bootstrap.pypa.io/get-pip.py` happens to serve today).

Pillow itself is installed by version (`pillowVersion` in the lock file), not
by a pinned wheel hash: `pip` already verifies what it downloads against the
package index, so pinning a hash on top of that would duplicate a check pip
already does.

`Get-PythonExecutable` (`tools/UserAppCommon.ps1`) resolves the baked
`toolchain/python/python.exe` before falling back to `PATH`. That fallback
still checks `python` before `python3`: on Windows, `python3` on `PATH` often
resolves to the Microsoft Store's alias stub, which `Get-Command` finds but
which fails the moment it actually runs.

## Visual Studio Build Tools is not baked

Every other tool in `toolchain/` is a zip or tarball with a fixed URL and a
pinned `sha256`. Visual Studio Build Tools cannot be that: Microsoft does not
publish a portable, version-pinned archive of the MSVC headers and libraries
that `clang` needs to compile a native Windows binary. The bootstrapper at
`aka.ms/vs/17/release` always serves whatever build is currently supported —
that is deliberate on Microsoft's part, for security reasons — so there is no
stable byte stream to hash the way there is for an LLVM or a ninja release.

This matters because compiling the kernel and userland does not need MSVC at
all (`-target x86_64-unknown-none-elf -ffreestanding` never touches the host
CRT), but one thing in the build does: `sxfs-cli`
(`tools/UserAppCommon.ps1:Build-SxfsCli`), the native Windows host tool that
libsxfs uses to write `SxFS` images. `clang` on Windows defaults to targeting
MSVC, so it needs `string.h`, `stdio.h` and the rest of the CRT to come from
somewhere — normally a Visual Studio installation.

`Install-VsBuildTools` detects an existing install via `vswhere.exe` and, if
the C++ workload is missing, installs `Microsoft.VisualStudio.2022.BuildTools`
through `winget` (present by default on Windows 10 2004+ and Windows 11).
`-SkipVsBuildTools` opts out for anyone who already has a full Visual Studio
or prefers to install it by hand.

## A space in the Windows profile name can break the build

`clang++` is invoked through a generated `build/compile.ninja`
(`tools/Ninja.ps1`), not directly, so every include path and flag passes
through one text file before it reaches the compiler. `Format-NinjaVarValue`
used to escape only `$`, on the assumption that spaces in a flag value were
never meaningful — true for every flag except a filesystem path.

On a machine whose Windows profile has a space in it (`C:\Users\Jane
Doe\...`), every `-I <path>` include ninja emits contains one. Ninja passes
`$flags` to the command line as one flat string, so the space inside the path
was indistinguishable from the space that is supposed to separate two
flags — `clang++` split `-I "...Jane Doe\...\include"` into `-I "...Jane"` and
a bogus second argument, and failed with `no such file or directory` naming
half a path.

The fix is in `Format-NinjaVarValue`: any flag value that contains a space
gets wrapped in quotes before it is joined into the ninja variable. Every
token in `Get-CommonFlags`/`Get-UacpiFlags` (`build.ps1`) is meant to be a
single argument — none of them intentionally pack two words into one
element — so quoting on a space is safe everywhere this function is used.
