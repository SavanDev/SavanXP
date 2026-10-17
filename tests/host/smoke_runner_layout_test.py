#!/usr/bin/env python3
"""Static checks for the Linux smoke runner and initramfs handoff."""

from __future__ import annotations

import argparse
import ast
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()

    build = (root / "build.sh").read_text(encoding="utf-8")
    assert "|smoke|" in build
    assert "tools/run_smoke.py" in build
    assert "--sxfs-cli" in build
    assert "SAVANXP_SMOKE_COMMAND" in build
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    assert 'set(SAVANXP_SMOKE_COMMAND ""' in cmake
    assert "write_smoke_spec.py" in cmake
    assert (root / "tools" / "write_smoke_spec.py").is_file()
    assert (root / "tools" / "run_smoke.py").is_file()
    assert (root / "tools" / "qmp_client.py").is_file()
    assert (root / "tools" / "taskbar_smoke.py").is_file()
    assert (root / "tools" / "tcp_echo_server.py").is_file()
    assert (root / "tools" / "build-user.sh").is_file()
    assert (root / "tools" / "build-user.sh").stat().st_mode & 0o111
    assert (root / "tools" / "run-user.sh").is_file()
    assert (root / "tools" / "run-user.sh").stat().st_mode & 0o111
    assert (root / "tools" / "new-user-app.sh").is_file()
    assert (root / "tools" / "new-user-app.sh").stat().st_mode & 0o111
    assert (root / "tools" / "shoot.sh").is_file()
    assert (root / "tools" / "shoot.sh").stat().st_mode & 0o111
    assert (root / "tools" / "verify_doom_persistence.sh").is_file()
    assert (root / "tools" / "verify_doom_persistence.sh").stat().st_mode & 0o111
    assert not (root / "sdk" / "doomgeneric").exists()
    assert not (root / "sdk" / "ffmpeg").exists()
    assert (root / "tools" / "iso_boot_test.py").is_file()
    assert (root / "tools" / "iso_boot_test.py").stat().st_mode & 0o111
    shoot_script = (root / "tools" / "shoot.sh").read_text(encoding="utf-8")
    assert "shoot_session.py" in shoot_script
    assert "run_qemu.py" in shoot_script
    build_user = (root / "tools" / "build-user.sh").read_text(encoding="utf-8")
    assert "--destination" in build_user
    assert "DESTINATION_RELATIVE" in build_user
    assert 'SAVANXP_CLANG' in build_user
    assert 'SAVANXP_LD' in build_user
    assert '-I "$SOURCE_ROOT"' in build_user
    run_user = (root / "tools" / "run-user.sh").read_text(encoding="utf-8")
    assert "build-user.sh" in run_user
    assert "build.sh" in run_user
    assert (root / "tools" / "smoke_catalog.py").is_file()
    startup = (root / "boot" / "startup.nsh").read_text(encoding="utf-8")
    assert startup == "fs0:\\EFI\\BOOT\\BOOTX64.EFI\n"
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "startup.nsh" in cmake
    build_script = (root / "build.sh").read_text(encoding="utf-8")
    assert "one-word smoke targets" in build_script
    assert "--gpu-soak-iterations" in build_script
    assert "between 1 and 32" in build_script
    runner = (root / "tools" / "run_smoke.py").read_text(encoding="utf-8")
    assert "make_smoke_copy" in runner
    assert "sxfs-cli" in runner
    assert "qmp_driver" in runner
    assert "remove_path" in runner
    run_qemu = (root / "tools" / "run_qemu.py").read_text(encoding="utf-8")
    assert "--qmp-path" in run_qemu
    assert "--serial-log" in run_qemu
    assert "SAVANXP_QEMU" in run_qemu
    assert "run_qmp_callback" in runner
    assert "qmp_callback" in runner
    catalog = (root / "tools" / "smoke_catalog.py").read_text(encoding="utf-8")
    assert '"smoke"' in catalog
    assert '"windowd-smoke"' in catalog
    assert '"ffmpeg-smoke"' in catalog
    assert '"sxfs-smoke"' in catalog
    assert '"kbd-smoke"' in catalog
    assert '"pointer-smoke"' in catalog
    # Sx boundary: the native dispatcher speaks sx:: only and shares no
    # translation unit with process.cpp anymore.
    assert (root / "include" / "kernel" / "sx.hpp").is_file()
    assert (root / "kernel" / "sx.cpp").is_file()
    assert (root / "kernel" / "native_dispatch.cpp").is_file()
    native_dispatch = (root / "kernel" / "native_dispatch.cpp").read_text(encoding="utf-8")
    assert "sx::" in native_dispatch
    assert "copy_user_string_from_current" not in native_dispatch
    # Slice 4: standalone native, no POSIX fallback.
    assert "dispatch_posix_syscall" not in native_dispatch
    assert "SXN_SYS_EXIT" in native_dispatch
    abi = (root / "include" / "abi" / "savanxp_native_abi.h").read_text(encoding="utf-8")
    assert "SXN_SYS_EXIT = 0x1002" in abi
    # Slice 5: spawn/wait lifecycle plus the shared waitpid core.
    assert "SXN_SYS_SPAWN = 0x1003" in abi
    assert "SXN_SYS_WAITPID = 0x1004" in abi
    assert "SXN_SYS_SPAWN" in native_dispatch
    assert "SXN_SYS_WAITPID" in native_dispatch
    posix_dispatch = (root / "subsystems" / "posix" / "kernel" / "posix_dispatch.cpp").read_text(encoding="utf-8")
    assert "waitpid_child" in posix_dispatch
    assert "waitpid_child" in native_dispatch
    # Slice 7: userland surface plus native event/section/wait syscalls.
    assert (root / "include" / "sxn" / "sxn.h").is_file()
    for number in ("SXN_SYS_EVENT_CREATE", "SXN_SYS_EVENT_SET", "SXN_SYS_EVENT_RESET",
                   "SXN_SYS_WAIT_ONE", "SXN_SYS_SECTION_CREATE", "SXN_SYS_MAP_VIEW",
                   "SXN_SYS_UNMAP_VIEW"):
        assert number in abi
        assert number in native_dispatch
    nativetest = (root / "sdk" / "nativetest" / "main.c").read_text(encoding="utf-8")
    assert '#include "sxn/sxn.h"' in nativetest
    assert "sxn_wait_one" in nativetest
    assert "sxn_map_view" in nativetest
    assert "clone_handles_for_spawn" in native_dispatch
    # Slice 8: ring ping-pong over the shared section, rendezvous by design.
    assert "SXN_RING_ROUNDS" in nativetest
    assert "rendezvous" in (root / "docs" / "KERNEL_NATIVE_API.md").read_text(encoding="utf-8")
    # Slice 9: output-only native window inside real windowd.
    assert (root / "sdk" / "nativedemo" / "main.c").is_file()
    demo = (root / "sdk" / "nativedemo" / "main.c").read_text(encoding="utf-8")
    assert "sxnwm_attach" in demo
    assert "sxnwm_poll_input" in demo
    assert "SAVANXP_WM_FD_SECTION" in demo
    assert "savanxp_read" not in demo and "savanxp_open" not in demo
    windowd = (root / "subsystems" / "posix" / "userland" / "windowd.c").read_text(encoding="utf-8")
    assert "windowd_native_client_selftest" in windowd
    assert "/disk/bin/nativedemo" in windowd
    # Slice 10: input ring transport plus the sxnwm seed.
    assert (root / "include" / "sxn" / "sxnwm.h").is_file()
    assert "SAVANXP_WM_FD_INPUT_RING" in (root / "subsystems" / "posix" / "sdk" / "v1" / "include" / "savanxp" / "wm_protocol.h").read_text(encoding="utf-8")
    assert "savanxp_wm_input_ring" in (root / "subsystems" / "posix" / "sdk" / "v1" / "include" / "savanxp" / "syscall.h").read_text(encoding="utf-8")
    assert "route_event_ring" in windowd
    assert "sxnwm_poll_input" in (root / "sdk" / "nativedemo" / "main.c").read_text(encoding="utf-8")
    # Slice 11: widgets seed plus scripted click/type/Esc interaction.
    assert (root / "include" / "sxn" / "sxnwidgets.h").is_file()
    assert (root / "sdk" / "nativewidgets" / "main.c").is_file()
    widgets_test = (root / "sdk" / "nativewidgets" / "main.c").read_text(encoding="utf-8")
    assert "sxnw_field_key" in widgets_test
    assert "windowd_native_widgets_selftest" in windowd
    assert "/disk/bin/nativewidgets" in windowd
    # Slice 12: real PS/2 keyboard through the production path plus launcher.
    assert "windowd_native_hw_selftest" in windowd
    assert "NATIVE HW READY" in windowd
    assert "--hw-native-test" in windowd
    assert "windowd-hw-native" in (root / "subsystems" / "posix" / "userland" / "init.c").read_text(encoding="utf-8")
    assert (root / "tools" / "qmp_native_hw.py").is_file()
    assert "native-hw-smoke" in (root / "tools" / "smoke_catalog.py").read_text(encoding="utf-8")
    assert (root / "sdk" / "nativewidgets" / "nativewidgets.sxres").is_file()
    assert (root / "sdk" / "nativedemo" / "nativedemo.sxres").is_file()
    assert "category=" in (root / "sdk" / "nativewidgets" / "nativewidgets.sxres").read_text(encoding="utf-8")
    assert "native listed" in (root / "subsystems" / "posix" / "userland" / "progman.c").read_text(encoding="utf-8")
    # Slice 6: sections/views/waits converge onto sx::; the translation keeps
    # flags, files, handles and orchestration.
    sx = (root / "kernel" / "sx.cpp").read_text(encoding="utf-8")
    assert "try_consume_objects" in sx
    assert "open_file_section" in sx
    assert "unmap_view" in sx
    process_cpp = (root / "kernel" / "process.cpp").read_text(encoding="utf-8")
    assert "object::create_section" not in process_cpp
    assert "object::acquire_file_range_section" not in process_cpp
    assert "vm::map_section_view" not in process_cpp
    assert "vm::unmap_section_view" not in process_cpp
    assert "object_is_waitable" not in process_cpp
    # Every remaining wait-policy use goes through sx::.
    assert process_cpp.count("can_complete_object_wait") == process_cpp.count("sx::can_complete_object_wait")
    assert process_cpp.count("consume_object_wait") == process_cpp.count("sx::consume_object_wait")
    assert "native_syscall_dispatch.inc" not in process_cpp
    # Slice 3: the POSIX dispatcher is its own unit too.
    assert (root / "subsystems" / "posix" / "kernel" / "posix_dispatch.cpp").is_file()
    assert not (root / "subsystems" / "posix" / "kernel" / "syscall_dispatch.inc").exists()
    assert "subsystems/posix/kernel/syscall_dispatch.inc" not in process_cpp
    posix_dispatch = (root / "subsystems" / "posix" / "kernel" / "posix_dispatch.cpp").read_text(encoding="utf-8")
    assert "sx::negative_error" in posix_dispatch
    assert "smp::this_cpu()" in posix_dispatch
    # Slice 13: POSIX-only bodies live in subsystems/posix, shared cores stay.
    posix_translation = (root / "subsystems" / "posix" / "kernel" / "posix_translation.cpp").read_text(encoding="utf-8")
    for moved in ("fcntl_fd", "poll_fds", "fork_process", "kill_process",
                  "duplicate_fd", "open_node", "create_socket_fd", "seek_fd",
                  "unlink_path", "stat_path", "readdir_node", "ioctl_handle",
                  "read_handle", "write_handle", "create_pipe",
                  "create_timer_handle", "create_semaphore_handle",
                  "wait_on_many_handles", "sleep_via_timer"):
        assert moved in posix_translation
        assert f"int {moved}(" not in process_cpp and f"Process* {moved}(" not in process_cpp
    for shared in ("waitpid_child", "create_event_handle", "wait_on_handle",
                   "create_section_handle", "map_view_handle",
                   "create_process_internal", "clone_handles_for_spawn"):
        assert shared in process_cpp
    assert (root / "include" / "kernel" / "process_detail.hpp").is_file()
    assert "posix_translation" in (root / "include" / "kernel" / "process.hpp").read_text(encoding="utf-8")
    # pointer-smoke es el unico que exige la maquina virtio, y el runner tiene que
    # negarse a arrancarlo sin ella en vez de fallar por el device que falta.
    assert "requires_virtio=True" in catalog
    assert "--requires-virtio" in build_script
    assert "requires_virtio" in runner
    assert "send_pointer_smoke_actions" in runner
    assert '"ac97-stream"' in catalog
    assert '"tcp-smoke"' in catalog
    # mediaplayer-availability y mediaplayer-missing se fueron con SxMedia: se
    # afirman los que el catalogo tiene hoy, no los que tenia cuando se escribio
    # esta linea. Un assert sobre un escenario retirado es un test que no puede
    # volver a pasar, y uno que no pasa no vigila nada.
    assert '"mediaplayer-selftest"' in catalog
    assert '"mediaplayer-display"' in catalog
    assert '"mediaplayer-nolib"' in catalog
    assert 'remove_paths=("lib/libffmpeg.so.0.4",)' in catalog
    qmp_display = (root / "ports" / "ffmpeg" / "tests" / "qmp_display.py").read_text(encoding="utf-8")
    assert "--socket" in qmp_display
    assert 'qmp_callback="./ports/ffmpeg/tests/qmp_display.py"' in catalog
    assert 'port_command="./ports/ffmpeg/smoke.sh"' in catalog
    assert '"gputest --soak 96"' in catalog
    assert "./tools/restore_doom.sh" in catalog
    assert "build-user.sh" in catalog
    assert "taskbar-smoke" in catalog

    for path in (
        root / "tools" / "run_qemu.py",
        root / "tools" / "run_smoke.py",
        root / "tools" / "qmp_client.py",
        root / "tools" / "taskbar_smoke.py",
        root / "tools" / "tcp_echo_server.py",
        root / "tools" / "smoke_catalog.py",
        root / "tools" / "write_smoke_spec.py",
        root / "tests" / "host" / "smoke_runner_layout_test.py",
    ):
        ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    subprocess.run(["bash", "-n", str(root / "tools" / "build-user.sh")], check=True)

    result = subprocess.run(
        ["python3", str(root / "tools" / "smoke_catalog.py"), "smoke", "--field", "success"],
        check=True,
        text=True,
        capture_output=True,
    )
    assert result.stdout.strip() == "SMOKE PASS"

    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "SMOKE"
        subprocess.run(
            [
                "python3",
                str(root / "tools" / "write_smoke_spec.py"),
                "--output",
                str(output),
                "--command",
                "smoke",
            ],
            check=True,
        )
        assert output.read_text(encoding="ascii") == "smoke\n"
        too_long = subprocess.run(
            [
                "python3",
                str(root / "tools" / "write_smoke_spec.py"),
                "--output",
                str(output),
                "--command",
                "x" * 64,
            ],
            capture_output=True,
        )
        assert too_long.returncode != 0
    print("smoke runner layout: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
