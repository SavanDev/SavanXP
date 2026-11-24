#!/usr/bin/env python3
"""Check disk selection and the QEMU block-device handoff without a physical disk."""

import errno
import os
import stat
import sys
import unittest
from contextlib import ExitStack
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import run_qemu


class QemuDiskTest(unittest.TestCase):
    device = Path("/dev/test-disk")
    block = SimpleNamespace(st_mode=stat.S_IFBLK, st_rdev=os.makedev(8, 0))

    def test_temporary_device_uses_a_read_only_exclusive_descriptor(self):
        with (patch.object(Path, "stat", return_value=self.block),
              patch.object(os, "open", return_value=11) as open_file,
              patch.object(os, "fstat", return_value=self.block),
              patch.object(os, "pread", return_value=bytes(512) + b"SXFS\0\0\0\0"),
              patch.object(os, "close") as close_file,
              ExitStack() as resources):
            fds = run_qemu.open_disk_device(self.device, False, resources)
            self.assertEqual(fds, (11,))
            open_file.assert_called_once_with(self.device, os.O_RDONLY | os.O_EXCL |
                                              os.O_NOFOLLOW | os.O_NONBLOCK)
            close_file.assert_not_called()
        close_file.assert_called_once_with(11)

    def test_persistent_device_passes_write_and_read_descriptors(self):
        with (patch.object(Path, "stat", return_value=self.block),
              patch.object(os, "open", side_effect=(11, 12)) as open_file,
              patch.object(os, "fstat", return_value=self.block),
              patch.object(os, "pread", return_value=b"SXFS\0\0\0\0"),
              patch.object(os, "close"), ExitStack() as resources):
            fds = run_qemu.open_disk_device(self.device, True, resources)
            self.assertEqual(fds, (11, 12))
            self.assertEqual(open_file.call_args_list[0].args,
                             (self.device, os.O_RDWR | os.O_EXCL | os.O_NOFOLLOW | os.O_NONBLOCK))
            self.assertEqual(open_file.call_args_list[1].args, ("/proc/self/fd/11", os.O_RDONLY))

    def test_in_use_device_is_rejected(self):
        with (patch.object(Path, "stat", return_value=self.block),
              patch.object(os, "open", side_effect=OSError(errno.EBUSY, "busy")),
              ExitStack() as resources, self.assertRaisesRegex(SystemExit, "device is busy")):
            run_qemu.open_disk_device(self.device, True, resources)

    def test_unrecognized_volume_is_rejected(self):
        with (patch.object(Path, "stat", return_value=self.block),
              patch.object(os, "open", return_value=11),
              patch.object(os, "fstat", return_value=self.block),
              patch.object(os, "pread", return_value=bytes(1024)),
              patch.object(os, "close"), ExitStack() as resources,
              self.assertRaisesRegex(SystemExit, "does not contain a raw SxFS volume")):
            run_qemu.open_disk_device(self.device, False, resources)

    def test_qemu_uses_the_claimed_device_and_optional_overlay(self):
        for writable, fds in ((False, (11,)), (True, (11, 12))):
            with self.subTest(writable=writable):
                args = run_qemu.build_qemu_args(
                    image_root=Path("image"), disk_image=self.device, code=Path("code"),
                    vars_copy=Path("vars"), debug_log=Path("debug"), accel="tcg", smp=1,
                    virtio=False, headless=True, debug=False, disk_bus="ahci",
                    disk_fds=fds, disk_snapshot=not writable,
                )
                drive = next(arg for arg in args if arg.startswith("if=none,id=svdisk,"))
                self.assertIn("file=/dev/fdset/0,file.driver=host_device", drive)
                self.assertEqual("snapshot=on" in drive, not writable)
                for fd in fds:
                    self.assertIn(f"fd={fd},set=0", args)


if __name__ == "__main__":
    unittest.main()
