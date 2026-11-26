#!/usr/bin/env python3
"""Check disk selection and the QEMU block-device handoff without a physical disk."""

import errno
import os
import stat
import sys
import tempfile
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

    def test_permission_denial_requests_sudo_for_the_selected_mode(self):
        for writable, fds in ((False, (11,)), (True, (11, 12))):
            with (self.subTest(writable=writable), ExitStack() as resources,
                  patch.object(run_qemu, "claim_disk_fds", side_effect=PermissionError("denied")),
                  patch.object(os, "geteuid", return_value=1000),
                  patch.object(os, "pread", return_value=b"SXFS\0\0\0\0"),
                  patch.object(run_qemu, "sudo_disk_fds", return_value=fds) as sudo):
                self.assertEqual(run_qemu.open_disk_device(self.device, writable, resources, "sudo"), fds)
                sudo.assert_called_once_with(self.device, writable, "sudo", resources)

    def test_sudo_handoff_uses_open_descriptors_without_reopening_the_path(self):
        identity = "device-identity"
        with tempfile.TemporaryDirectory(prefix="savanxp-qemu-fds-") as temporary:
            image = Path(temporary) / "volume"
            image.write_bytes(b"SXFS\0\0\0\0")

            def fake_sudo(command, *, check):
                self.assertEqual(command[:3], ["sudo", "--", sys.executable])
                args = command[command.index("--send-disk-fds") + 1:]
                with image.open("r+b") as writer, image.open("rb") as reader:
                    opened = (writer.fileno(), reader.fileno()) if writable else (reader.fileno(),)
                    with (patch.object(run_qemu, "claim_disk_fds", return_value=opened) as claim,
                          patch.object(run_qemu, "peer_uid", return_value=1000),
                          patch.object(os, "geteuid", return_value=0),
                          patch.dict(os.environ, {"SUDO_UID": "1000"})):
                        self.assertEqual(run_qemu.send_disk_fds(args), 0)
                        self.assertEqual(claim.call_args.args[:2], (image, writable))
                image.unlink()
                return SimpleNamespace(returncode=0)

            for writable in (False, True):
                image.write_bytes(b"SXFS\0\0\0\0")
                with (self.subTest(writable=writable), ExitStack() as resources,
                      patch.object(run_qemu, "device_identity", return_value=identity),
                      patch.object(run_qemu, "peer_uid", return_value=0),
                      patch.object(run_qemu.subprocess, "run", side_effect=fake_sudo)):
                    fds = run_qemu.sudo_disk_fds(image, writable, "sudo", resources)
                    self.assertFalse(image.exists())
                    self.assertEqual(len(fds), 2 if writable else 1)
                    self.assertEqual(os.pread(fds[0], 8, 0), b"SXFS\0\0\0\0")
                    self.assertTrue(all(not os.get_inheritable(fd) for fd in fds))

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

    def test_socket_paths_over_the_unix_limit_are_refused_before_launch(self):
        def build(socket_path):
            return run_qemu.build_qemu_args(
                image_root=Path("image"), disk_image=None, code=Path("code"),
                vars_copy=Path("vars"), debug_log=Path("debug"), accel="tcg", smp=1,
                virtio=False, headless=True, debug=False, monitor_path=socket_path,
            )

        limit = run_qemu.UNIX_SOCKET_PATH_MAX
        fits = Path("/" + "d" * (limit - 1))
        self.assertIn(f"unix:{fits},server=on,wait=off", build(fits))
        too_long = Path("/" + "d" * limit)
        with self.assertRaises(SystemExit) as refused:
            build(too_long)
        self.assertIn(f"{limit + 1} bytes", str(refused.exception))
        self.assertIn(str(too_long), str(refused.exception))


if __name__ == "__main__":
    unittest.main()
