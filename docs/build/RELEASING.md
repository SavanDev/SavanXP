# Releasing a version

How a SavanXP version is closed, built, checked and published on GitHub, so
the next release does not have to be reconstructed from the previous ones.
`v0.4.3` was the first release made from this document. Version numbering and
the build number are in [BUILD_CMAKE.md](BUILD_CMAKE.md#version-and-build-number);
the ISO itself is described in [BUILD_LINUX.md](BUILD_LINUX.md).

Pushing and publishing are outward-facing: they happen only when the release
was asked for. Everything below runs from a full clone (the build number is
`git rev-list --count HEAD`), on `master`, with a clean tree that matches
`origin/master`, after the area checks of `AGENTS.md` have passed on it.

## Names

A release has a short name chosen with it. With `X.Y.Z` the version and
`<Name>` the name (`v0.4.3` was *SavanXP Week*):

| What | Format |
| --- | --- |
| Release commit | `release(vX.Y.Z): <Name>`, body summarizing what the version brings |
| Tag | annotated `vX.Y.Z` on the release commit, message `vX.Y.Z: <Name>!` |
| GitHub release title | `vX.Y.Z: <Name>!` |
| Version bump after it | `chore(version): master carries X.Y.(Z+1)` |

Releases up to `v0.4.2` had no name; their titles are the bare tag.

## 1. The release commit

`include/shared/version.h` already says `X.Y.Z`: master always carries the next
version. The release commit changes three things and nothing else:

- `CHANGELOG.md`: `## [Unreleased]` becomes `## [X.Y.Z] - YYYY-MM-DD`.
- `CMakeLists.txt`: `project(SavanXP VERSION X.Y.Z ...)`.
- `README.md`: ``Current version: `vX.Y.Z` ``.

Commit it before building: the image must carry this commit's build number.

## 2. The release image

The ISO is built in an output root of its own, never from `build/`. The
development `build/disk.img` collects whatever was installed or tested on it
(the shareware `doom1.wad`, the Xeyes runtime, test data), and the ISO takes
its volume from the output root's `disk.img`.

```bash
export SAVANXP_OUTPUT_ROOT=$PWD/build/release-X.Y.Z
export SAVANXP_BUILD_DIR=$SAVANXP_OUTPUT_ROOT/linux
./build.sh build
./ports/doomgeneric/build.sh    # default WAD: ports/doomgeneric/wad/freedoom1.wad
./build.sh iso                  # build/release-X.Y.Z/SavanXP.iso
```

The default build type is used (Debug), as for every release so far. The image
is the plain system plus the Doom port with Freedoom: Phase 1. The FFmpeg port,
Celeste Classic, the Xeyes runtime and any Windows files stay out, unless the
release decides to carry FFmpeg ([below](#a-release-that-carries-ffmpeg)).

## 3. Checks before publishing

With the same two variables exported:

- **Doom from a fresh image.** `./build.sh smoke doom-music-smoke`: Doom opens,
  reaches E1M1 with only `freedoom1.wad` installed, and its music is on the
  captured audio. `v0.4.2` shipped a Doom that exited at once on a fresh
  system (`failed to prepare data directories`): development images had the
  directory it needed, so only an image like the release one showed it.
- **ISO layout and boot.** `python3 tools/iso_boot_test.py --iso
  build/release-X.Y.Z/SavanXP.iso` checks the El Torito catalog and boots BIOS
  and UEFI to `sessiond: session started`.
- **The ISO's volume.** Extract it and look:

  ```bash
  R=build/release-X.Y.Z
  $R/tools/sxfs-cli extract $R/iso-root/boot/disk.img /tmp/isovol /tmp/isovol.owners
  ls -l /tmp/isovol/games/doom      # only freedoom1.wad
  grep doomgeneric /tmp/isovol.owners   # bin/doomgeneric ... 100755
  # Nothing of Microsoft's (AGENTS.md): all three must print nothing. A PE's
  # version resource is UTF-16, hence the second grep.
  find /tmp/isovol -iname sol.exe -o -iname cards.dll -o -iname 'program files'
  grep -rl -a -i 'microsoft corporation' /tmp/isovol
  grep -rlaP 'M\x00i\x00c\x00r\x00o\x00s\x00o\x00f\x00t\x00 \x00C\x00o\x00r\x00p' /tmp/isovol
  ```

  The image carries the Win32 subsystem and a Solitaire shortcut in the Start
  menu, but never the program: the shortcut points at the user's own
  `C:\Program Files\Solitaire\sol.exe`. The DLLs in `system32` and the
  programs in `drive_c/tests` are built from this repository.

- **Doom from the ISO itself**, the way a user meets it: UEFI, 512 MiB, no
  disk. The visual `doom` scenario runs against any QEMU with a QMP socket; it
  reads the audio from two levels above its capture directory. Keep `R` short:
  QEMU's Unix socket path must stay under 108 bytes.

  ```bash
  R=build/release-X.Y.Z/i
  mkdir -p $R/shots/doom
  cp /usr/share/edk2/x64/OVMF_VARS.4m.fd $R/vars.fd   # OVMF paths vary; see tools/iso_boot_test.py
  qemu-system-x86_64 -machine q35 -accel tcg -m 512M -cpu max -smp 2 \
    -display none -vga none -device VGA,edid=on,xres=1280,yres=800 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
    -drive if=pflash,format=raw,file=$R/vars.fd \
    -boot d -cdrom build/release-X.Y.Z/SavanXP.iso -serial file:$R/serial.log \
    -audiodev wav,id=snd0,path=$R/doom-music.wav -device AC97,audiodev=snd0 \
    -qmp unix:$R/q.qmp,server=on,wait=off -no-reboot -no-shutdown &
  python3 tools/shoot_session.py --socket $R/q.qmp --serial $R/serial.log \
    --out $R/shots/doom --scenario doom
  kill %1
  ```

  It prints `doom: OK`; the captures in `$R/shots/doom` show the level.

A failure here stops the release: the fix goes to `master` with its changelog
entry under the version being closed, and the release commit is redone on top.

## 4. Tag, bump and push

```bash
git tag -a vX.Y.Z <release-commit> -m "vX.Y.Z: <Name>!"
# include/shared/version.h: SAVANXP_VERSION_PATCH + 1, then
git commit -m "chore(version): master carries X.Y.(Z+1)" include/shared/version.h
git push origin master vX.Y.Z
```

## 5. Assets

Three files, the same every time:

```bash
cp build/release-X.Y.Z/SavanXP.iso SavanXP-vX.Y.Z.iso
cp ports/doomgeneric/licenses/FREEDOOM-COPYING.txt FREEDOOM-LICENSE.txt
sha256sum SavanXP-vX.Y.Z.iso FREEDOOM-LICENSE.txt > SHA256SUMS
```

## 6. Release notes

The body has this shape, in this order:

1. One line: ``SavanXP `vX.Y.Z` — summarized change list: the opening line of
   each entry. Full detail in `CHANGELOG.md` (`[X.Y.Z]` — YYYY-MM-DD).``
2. `### Highlights`: three bullets, each opening in bold, on what a user would
   notice first.
3. `### Download`: one paragraph on the ISO. Restate it from the current
   facts each time: BIOS and UEFI; QEMU `q35` with 512 MiB or more, or a USB
   stick (`dd`, or Rufus in DD image mode) on an `x86_64` PC with Secure Boot
   off; the Limine menu entries (`boot/limine.conf`); the volume in RAM, so
   changes do not survive a reboot; which disks the live system sees and
   writes; what the image includes (Doom with `freedoom1.wad`, BSD-3-Clause,
   license in `FREEDOOM-LICENSE.txt`) and what it leaves out; checksums in
   `SHA256SUMS`.
4. `### Added`, `### Changed`, `### Removed`, `### Fixed`, as present: one
   bullet per changelog entry, in changelog order, holding the entry's first
   bold span in bold, with a trailing colon dropped.

The lists come straight from the changelog section:

```bash
python3 - CHANGELOG.md 'X.Y.Z' <<'EOF'
import re, sys
text = open(sys.argv[1]).read()
body = text.split(f'## [{sys.argv[2]}]', 1)[1].split('\n## [', 1)[0]
section, entry = None, None
def flush():
    if entry and section:
        m = re.search(r'\*\*(.+?)\*\*', ' '.join(l.strip() for l in entry))
        print(f"- **{m.group(1).strip().rstrip(':') if m else entry[0][:80]}**")
for line in body.splitlines():
    if line.startswith('### '):
        flush(); entry = None; section = line[4:]; print(f'\n{line}\n')
    elif line.startswith('- '):
        flush(); entry = [line[2:]]
    elif entry is not None and line.startswith('  '):
        entry.append(line)
flush()
EOF
```

## A release that carries FFmpeg

FFmpeg is LGPL-2.1, and unlike Freedoom's BSD license that asks for more than
the notice: whoever downloads the library must be able to get its source **from
the same place**. A link to ffmpeg.org does not do it, because the LGPL 2.1
does not let the source live on someone else's server, and that archive could
move. So a release whose image has `/lib/libffmpeg.so.0.4` also publishes the
pinned FFmpeg archive as one of its assets. The port carries no patches
(`patch_count=0` in `ports/ffmpeg/UPSTREAM`), so that archive plus the
scripts in `ports/ffmpeg/` at the tag are the whole corresponding source;
[`ports/ffmpeg/SOURCE.md`](../../ports/ffmpeg/SOURCE.md), shipped in the image,
says so.

Before deciding to carry it: `h264`, `hevc` and `aac` are patent-encumbered
codecs that some distributions leave out. Narrowing `VIDEO_DECODERS` and
`AUDIO_DECODERS` in `ports/ffmpeg/configure.sh` is the way to leave them out.

What changes in the steps above, with the same two variables exported:

- **Step 2**, after the Doom port and before `./build.sh iso`:

  ```bash
  ./ports/ffmpeg/build.sh       # no --with-test-media: test clips stay out
  ```

- **Step 3**, on the extracted volume:

  ```bash
  ls /tmp/isovol/lib/libffmpeg.so.0.4 /tmp/isovol/system/licenses/ffmpeg/
  ls /tmp/isovol/media 2>/dev/null      # nothing: no test clips
  ```

  and `./build.sh smoke ffmpeg-smoke` passes on `master` before the release
  commit.
- **Step 5**, one more asset: the archive the build fetched, checked against
  the pin.

  ```bash
  A=$SAVANXP_OUTPUT_ROOT/ports/ffmpeg/work/ffmpeg-7.1.1.tar.xz
  grep archive_sha256 ports/ffmpeg/UPSTREAM; sha256sum $A   # must match
  cp $A ffmpeg-7.1.1.tar.xz
  sha256sum SavanXP-vX.Y.Z.iso FREEDOOM-LICENSE.txt ffmpeg-7.1.1.tar.xz > SHA256SUMS
  ```

- **Step 6**, the Download paragraph says the image includes the Media
  Player's FFmpeg library (LGPL-2.1) and that its source is the
  `ffmpeg-7.1.1.tar.xz` asset.
- **Step 7**, `ffmpeg-7.1.1.tar.xz` goes on the `gh release create` line, and
  `gh release view` lists four assets.

The archive name follows the version pinned in `UPSTREAM`; when the port moves
to another FFmpeg release, so do these names and the one in `SOURCE.md`.

## 7. Publish

```bash
gh release create vX.Y.Z --title "vX.Y.Z: <Name>!" --notes-file notes.md --latest \
    SavanXP-vX.Y.Z.iso FREEDOOM-LICENSE.txt SHA256SUMS
```

Then check it: `gh release view vX.Y.Z` lists the three assets, and the
downloaded `SHA256SUMS` verifies them.

Real-hardware runs of the new ISO are separate and do not gate the release;
[REAL_HARDWARE.md](../hardware/REAL_HARDWARE.md) records what each machine
showed.
