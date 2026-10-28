#!/usr/bin/env python3
"""Regression test for additive SxFS synchronization and safe compaction."""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sxfs_checksum(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def assert_pending_journal_rejected(cli: Path, image: Path, root: Path) -> None:
    pending = root / "pending.img"
    pending.write_bytes(image.read_bytes())
    data = bytearray(pending.read_bytes())
    sequence = struct.unpack_from("<I", data, 16)[0]
    header = bytearray(struct.pack("<8sIIII", b"SXJN\0\0\0\0", 0, sequence + 1, 1, 97))
    struct.pack_into("<I", header, 8, sxfs_checksum(header))
    data[2 * 512 : 3 * 512] = header
    pending.write_bytes(data)
    result = subprocess.run(
        [str(cli), "check", str(pending)], cwd=root, text=True, capture_output=True, check=False
    )
    if result.returncode == 0:
        raise RuntimeError("pending journal was accepted by sxfs-cli check")


def allocated_bytes(path: Path) -> int:
    return path.stat().st_blocks * 512


def assert_grown_image_survives_compaction(cli: Path, sync: Path, root: Path) -> None:
    """A grown image file must survive a rebuild, and must stay sparse.

    An image may be larger than the filesystem inside it: the kernel accepts
    `total_sectors <= device sectors` and ignores the tail, which is the room a
    later grow needs. Compaction rebuilds the image from scratch at exactly
    `total_sectors`, so without an explicit size floor it would quietly shrink a
    deliberately grown image back to 64 MiB and lose the room.

    The second half matters just as much: a grown image is sparse, and the
    rebuild path copies it, so a copy that does not preserve holes would turn
    the reserved tail into real bytes that every later build rewrites.
    """
    seed = root / "grow-seed"
    source = root / "grow-source"
    seed.mkdir()
    source.mkdir()
    # Mismos parametros que el caso de fragmentacion de arriba, que ya se sabe
    # que obligan a compactar: nueve archivos y se borran los alternos, dejando
    # el espacio libre en corridas que no alcanzan para el archivo nuevo.
    names = ("grown_a", "grown_b", "grown_c", "grown_d", "grown_e", "grown_f", "grown_g", "grown_h", "grown_i")
    # Mismos tamanos que el caso de fragmentacion de arriba, y por el mismo
    # motivo: sin esta fragmentacion el rebuild ni siquiera se intenta y el caso
    # pasaria sin probar nada.
    for name in names:
        (seed / f"{name}.bin").write_bytes(b"savanxp-compaction-payload\n" * 9200)
    (source / "added.bin").write_bytes(b"new-build-file\n" * 42858)
    manifest = root / "grow.manifest"
    manifest.write_text(
        "\n".join(f"file\t{name}.bin\t{seed / (name + '.bin')}" for name in names) + "\n",
        encoding="utf-8",
    )

    image = root / "grown.img"
    run([str(cli), "create", str(image), "8192"], root)
    run([str(cli), "apply", str(image), str(manifest)], root)
    # Ownership that is not grown_b.bin's default: compaction rebuilds the
    # image from host files, which carry none, so this survives only if the
    # rebuild carries it (sxfs_sync.carried_ownership).
    owner_manifest = root / "grow.owner"
    owner_manifest.write_text("owner\tgrown_b.bin\t1000\t100\t100600\n", encoding="utf-8")
    run([str(cli), "apply", str(image), str(owner_manifest)], root)
    # Fragment it so the next sync is forced through compaction.
    run(
        [
            str(cli),
            "rm",
            str(image),
            "grown_a.bin",
            "grown_c.bin",
            "grown_e.bin",
            "grown_g.bin",
        ],
        root,
    )

    # Grow the FILE without growing the filesystem, exactly as an admin would.
    # Relative to the current size, because the point is "bigger than the
    # filesystem" and the base image is as big as the format needs for its
    # metadata (8192 sectors with the v2 layout is already 4 MiB).
    os.truncate(image, image.stat().st_size * 2)
    grown_size = image.stat().st_size
    if allocated_bytes(image) >= grown_size:
        raise RuntimeError("the grown image is not sparse to begin with")

    result = run(
        [
            sys.executable,
            str(sync),
            "--image",
            str(image),
            "--source",
            str(source),
            "--cli",
            str(cli),
            "--sectors",
            "8192",
        ],
        root,
    )
    if "compactacion segura completada" not in (result.stdout + result.stderr):
        raise RuntimeError("the grown-image test did not exercise compaction")

    if image.stat().st_size != grown_size:
        raise RuntimeError(
            f"compaction shrank a grown image: {image.stat().st_size} != {grown_size}"
        )
    if allocated_bytes(image) >= grown_size:
        raise RuntimeError("compaction de-sparsified the reserved tail of a grown image")
    # The filesystem itself is unchanged: the tail is room, not data.
    reported = run([str(cli), "info", str(image)], root).stdout.strip()
    if reported != "8192":
        raise RuntimeError(f"the filesystem grew on its own: sxfs-cli info says {reported!r}")

    extracted = root / "grow-extracted"
    run([str(cli), "extract", str(image), str(extracted)], root)
    for name in ("grown_b", "grown_d", "grown_f", "grown_h", "grown_i"):
        if digest(seed / f"{name}.bin") != digest(extracted / f"{name}.bin"):
            raise RuntimeError(f"preserved file differs after growing compaction: {name}")
    if digest(source / "added.bin") != digest(extracted / "added.bin"):
        raise RuntimeError("new build file differs after growing compaction")
    owners = root / "grow-extracted.owners"
    run([str(cli), "extract", str(image), str(root / "grow-owners-tree"), str(owners)], root)
    if "owner\tgrown_b.bin\t1000\t100\t100600\n" not in owners.read_text(encoding="utf-8"):
        raise RuntimeError("compaction lost the ownership a file carried")


# Offsets into the v2 layout (include/sxfs/sxfs_format.h).
SECTOR = 512
SUPERBLOCK_LBAS = (0, 1)
SUPERBLOCK_CHECKSUM = 12
SUPERBLOCK_FEATURES = 72
INODE_TABLE_LBA = 2 + (1 + 512 + 1 + 1024) + 512 + 1
INODE_SIZE = 128
INODE_COUNT = 4096
INODE_MODE = 6
INODE_UID = 84


def strip_ownership(image: Path) -> None:
    """Make an image look written before SXFS_FEATURE_OWNERSHIP: no feature
    bit, and uid, gid and mode zero in every inode."""
    data = bytearray(image.read_bytes())
    table = INODE_TABLE_LBA * SECTOR
    for index in range(INODE_COUNT):
        inode = table + index * INODE_SIZE
        struct.pack_into("<H", data, inode + INODE_MODE, 0)
        struct.pack_into("<II", data, inode + INODE_UID, 0, 0)
    for lba in SUPERBLOCK_LBAS:
        block = lba * SECTOR
        struct.pack_into("<I", data, block + SUPERBLOCK_FEATURES, 0)
        struct.pack_into("<I", data, block + SUPERBLOCK_CHECKSUM, 0)
        checksum = sxfs_checksum(bytes(data[block : block + SECTOR]))
        struct.pack_into("<I", data, block + SUPERBLOCK_CHECKSUM, checksum)
    image.write_bytes(data)


def assert_old_image_is_stamped(cli: Path, root: Path) -> None:
    """An image written before ownership existed is still valid, and the first
    change to it records the default ownership of every path."""
    seed = root / "stamp-seed"
    seed.mkdir()
    (seed / "tool").write_bytes(b"\x7fELF-not-really\n")
    (seed / "notes.txt").write_bytes(b"notes\n")
    manifest = root / "stamp.manifest"
    manifest.write_text(
        f"file\tbin/tool\t{seed / 'tool'}\nfile\tnotes.txt\t{seed / 'notes.txt'}\n"
        f"file\tusers/someone/a.txt\t{seed / 'notes.txt'}\n",
        encoding="utf-8",
    )
    image = root / "stamp.img"
    run([str(cli), "create", str(image), "8192"], root)
    run([str(cli), "apply", str(image), str(manifest)], root)
    strip_ownership(image)

    before = run([str(cli), "check", str(image)], root).stdout
    if "ownership not recorded" not in before:
        raise RuntimeError(f"a pre-ownership image did not check as such: {before!r}")

    empty = root / "stamp-empty.manifest"
    empty.write_text("", encoding="utf-8")
    run([str(cli), "apply", str(image), str(empty)], root)
    after = run([str(cli), "check", str(image)], root).stdout
    if "ownership recorded" not in after:
        raise RuntimeError(f"apply did not stamp a pre-ownership image: {after!r}")

    owners = root / "stamp.owners"
    run([str(cli), "extract", str(image), str(root / "stamp-tree"), str(owners)], root)
    recorded = owners.read_text(encoding="utf-8")
    for expected in (
        "owner\tbin\t0\t0\t40755\n",
        "owner\tbin/tool\t0\t0\t100755\n",
        "owner\tnotes.txt\t0\t0\t100644\n",
        "owner\tusers\t0\t0\t40755\n",
        "owner\tusers/someone\t1000\t100\t40700\n",
        "owner\tusers/someone/a.txt\t1000\t100\t100600\n",
    ):
        if expected not in recorded:
            raise RuntimeError(f"stamped ownership is missing {expected!r}:\n{recorded}")

    bad = root / "stamp-bad.manifest"
    bad.write_text("owner\tnotes.txt\t0\t0\t40755\n", encoding="utf-8")
    refused = subprocess.run(
        [str(cli), "apply", str(image), str(bad)], cwd=root, text=True, capture_output=True, check=False
    )
    if refused.returncode == 0:
        raise RuntimeError("apply accepted a directory mode on a file")


def assert_grown_image_stays_sparse_on_ordinary_sync(cli: Path, sync: Path, root: Path) -> None:
    """An ordinary sync must not de-sparsify a grown image.

    Compaction is the rare path; the ordinary one is what actually runs on every
    build. It copies the image to a candidate before applying the manifest, and
    shutil.copy2 does not preserve holes on Linux, so a grown image would lose
    its sparseness on the very first build: the reserved tail becomes real bytes
    and every later build rewrites all of it. This is the cheaper half of the
    invariant and the one that bites in practice.
    """
    source = root / "sparse-source"
    source.mkdir()
    (source / "one.bin").write_bytes(b"savanxp-ordinary-sync\n" * 40)

    image = root / "sparse.img"
    run([str(cli), "create", str(image), "8192"], root)
    os.truncate(image, image.stat().st_size * 2)
    grown_size = image.stat().st_size
    if allocated_bytes(image) >= grown_size:
        raise RuntimeError("the grown image is not sparse to begin with")

    result = run(
        [
            sys.executable,
            str(sync),
            "--image",
            str(image),
            "--source",
            str(source),
            "--cli",
            str(cli),
            "--sectors",
            "8192",
        ],
        root,
    )
    if image.stat().st_size != grown_size:
        raise RuntimeError(f"an ordinary sync changed the image size: {image.stat().st_size}")
    if allocated_bytes(image) >= grown_size:
        raise RuntimeError("an ordinary sync de-sparsified the reserved tail")

    extracted = root / "sparse-extracted"
    run([str(cli), "extract", str(image), str(extracted)], root)
    if digest(source / "one.bin") != digest(extracted / "one.bin"):
        raise RuntimeError("the applied file differs after an ordinary sync on a grown image")


def assert_sync_retires_renamed_programs(cli: Path, sync: Path, root: Path) -> None:
    """Installing a program retires its other name under bin/.

    A SUN program is bin/<name>.sxe and a POSIX one is bin/<name>, and they are
    the same program. An image from before the rename keeps bin/console next to
    the new bin/console.sxe unless the sync removes it: the shell would run the
    stale copy and the launcher would list it twice. The reverse direction (a
    program that becomes POSIX again) and data outside bin/ are covered too.
    """
    seed = root / "rename-seed"
    source = root / "rename-source"
    (seed / "bin").mkdir(parents=True)
    (seed / "data").mkdir()
    (source / "bin").mkdir(parents=True)
    (source / "data").mkdir()
    for relative in ("bin/console", "bin/tool.sxe", "bin/keep", "data/notes.sxe"):
        (seed / relative).write_bytes(b"old " + relative.encode() + b"\n")
    for relative in ("bin/console.sxe", "bin/tool", "data/notes"):
        (source / relative).write_bytes(b"new " + relative.encode() + b"\n")

    image = root / "rename.img"
    manifest = root / "rename.manifest"
    manifest.write_text(
        "mkdir\tbin\nmkdir\tdata\n"
        + "".join(
            f"file\t{relative}\t{seed / relative}\n"
            for relative in ("bin/console", "bin/tool.sxe", "bin/keep", "data/notes.sxe")
        ),
        encoding="utf-8",
    )
    run([str(cli), "create", str(image), "8192"], root)
    run([str(cli), "apply", str(image), str(manifest)], root)
    run(
        [sys.executable, str(sync), "--image", str(image), "--source", str(source),
         "--cli", str(cli), "--sectors", "8192"],
        root,
    )

    extracted = root / "rename-extracted"
    run([str(cli), "extract", str(image), str(extracted)], root)
    for relative in ("bin/console", "bin/tool.sxe"):
        if (extracted / relative).exists():
            raise RuntimeError(f"the sync kept the replaced program {relative}")
    for relative in ("bin/console.sxe", "bin/tool", "data/notes"):
        if digest(source / relative) != digest(extracted / relative):
            raise RuntimeError(f"the installed file differs: {relative}")
    for relative in ("bin/keep", "data/notes.sxe"):
        if digest(seed / relative) != digest(extracted / relative):
            raise RuntimeError(f"the sync touched an unrelated file: {relative}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--sync", required=True, type=Path)
    args = parser.parse_args()
    cli = args.cli.resolve()
    sync = args.sync.resolve()

    with tempfile.TemporaryDirectory(prefix="savanxp-sxfs-compaction-") as temporary:
        root = Path(temporary)
        seed = root / "seed"
        source = root / "source"
        seed.mkdir()
        source.mkdir()
        names = ("external", "a", "b", "c", "d", "e", "f", "g", "h")
        payload = b"savanxp-compaction-payload\n" * 9200
        for name in names:
            (seed / f"{name}.bin").write_bytes(payload)
        new_payload = b"new-build-file\n" * 42858
        (source / "new.bin").write_bytes(new_payload)
        manifest = root / "seed.manifest"
        manifest.write_text(
            "\n".join(f"file\t{name}.bin\t{seed / (name + '.bin')}" for name in names) + "\n",
            encoding="utf-8",
        )

        image = root / "disk.img"
        run([str(cli), "create", str(image), "8192"], root)
        run([str(cli), "apply", str(image), str(manifest)], root)
        run([str(cli), "rm", str(image), "a.bin", "c.bin", "e.bin", "g.bin"], root)
        before_blocked_sync = digest(image)
        blocked = subprocess.run(
            [
                sys.executable,
                str(sync),
                "--image",
                str(image),
                "--source",
                str(source),
                "--cli",
                str(cli),
                "--sectors",
                "8192",
                "--no-compact",
            ],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        if blocked.returncode != 3 or digest(image) != before_blocked_sync:
            raise RuntimeError("a blocked candidate apply modified the persistent image")

        capacity_source = root / "capacity-source"
        capacity_source.mkdir()
        (capacity_source / "too-large.bin").write_bytes(b"x" * 3000000)
        capacity_image = root / "capacity.img"
        capacity_image.write_bytes(image.read_bytes())
        before_capacity_failure = digest(capacity_image)
        capacity = subprocess.run(
            [
                sys.executable,
                str(sync),
                "--image",
                str(capacity_image),
                "--source",
                str(capacity_source),
                "--cli",
                str(cli),
                "--sectors",
                "8192",
            ],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        if capacity.returncode != 3 or digest(capacity_image) != before_capacity_failure:
            raise RuntimeError("real capacity exhaustion replaced the persistent image")

        sync_result = run(
            [
                sys.executable,
                str(sync),
                "--image",
                str(image),
                "--source",
                str(source),
                "--cli",
                str(cli),
                "--sectors",
                "8192",
            ],
            root,
        )
        if "compactacion segura completada" not in (sync_result.stdout + sync_result.stderr):
            raise RuntimeError("fragmentation test did not exercise safe compaction")

        extracted = root / "extracted"
        run([str(cli), "extract", str(image), str(extracted)], root)
        for name in ("external", "b", "d", "f", "h"):
            expected = seed / f"{name}.bin"
            actual = extracted / f"{name}.bin"
            if digest(expected) != digest(actual):
                raise RuntimeError(f"preserved file differs: {name}")
        if digest(source / "new.bin") != digest(extracted / "new.bin"):
            raise RuntimeError("new build file differs after compaction")
        assert_pending_journal_rejected(cli, image, root)
        if list(root.glob(f"{image.name}.compact-*")) or list(root.glob(f"{image.name}.pre-compact")):
            raise RuntimeError("successful compaction left a staging or recovery file")

        assert_grown_image_survives_compaction(cli, sync, root)
        assert_old_image_is_stamped(cli, root)
        assert_grown_image_stays_sparse_on_ordinary_sync(cli, sync, root)
        assert_sync_retires_renamed_programs(cli, sync, root)

    print("sxfs compaction regression: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
