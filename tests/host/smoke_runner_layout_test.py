#!/usr/bin/env python3
"""Static checks for the Linux smoke runner and initramfs handoff."""

from __future__ import annotations

import argparse
import ast
import re
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
    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
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
    assert "rendezvous" in (root / "docs" / "kernel" / "KERNEL_NATIVE_API.md").read_text(encoding="utf-8")
    # Slice 9: output-only native window inside real windowd.
    assert (root / "sdk" / "nativedemo" / "main.c").is_file()
    demo = (root / "sdk" / "nativedemo" / "main.c").read_text(encoding="utf-8")
    assert "sxnwm_connect" in demo
    assert "sxnwm_poll_input" in demo
    assert "savanxp_read" not in demo and "savanxp_open" not in demo
    windowd = (root / "system" / "windowd" / "windowd.c").read_text(encoding="utf-8")
    assert "windowd_native_client_selftest" in windowd
    assert "/disk/bin/nativedemo" in windowd
    # Slice 10: input ring transport plus the sxnwm seed.
    assert (root / "include" / "sxn" / "sxnwm.h").is_file()
    assert "SAVANXP_WM_HELLO_INPUT_RING" in (root / "include" / "abi" / "savanxp_abi.h").read_text(encoding="utf-8")
    assert "savanxp_wm_input_ring" in (root / "include" / "abi" / "savanxp_abi.h").read_text(encoding="utf-8")
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
    # Isolation A: the kernel speaks the neutral wire contract, never the POSIX
    # SDK header. This is the guard that keeps it that way.
    kernel_sources = sorted((root / "kernel").glob("*.cpp")) + sorted((root / "kernel").glob("*.hpp"))
    kernel_sources += sorted((root / "include" / "kernel").glob("*.hpp"))
    offenders = [
        path.relative_to(root).as_posix()
        for path in kernel_sources
        if '#include "savanxp/' in path.read_text(encoding="utf-8")
    ]
    assert not offenders, f"kernel must not include the POSIX SDK header: {offenders}"
    # Isolation B: no POSIX per-process state in the kernel. The current
    # directory lives in the state the POSIX subsystem declares.
    assert "char cwd[" not in process_cpp
    process_hpp = (root / "include" / "kernel" / "process.hpp").read_text(encoding="utf-8")
    assert "char cwd[" not in process_hpp
    assert "subsystem_state" in process_hpp
    posix_state = (root / "subsystems" / "posix" / "kernel" / "posix_process_state.hpp").read_text(encoding="utf-8")
    assert "char cwd[" in posix_state
    assert "resolve_path_from_root" in process_cpp
    assert "resolve_path_from_root" in (root / "kernel" / "native_dispatch.cpp").read_text(encoding="utf-8")
    # Isolation C: no poll state in the kernel. The kernel only knows a
    # process is parked with a deadline; which wait it is belongs to the
    # subsystem that parked it.
    for gone in ("poll_user_fds", "poll_count", "poll_entries", "WaitReason::poll",
                 "kMaxPollDescriptors", "evaluate_poll_entries", "commit_poll_entries"):
        assert gone not in process_cpp, f"{gone} still in kernel/process.cpp"
        assert gone not in process_hpp, f"{gone} still in include/kernel/process.hpp"
    subsystem_hpp = (root / "include" / "kernel" / "subsystem.hpp").read_text(encoding="utf-8")
    assert "register_wait_recheck" in subsystem_hpp
    assert "recheck_parked_thread" in subsystem_hpp
    assert "recheck_parked_poll" in posix_state
    assert "register_wait_recheck" in (root / "subsystems" / "posix" / "kernel" / "posix_dispatch.cpp").read_text(encoding="utf-8")
    # Isolation D: no POSIX signals in the kernel. This kernel has no signal
    # delivery at all, so the pending flags were write-only state whose only
    # purpose was naming POSIX signals; the signal vocabulary is POSIX-side.
    # Prose about what was removed is fine; code is not. Comments and docs
    # legitimately mention the old names.
    process_code = "\n".join(
        line.split("//", 1)[0] for line in process_cpp.split("\n")
    )
    for gone in ("SignalFlags", "signal_sigint", "signal_sigkill", "signal_sigchld",
                 "pending_signals", "last_signal", "signal_flag_for"):
        assert gone not in process_code, f"{gone} still in kernel/process.cpp code"
        assert gone not in process_hpp, f"{gone} still in include/kernel/process.hpp"
    posix_signals = (root / "subsystems" / "posix" / "kernel" / "posix_signals.hpp").read_text(encoding="utf-8")
    assert "posix_signal_sigterm" in posix_signals
    assert "posix_signal_flag_for" in (root / "subsystems" / "posix" / "kernel" / "posix_translation.cpp").read_text(encoding="utf-8")
    abi_header = (root / "include" / "abi" / "savanxp_abi.h").read_text(encoding="utf-8")
    # Isolation E: the error space belongs to the kernel, values unchanged.
    kernel_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in list((root / "kernel").glob("*.cpp")) + list((root / "include" / "kernel").glob("*.hpp"))
    )
    assert "savanxp_error_code" not in kernel_text
    # No POSIX error constant either (SAVANXP_EDITION / SAVANXP_EVENT_* are not
    # errors and are allowed to stay).
    for line in kernel_text.split("\n"):
        if "SAVANXP_E" in line:
            rest = line.split("SAVANXP_E", 1)[1]
            assert rest.startswith(("DITION", "VENT_", "XP_")), f"POSIX error name in kernel: {line.strip()[:70]}"
    assert "enum sx_status" in abi_header
    assert "typedef enum sx_status savanxp_error_code" in abi_header
    assert "#define SAVANXP_EINVAL SX_EINVAL" in abi_header
    # The values must not move: that is what makes this observable as a no-op.
    assert "SX_EINVAL = 22" in abi_header and "SX_EBADF = 9" in abi_header
    assert "SX_ENOSYS = 38" in abi_header and "SX_ECONNRESET = 104" in abi_header
    # The POSIX subsystem layer itself legitimately speaks the POSIX surface;
    # nobody else may. This is the part that keeps the SDK header from
    # creeping back into the kernel through a shared driver.
    # Only the POSIX subsystem speaks the POSIX surface. The scanner above
    # walks the whole tree, including build/ copies; restrict it to sources.
    sdk_includers = {
        path.relative_to(root).as_posix()
        for pattern in ("*.c", "*.cpp", "*.h", "*.hpp")
        for top in ("kernel", "include", "subsystems", "sdk", "tests")
        for path in sorted((root / top).rglob(pattern))
        if '#include "savanxp/' in path.read_text(encoding="utf-8", errors="ignore")
    }
    stray = [
        path for path in sdk_includers
        if not (
            path.startswith("subsystems/posix/")
            or path.startswith("sdk/")
            or path.startswith("ports/")
            or path.startswith("tests/host/")
            or path.startswith("include/sxn/")
        )
    ]
    assert not stray, f"only the POSIX subsystem may include the POSIX SDK: {sorted(stray)}"
    assert (root / "include" / "abi" / "savanxp_abi.h").is_file()
    sdk_header = (root / "subsystems" / "posix" / "sdk" / "v1" / "include" / "savanxp" / "syscall.h").read_text(encoding="utf-8")
    # The POSIX vocabulary lives in the SDK; the data contract in the kernel's ABI.
    assert '#include "abi/savanxp_abi.h"' in sdk_header
    for posix_only in ("savanxp_syscall_number", "savanxp_mode_bits",
                       "savanxp_seek_whence", "savanxp_fcntl_command"):
        assert posix_only in sdk_header
        assert posix_only not in abi_header
    # Vocabularies the kernel still consumes as DATA. Isolation slices B-D
    # remove them from the kernel; the guard below keeps them named, so the
    # remaining debt is visible instead of forgotten.
    for shared_data in ("savanxp_open_flags", "savanxp_poll_events",
                        "savanxp_signal_number", "savanxp_pollfd",
                        "savanxp_socket_domain", "savanxp_socket_type",
                        "savanxp_process_state"):
        assert shared_data in abi_header
    # The runner must not leak a TCG machine when it dies before its cleanup.
    # stop_process lives in a finally, and a finally does not run when the
    # process is killed -- so QEMU gets PR_SET_PDEATHSIG. A/B: without it a
    # killed runner leaves a VM burning a core for hours, and the accumulated
    # leftovers slow every scenario after them.
    runner_text = (root / "tools" / "run_smoke.py").read_text(encoding="utf-8")
    assert "preexec_fn=_die_with_parent" in runner_text
    assert "PR_SET_PDEATHSIG" in runner_text
    assert "libc.prctl" in runner_text
    # A PIO command that fails must be re-issued whole, not resumed: the
    # command is half-done and only a fresh select_drive + command clears the
    # port. Bounded, and absent from the happy path.
    ata_text = (root / "kernel" / "ata.cpp").read_text(encoding="utf-8")
    assert "constexpr uint32_t kMaxChunkAttempts = 3;" in ata_text
    assert "rw_chunk_once" in ata_text
    assert re.search(r"bool rw_chunk\([^)]*\)[\s\S]{0,600}for \(uint32_t attempt = 1;", ata_text)
    assert "if (attempt >= kMaxChunkAttempts)" in ata_text
    # Agotados los reintentos baratos, el dispositivo estaba wedged y hace falta
    # reset por software (SRST): dos escrituras, bus y luego dispositivo
    # seleccionado, y re-IDENTIFY porque el cacheado deja de valer.
    assert "void software_reset(Slot& slot)" in ata_text
    assert "kControlSrst = 0x04" in ata_text
    assert "kControlSrstNien = 0x06" in ata_text
    assert "if (!identify(slot))" in ata_text
    assert re.search(r"for \(uint32_t round = 0; round < kMaxChunkRounds;", ata_text)
    assert "kPollSpinBudget = 1000000" in ata_text
    assert "ANDAMIO" not in ata_text, "test scaffolding left in the ATA driver"
    # The SXE stamp must rerun when the neutral contract changes.
    assert '"${CMAKE_SOURCE_DIR}/include/abi/savanxp_abi.h"' in cmake_text
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
