#!/usr/bin/env python3
"""Create, update, and safely compact the persistent SavanXP SxFS image."""

from __future__ import annotations

import argparse
import fcntl
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str]) -> int:
    print("+", " ".join(command), flush=True)
    return subprocess.run(command, check=False).returncode


def run_capture(command: list[str]) -> tuple[int, str]:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr, flush=True)
    return result.returncode, result.stdout


def manifest_for(source: Path) -> tuple[str, set[str]]:
    """Build an sxfs-cli manifest and return the file paths it produces."""
    lines = ["mkdir\tbin", "mkdir\ttmp"]
    produced: set[str] = set()
    entries: list[tuple[str, Path]] = []
    for current, directories, files in os.walk(source):
        current_path = Path(current)
        directories.sort()
        files.sort()
        for name in directories + files:
            path = current_path / name
            relative = path.relative_to(source).as_posix()
            entries.append((relative, path))
    entries.sort(key=lambda item: item[0])

    for relative, path in entries:
        if path.is_symlink():
            raise SystemExit(f"sxfs: symlinks are not supported: {path}")
        if path.is_dir():
            lines.append(f"mkdir\t{relative}")
        elif path.is_file():
            lines.append(f"file\t{relative}\t{path}")
            produced.add(relative)
        else:
            raise SystemExit(f"sxfs: unsupported filesystem entry: {path}")
    return "\n".join(lines) + "\n", produced


def remove_produced_files(carry_root: Path, produced: set[str]) -> None:
    """Drop build-owned files, but retain directories that contain user data."""
    for relative in sorted(produced, key=lambda value: (value.count("/"), value), reverse=True):
        path = carry_root / relative
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.exists() and not path.is_dir():
            raise SystemExit(f"sxfs: carryover path is not a regular file: {path}")


def fsync_file(path: Path) -> None:
    with path.open("rb") as stream:
        os.fsync(stream.fileno())


def fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def make_sibling(path: Path, prefix: str, suffix: str) -> Path:
    descriptor, name = tempfile.mkstemp(prefix=prefix, suffix=suffix, dir=str(path.parent))
    os.close(descriptor)
    return Path(name)


def image_total_sectors(cli: Path, image: Path) -> int | None:
    result, output = run_capture([str(cli), "info", str(image)])
    if result != 0:
        return None
    for line in reversed(output.splitlines()):
        try:
            sectors = int(line.strip())
        except ValueError:
            continue
        if sectors > 0:
            return sectors
    print("sxfs: sxfs-cli info no devolvio un tamano valido", file=sys.stderr)
    return None


def recovery_path(image: Path) -> Path:
    return image.with_name(image.name + ".pre-compact")


def make_recovery_copy(image: Path) -> Path:
    backup = recovery_path(image)
    if backup.exists():
        raise RuntimeError(f"sxfs: ya existe una recuperacion sin revisar: {backup}")
    # A hard link would be cheaper, but an older QEMU or external tool could
    # still write through the original path. A separate copy keeps the recovery
    # image independent even if another process has the disk open.
    shutil.copy2(image, backup)
    fsync_file(backup)
    return backup


def restore_recovery(backup: Path, image: Path) -> bool:
    if not backup.exists():
        return False
    os.replace(backup, image)
    fsync_directory(image.parent)
    return True


def install_candidate(candidate: Path, image: Path, cli: Path) -> int:
    """Atomically install a validated sibling, retaining a rollback copy."""
    if run([str(cli), "check", str(candidate)]) != 0:
        return 1
    fsync_file(candidate)
    backup: Path | None = None
    replaced = False
    try:
        if image.exists():
            backup = make_recovery_copy(image)
            fsync_directory(image.parent)
        os.replace(candidate, image)
        replaced = True
        fsync_directory(image.parent)
        if run([str(cli), "check", str(image)]) != 0:
            print("sxfs: la validacion post-instalacion fallo; restaurando la imagen anterior", file=sys.stderr)
            if backup is not None and not restore_recovery(backup, image):
                print("sxfs: no se encontro la copia de recuperacion", file=sys.stderr)
                return 1
            if backup is None:
                image.unlink(missing_ok=True)
            backup = None
            replaced = False
            return 1
        if backup is not None:
            backup.unlink()
            backup = None
        fsync_directory(image.parent)
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"sxfs: no se pudo instalar la imagen candidata: {exc}", file=sys.stderr)
        if replaced and backup is not None:
            try:
                restore_recovery(backup, image)
                backup = None
            except OSError as restore_error:
                print(f"sxfs: no se pudo restaurar la imagen anterior: {restore_error}", file=sys.stderr)
        elif not replaced and backup is not None:
            try:
                backup.unlink()
                backup = None
            except OSError:
                pass
        return 1


