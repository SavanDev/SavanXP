# License notices in the image

Every third-party component that ends up in an image carries its license text
inside that image, under `/system/licenses/<id>/`. The provenance registry
([THIRD_PARTY_PROVENANCE.md](THIRD_PARTY_PROVENANCE.md)) is where the license
gets reviewed. This directory is how the binary carries it. Most of the
licenses we ship require it: BSD's second clause, the GPL and LGPL, and the
OFL all ask for the notice to go with the binary, not only to stay in the
repository.

## Layout

```text
/system/licenses/<id>/NOTICE      the component's fields, below
/system/licenses/<id>/<text>...   full license texts, copied by basename
```

`/system/licenses` holds one directory per installed component and no index.
Whatever lists the licenses has to scan the directory, so an installed port
shows up and one that was never installed does not.

## Where the notices come from

| Source | Staged by | When |
| --- | --- | --- |
| `assets/licenses/*.notice` | `savanxp_diskfs` (`./build.sh build`) | every build |
| `ports/<name>/licenses/*.notice` | the port's install step | when that port is installed |

Both run `tools/stage_licenses.py stage`. Port notices reach the image through
the same `sxfs_sync.py` pass as the port's binaries. A rebuild of the base
system never removes them, for the same reason it never removes `/bin/doomgeneric`.
A port can install several notices: Doom installs `doomgeneric`, plus
`freedoom` when the IWAD it installs is Freedoom.

## The `.notice` format

One `key=value` per line; `#` starts a comment. The id is the file name
without `.notice`, and has to be unique across the tree.

| Key | Count | Meaning |
| --- | --- | --- |
| `name` | 1, required | Display name |
| `license` | 1, required | License name or SPDX id; `None: ...` when upstream states none |
| `provenance` | 1, required | The `###` heading of its registry entry, verbatim (`SavanXP` for the system itself) |
| `copyright` | any | One holder per line |
| `origin` | 0–1 | Upstream URL |
| `version` | 0–1 | Release or pinned commit |
| `used-in` | any | Where it ends up in the image |
| `source` | 0–1 | Where the corresponding source is, for copyleft components |
| `note` | any | Free text: a required acknowledgement, a redistribution warning |
| `text` | any | A license file, as a path from the repository root |

In the staged `NOTICE`, `text=` gives only the basename, next to it.

## The check

`ctest -R license-notices-test` runs `tools/stage_licenses.py check`. It fails
when a notice is malformed, when a `text=` file is missing, when two notices
share an id, or when a registry entry that ships something has no notice
naming it with `provenance=`. Not shipped: the "Build only" section, and the
entries for the project's own art. Adding a component to the registry
without a notice therefore breaks `./build.sh test`.

## What a notice does not settle

A notice satisfies the "keep the license with the binary" part of a license,
and nothing more:

- **Copyleft source.** GPL and LGPL components also need their corresponding
  source available. `source=` says where it is. For an ISO that is
  published, that source has to be published in the same place.
- **No license.** Celeste Classic's notice says that it has no license and must
  not be redistributed. The notice makes the problem visible but does not
  solve it.
- **The ISO copies the development image.** `tools/iso_volume.py` rebuilds the
  LiveCD volume from `build/disk.img`, so a port installed there goes into
  the ISO with its notice. Keep a port out of a published ISO by building
  that ISO from an image where the port was never installed.
