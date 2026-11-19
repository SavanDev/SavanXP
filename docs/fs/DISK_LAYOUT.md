# What lives where on the system disk

The system disk is the SxFS volume mounted at `/`. The build fills it from two
places: what it compiles (programs, libraries, icons) and the repository's
`diskfs/` folder, copied as it is. Ports and the user add the rest. This page
says which folder each kind of file goes in, so a new one has an obvious place.

## The root

The root holds folders, plus `README` (from `diskfs/README`): what SavanXP is,
how to get around and this layout in short, for whoever opens the disk. The
smokes also use it as a text file that is always there, so it keeps the word
"SavanXP" in its first line and differs between its first and second 8 bytes
(`fdtest`'s shared-offset check).

| folder | what | whose |
| --- | --- | --- |
| `/bin` | POSIX programs: the build's and external ones | system |
| `/lib` | shared libraries | system |
| `/icons` | the `.sxicon` catalog | system |
| `/system` | everything else the build installs (below) | system |
| `/users/<name>` | each person's profile, with `Settings` | that person |
| `/tmp` | scratch space, `1777` | everyone |
| `/games`, `/media` | data that ports install (Doom's WADs, FFmpeg's test media) | system |

Nothing else the build installs goes loose at the root. Before the system ran
from its disk, the root was a data volume and collected files of every kind;
they moved under `/system` once nothing needed them there.

## Under `/system`

| path | what |
| --- | --- |
| `/system/*.sxe` | the SUN programs, with `programs.txt` listing them |
| `/system/assoc.ini`, `/system/mimeicon.ini` | the default file associations and icons by type; a copy in the profile's `Settings` overrides each (icons only in POSIX programs, [FILE_DIALOG.md](../desktop/FILE_DIALOG.md)) |
| `/system/wallpapers` | the wallpapers Display Properties lists |
| `/system/samples` | files to try the system with: `welcome.html` and `about.html` for Web Viewer, `system.mp3`, an MP3 to play |
| `/system/selftests` | fixtures a program's `--selftest` reads, such as Paint's `paint-test.png` |
| `/system/start` | the default Start menu shortcuts a new profile gets |
| `/system/cursors`, `/system/licenses` | cursor themes and third-party notices |
| `/system/sxbasic`, `/system/win`, `/system/linux` | each subsystem's or language's own tree |
| `/system/loader-fixtures` | broken and edge-case shared objects the loader tests load |

Test data that a smoke scenario writes on its disposable disk copy does not
belong to the build and is not listed here.

## Moving a file the build installs

An existing image keeps whatever an earlier build put in it: installing a file
at its new path does not remove the old one. `tools/sxfs_sync.py` does that.
Its `RELOCATED_FILES` and `RELOCATED_FOLDERS` say where each file used to be,
and installing the new path retires the old one. `RETIRED_FOLDERS` removes an
old folder once that leaves it empty; a folder that still holds a file of the
user's stays. A setting that stores a path is migrated where it is read: the
wallpaper in `desktop.cfg`, for example, maps `/wallpapers/...` to
`/system/wallpapers/...`.