def compact_image(
    image: Path, cli: Path, produced: set[str], sectors: int, source_manifest: str
) -> int:
    """Rebuild a same-size image with carryover and current build data."""
    staging = make_sibling(image, f"{image.name}.compact-", ".img")
    carry_root = Path(tempfile.mkdtemp(prefix="savanxp-sxfs-carry-", dir=str(image.parent)))
    carry_manifest_path: Path | None = None
    success = False
    try:
        if run([str(cli), "extract", str(image), str(carry_root)]) != 0:
            return 1
        remove_produced_files(carry_root, produced)
        carry_manifest, _ = manifest_for(carry_root)
        combined_manifest = carry_manifest + source_manifest
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", prefix="savanxp-sxfs-carry-", suffix=".manifest", delete=False
        ) as handle:
            carry_manifest_path = Path(handle.name)
            handle.write(combined_manifest)

        if run([str(cli), "create", str(staging), str(sectors)]) != 0:
            return 1
        result = run([str(cli), "apply", str(staging), str(carry_manifest_path)])
        if result != 0:
            return result
        if run([str(cli), "check", str(staging)]) != 0:
            return 1
        if install_candidate(staging, image, cli) != 0:
            return 1
        success = True
        print(f"sxfs: compactacion segura completada; imagen preservada en {image}")
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"sxfs: compactacion fallo: {exc}", file=sys.stderr)
        return 1
    finally:
        if carry_manifest_path is not None:
            carry_manifest_path.unlink(missing_ok=True)
        if success:
            shutil.rmtree(carry_root, ignore_errors=True)
        else:
            staging.unlink(missing_ok=True)
            if carry_root.exists():
                print(f"sxfs: carryover de recuperacion conservado en {carry_root}", file=sys.stderr)


def copy_candidate(image: Path) -> Path:
    candidate = make_sibling(image, f"{image.name}.candidate-", ".img")
    try:
        shutil.copy2(image, candidate)
        fsync_file(candidate)
    except OSError:
        candidate.unlink(missing_ok=True)
        raise
    return candidate


def sync_image(args: argparse.Namespace, image: Path, source: Path, cli: Path) -> int:
    image.parent.mkdir(parents=True, exist_ok=True)
    if recovery_path(image).exists():
        print(f"sxfs: revisar la recuperacion pendiente antes de sincronizar: {recovery_path(image)}", file=sys.stderr)
        return 1
    if args.reset and image.exists():
        image.unlink()

    manifest, produced = manifest_for(source)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", prefix="savanxp-sxfs-", suffix=".manifest", delete=False
    ) as handle:
        manifest_path = Path(handle.name)
        handle.write(manifest)
    try:
        if not image.exists():
            candidate = make_sibling(image, f"{image.name}.candidate-", ".img")
            try:
                if run([str(cli), "create", str(candidate), str(args.sectors)]) != 0:
                    return 1
                result = run([str(cli), "apply", str(candidate), str(manifest_path)])
                if result != 0:
                    return result
                return install_candidate(candidate, image, cli)
            finally:
                candidate.unlink(missing_ok=True)

        if run([str(cli), "check", str(image)]) != 0:
            print("sxfs: existing image was not modified", file=sys.stderr)
            return 1

        candidate = copy_candidate(image)
        try:
            result = run([str(cli), "apply", str(candidate), str(manifest_path)])
            if result == 0:
                return install_candidate(candidate, image, cli)
            if result != 3:
                print("sxfs: apply failed; the original image was not modified", file=sys.stderr)
                return result
            if args.no_compact:
                print("sxfs: no contiguous free run; compaction disabled by --no-compact", file=sys.stderr)
                return 3
        finally:
            candidate.unlink(missing_ok=True)

        sectors = image_total_sectors(cli, image)
        if sectors is None:
            return 1
        print("sxfs: no hay corrida contigua; compactando de forma segura")
        return compact_image(image, cli, produced, sectors, manifest)
    finally:
        manifest_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--sectors", type=int, default=131072)
    parser.add_argument("--no-compact", action="store_true", help="fail instead of compacting on exit 3")
    parser.add_argument(
        "--reset",
        action="store_true",
        help="explicitly remove an existing image before creating it",
    )
    args = parser.parse_args()

    image = args.image.resolve()
    source = args.source.resolve()
    cli = args.cli.resolve()
    if not source.is_dir():
        raise SystemExit(f"sxfs: source directory does not exist: {source}")
    if not cli.is_file():
        raise SystemExit(f"sxfs: missing sxfs-cli: {cli}")

    image.parent.mkdir(parents=True, exist_ok=True)
    lock_path = image.with_name(image.name + ".lock")
    with lock_path.open("a+") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        return sync_image(args, image, source, cli)


if __name__ == "__main__":
    raise SystemExit(main())
