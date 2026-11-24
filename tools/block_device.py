"""Linux block-device I/O for the host launcher and offline tools."""

import errno
import fcntl
import os
import stat
from contextlib import contextmanager
from pathlib import Path


@contextmanager
def claim(path: Path, *, writable: bool):
    expected = path.stat()
    if not stat.S_ISBLK(expected.st_mode):
        raise ValueError(f"{path} is not a block device")
    access = os.O_RDWR if writable else os.O_RDONLY
    try:
        fd = os.open(path, access | os.O_EXCL | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError as error:
        if error.errno == errno.EBUSY:
            raise ValueError(f"device is busy. Unmount {path} and close other users first.") from error
        raise
    try:
        opened = os.fstat(fd)
        if not stat.S_ISBLK(opened.st_mode) or opened.st_rdev != expected.st_rdev:
            raise ValueError(f"{path} changed while opening it")
        yield fd
    finally:
        os.close(fd)


def device_node(device: int) -> Path:
    return Path(f"/sys/dev/block/{os.major(device)}:{os.minor(device)}").resolve(strict=True)


def device_geometry(node: Path) -> tuple[int, int]:
    """Return capacity in bytes and logical sector size."""
    disk = node.parent if (node / "partition").exists() else node
    sectors = int((node / "size").read_text())  # Linux sysfs uses 512-byte units.
    logical_size = int((disk / "queue/logical_block_size").read_text())
    return sectors * 512, logical_size


def geometry(fd: int) -> tuple[int, int]:
    return device_geometry(device_node(os.fstat(fd).st_rdev))


def rescan(fd: int):
    fcntl.ioctl(fd, 0x125F)  # BLKRRPART


def write_at(fd: int, data: bytes, offset: int):
    remaining = memoryview(data)
    while remaining:
        done = os.pwrite(fd, remaining, offset)
        if done == 0:
            raise OSError("short write")
        remaining = remaining[done:]
        offset += done
