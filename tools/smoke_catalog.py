#!/usr/bin/env python3
"""Declarative catalog shared by the Linux smoke runner and build.sh."""

from __future__ import annotations

import argparse
from dataclasses import dataclass


@dataclass(frozen=True)
class Scenario:
    command: str
    success: str
    failure: str
    timeout: float = 120.0
    description: str = ""
    prepare: tuple[str, ...] = ()
    ready: str = ""
    qmp_driver: str = ""
    capture_fixture: bool = False
    clean_audio_preferences: bool = False
    audio_device: str = "auto"
    wav_name: str = ""
    wav_check: str = ""
    completion: str = "serial"
    ready_wait: float = 0.0
    host_server: str = ""
    host_test: str = ""
    remove_paths: tuple[str, ...] = ()
    qmp_callback: str = ""
    port_command: str = ""
    unsupported: str = ""
    host_action: str = ""
    visual_scenario: str = ""
    requires_virtio: bool = False
    no_network: bool = False
    no_disk: bool = False
    extra_disk_mib: int = 0
    fat_disk_mib: int = 0
    usb: bool = False
    usb_stick_mib: int = 0
    no_ps2: bool = False
    usb_pointer: str = "mouse"
    usb_hub: bool = False
    usb_host: str = "xhci"
    # The user's own Windows files the scenario needs, by name, from the
    # folder SAVANXP_WIN32_FIXTURES names: copied into the disposable disk
    # under /system/win/drive_c/tests, or with NAME@DIR under drive_c/DIR,
    # never into the build. Without them the scenario is skipped (exit 77),
    # not failed.
    win32_files: tuple[str, ...] = ()


def _qemu_scenario(
    command: str,
    success: str,
    failure: str,
    description: str,
    timeout: float = 180.0,
    ready: str = "",
    qmp_driver: str = "",
    capture_fixture: bool = False,
    clean_audio_preferences: bool = False,
    audio_device: str = "auto",
    wav_name: str = "",
    wav_check: str = "",
    completion: str = "serial",
    ready_wait: float = 0.0,
    host_server: str = "",
    prepare: tuple[str, ...] = (),
    remove_paths: tuple[str, ...] = (),
    qmp_callback: str = "",
    host_action: str = "",
    visual_scenario: str = "",
    requires_virtio: bool = False,
    no_network: bool = False,
    no_disk: bool = False,
    extra_disk_mib: int = 0,
    fat_disk_mib: int = 0,
    usb: bool = False,
    usb_stick_mib: int = 0,
    no_ps2: bool = False,
    usb_pointer: str = "mouse",
    usb_hub: bool = False,
    usb_host: str = "xhci",
    win32_files: tuple[str, ...] = (),
) -> Scenario:
    return Scenario(
        command=command,
        success=success,
        failure=failure,
        timeout=timeout,
        description=description,
        ready=ready,
        qmp_driver=qmp_driver,
        audio_device=audio_device,
        capture_fixture=capture_fixture,
        clean_audio_preferences=clean_audio_preferences,
        wav_name=wav_name,
        wav_check=wav_check,
        completion=completion,
        ready_wait=ready_wait,
        host_server=host_server,
        prepare=prepare,
        remove_paths=remove_paths,
        qmp_callback=qmp_callback,
        host_action=host_action,
        visual_scenario=visual_scenario,
        requires_virtio=requires_virtio,
        no_network=no_network,
        no_disk=no_disk,
        extra_disk_mib=extra_disk_mib,
        fat_disk_mib=fat_disk_mib,
        usb=usb,
        usb_stick_mib=usb_stick_mib,
        no_ps2=no_ps2,
        usb_pointer=usb_pointer,
        usb_hub=usb_hub,
        usb_host=usb_host,
        win32_files=win32_files,
    )


def _host_scenario(test_name: str, description: str) -> Scenario:
    return Scenario(
        command="host",
        success="PASS",
        failure="FAIL",
        description=description,
        host_test=test_name,
    )


def _unsupported(description: str, reason: str) -> Scenario:
    return Scenario(
        command="unsupported",
        success="UNSUPPORTED PASS",
        failure="UNSUPPORTED FAIL",
        description=description,
        unsupported=reason,
    )


SCENARIOS: dict[str, Scenario] = {
    "smoke": _qemu_scenario(
        "smoke", "SMOKE PASS", "SMOKE FAIL", "Core POSIX userland smoke test", 120.0
    ),
    # The kernel panics on purpose (POWER_IOC_PANIC) with every core running;
    # the panic must stop the others by NMI and say so.
    "panic-smoke": _qemu_scenario(
        "panictest",
        "smp: other cores stopped",
        "smp: other cores NOT stopped",
        "A kernel panic halts every core",
        120.0,
    ),
    # The panic text is what the screen shows, whichever DISPI buffer was
    # visible and whatever mode was set (FB4,
    # docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md). panictest leaves a magenta
    # frame on screen, then panics; tools/panic_display_smoke.py checks the
    # screenshot.
    "panic-display-front": _qemu_scenario(
        "panictest front",
        "smp: other cores stopped",
        "PANICTEST FAIL",
        "Panic text visible with the first DISPI buffer shown",
        120.0,
        ready="smp: other cores stopped",
        qmp_callback="tools/panic_display_smoke.py",
    ),
    "panic-display-back": _qemu_scenario(
        "panictest back",
        "smp: other cores stopped",
        "PANICTEST FAIL",
        "Panic text visible with the second DISPI buffer shown",
        120.0,
        ready="smp: other cores stopped",
        qmp_callback="tools/panic_display_smoke.py",
    ),
    "panic-display-single": _qemu_scenario(
        "panictest single",
        "smp: other cores stopped",
        "PANICTEST FAIL",
        "Panic text visible in a single-buffered mode other than the firmware's",
        120.0,
        ready="smp: other cores stopped",
        qmp_callback="tools/panic_display_smoke.py",
    ),
    "panic-display-released": _qemu_scenario(
        "panictest released",
        "smp: other cores stopped",
        "PANICTEST FAIL",
        "Panic text visible after the graphics session was released",
        120.0,
        ready="smp: other cores stopped",
        qmp_callback="tools/panic_display_smoke.py",
    ),
    "windowd-smoke": _qemu_scenario(
        "windowd-selftest",
        "WINDOWD SMOKE PASS",
        "WINDOWD SMOKE FAIL",
        "Window manager and compositor self-test",
        prepare=(
            "./tools/build-user.sh --source sdk/nativedemo --name nativedemo --native",
            "./tools/build-user.sh --source sdk/nativewidgets --name nativewidgets --native",
            "./tools/build-user.sh --source sdk/sungui --name sungui --native --gui",
        ),
    ),
    # This smoke copies a whole program (390 KB) to get a fixture with .sxmeta,
    # and prints how long the copy took. It used to take 365930 ms under TCG,
    # when every metadata commit rewrote the whole metadata region over PIO;
    # with sparse journals and AHCI it is a few seconds under KVM, most of it
    # host fsyncs (see docs/roadmaps/open/SXFS_ROADMAP.md). The 900 s timeout is kept as
    # margin for TCG on a loaded host and for SAVANXP_DISK_BUS=ide.
    "progman-smoke": _qemu_scenario(
        "progman-selftest",
        "PROGMAN SMOKE PASS",
        "PROGMAN SMOKE FAIL",
        "Program Manager registry self-test",
        900.0,
        prepare=(
            "./tools/restore_doom.sh",
            "./tools/build-user.sh --source sdk/nativedemo --name nativedemo --native",
            "./tools/build-user.sh --source sdk/nativewidgets --name nativewidgets --native",
        ),
    ),
    "native-hw-smoke": _qemu_scenario(
        "windowd-hw-native",
        "NATIVE HW PASS",
        "NATIVE HW FAIL",
        "Real PS/2 keys through the production path into a native form",
        180.0,
        "NATIVE HW READY",
        qmp_callback="./tools/qmp_native_hw.py",
        prepare=(
            "./tools/build-user.sh --source sdk/nativewidgets --name nativewidgets --native",
        ),
    ),
    "appwiz-smoke": _qemu_scenario(
        "appwiz-selftest",
        "APPWIZ SMOKE PASS",
        "APPWIZ SMOKE FAIL",
        "Application Wizard self-test",
        prepare=("./tools/restore_doom.sh",),
    ),
    "taskmgr-smoke": _qemu_scenario(
        "taskmgr-selftest",
        "TASKMGR SMOKE PASS",
        "TASKMGR SMOKE FAIL",
        "Task Manager process and CPU self-test",
    ),
    "logview-smoke": _qemu_scenario(
        "logview-selftest",
        "LOGVIEW SMOKE PASS",
        "LOGVIEW SMOKE FAIL",
        "Log Viewer: reads the system log and files it by subsystem and program",
    ),
    "diskmgmt-smoke": _qemu_scenario(
        "diskmgmt-selftest",
        "DISKMGMT SMOKE PASS",
        "DISKMGMT SMOKE FAIL",
        "Disk Management: listing, then initialize, partition, format and delete on a blank disk, "
        "and mount a FAT disk and a FAT USB stick",
        timeout=300.0,
        extra_disk_mib=64,
        fat_disk_mib=16,
        usb_stick_mib=16,
    ),
    "mines-smoke": _qemu_scenario(
        "mines-selftest",
        "MINES SMOKE PASS",
        "MINES SMOKE FAIL",
        "Mines game and persistent settings self-test",
    ),
    "paint-smoke": _qemu_scenario(
        "paint-selftest",
        "PAINT SMOKE PASS",
        "PAINT SMOKE FAIL",
        "Paint BMP round-trip self-test",
    ),
    "calc-smoke": _qemu_scenario(
        "calc-selftest",
        "CALC SMOKE PASS",
        "CALC SMOKE FAIL",
        "Calculator state-machine self-test",
    ),
    "clock-smoke": _qemu_scenario(
        "clocktest",
        "CLOCK SMOKE PASS",
        "CLOCK SMOKE FAIL",
        "Clock and RTC self-test",
    ),
    "date-smoke": _qemu_scenario(
        "datetest",
        "DATE SMOKE PASS",
        "DATE SMOKE FAIL",
        "RTC set, reject and restore self-test",
    ),
    "ntp-smoke": _qemu_scenario(
        "ntptest {port}",
        "NTP SMOKE PASS",
        "NTP SMOKE FAIL",
        "SNTP sync against a host fixture",
        180.0,
        host_server="ntp",
    ),
    "sxe-smoke": _qemu_scenario(
        "sxe-selftest",
        "SXE SMOKE PASS",
        "SXE SMOKE FAIL",
        "SXE resource parser and metadata self-test",
    ),
    "filesapp-smoke": _qemu_scenario(
        "filesapp-selftest",
        "FILESAPP SMOKE PASS",
        "FILESAPP SMOKE FAIL",
        "File manager associations and icon self-test",
    ),
    "net-smoke": _qemu_scenario(
        "netsmoke",
        "NET SMOKE PASS",
        "NET SMOKE FAIL",
        "User-network ICMP and ARP self-test",
    ),
    "cursor-fallback-smoke": _qemu_scenario(
        "cursor-fallback-selftest", "CURSOR SMOKE PASS", "CURSOR SMOKE FAIL",
        "Missing classic pack keeps the compositor usable with an emergency arrow",
        remove_paths=("/users/user/Settings/cursors.cfg",) + tuple(f"/system/cursors/classic/{name}.cur" for name in
                           ("arrow", "wait", "text", "move", "resize_h", "resize_v", "unavailable", "link", "resize_nwse", "resize_nesw")),
    ),
    "cursor-smoke": _qemu_scenario(
        "cursor-selftest", "CURSOR SMOKE PASS", "CURSOR SMOKE FAIL",
        "Classic, Retrosmart and Hackneyed CUR loading and scanout pixels",
        ready="CURSOR SMOKE PASS", qmp_callback="tools/cursor_smoke.py",
        remove_paths=("/users/user/Settings/cursors.cfg",),
    ),
    "effects-smoke": _qemu_scenario(
        "", "EFFECTS SMOKE PASS", "EFFECTS SMOKE FAIL",
        "Display effects: independent shadows, Apply/Cancel and restart persistence",
        900.0, "handoff: starting /bin/init", "taskbar", completion="host",
        ready_wait=45.0, host_action="visual", visual_scenario="effects",
    ),
    "cursor-settings-smoke": _qemu_scenario(
        "", "CURSOR SETTINGS SMOKE PASS", "CURSOR SETTINGS SMOKE FAIL",
        "Mouse Properties schemes, licenses, pointer speed, live changes, cancel and restart persistence",
        540.0, "handoff: starting /bin/init", "taskbar", completion="host",
        ready_wait=45.0, host_action="visual", visual_scenario="cursor_settings",
        remove_paths=("/users/user/Settings/cursors.cfg",),
    ),
    "cursor-repro": _qemu_scenario(
        "windowd-cursor-repro",
        "CURSOR REPRO PASS",
        "CURSOR REPRO FAIL",
        "Window cursor presentation regression",
    ),
    # El unico escenario que mueve el puntero de verdad a traves del device:
    # cursor-repro inyecta savanxp_mouse_event a mano y no toca la cola de
    # virtio-input, asi que una cola que se repone contra una direccion de notify
    # equivocada -- que es lo que colgaba la VM al entrar el raton en la ventana
    # -- no la veia nadie. Necesita la maquina virtio: con PS/2 no hay puntero
    # absoluto, y sin el flag el invitado no tiene posicion que afirmar.
    "pointer-smoke": _qemu_scenario(
        "mousetest --selftest",
        "POINTER SMOKE PASS",
        "POINTER SMOKE FAIL",
        "virtio-tablet position and button path",
        120.0,
        ready="POINTER SMOKE READY",
        qmp_driver="pointer",
        requires_virtio=True,
    ),
    "gpu-soak": _qemu_scenario(
        "gputest --soak 96",
        "SOAK PASS",
        "SOAK FAIL",
        "GPU presentation and mode-switch soak",
        360.0,
    ),
    "ac97-stream": _qemu_scenario(
        "audiostream",
        "AUDIO STREAM PASS",
        "AUDIO STREAM FAIL",
        "AC'97 streaming audio",
        120.0,
        audio_device="ac97",
        wav_name="ac97-stream.wav",
    ),
    "ac97-stream-quiet": _qemu_scenario(
        "audiostream-quiet",
        "AUDIO STREAM QUIET PASS",
        "AUDIO STREAM FAIL",
        "AC'97 streaming audio at master volume 50",
        120.0,
        audio_device="ac97",
        wav_name="ac97-stream-quiet.wav",
    ),
    "hda-stream": _qemu_scenario(
        "audiostream", "AUDIO STREAM PASS", "AUDIO STREAM FAIL",
        "OpenBSD azalia playback, asserted on recorded PCM", 120.0,
        audio_device="hda", wav_name="hda-stream.wav", wav_check="tone",
    ),
    "hda-stream-quiet": _qemu_scenario(
        "audiostream-quiet", "AUDIO STREAM QUIET PASS", "AUDIO STREAM FAIL",
        "OpenBSD azalia playback at volume 50, asserted on recorded PCM", 120.0,
        audio_device="hda", wav_name="hda-stream-quiet.wav", wav_check="tone-quiet",
    ),
    "hda-capture": _qemu_scenario(
        "nativecapture", "NATIVE CAPTURE PASS", "NATIVE CAPTURE FAIL",
        "Known stereo microphone PCM through native HDA capture, gain/mute and duplex",
        audio_device="hda", capture_fixture=True, no_network=True, wav_name="hda-capture.wav", wav_check="duplex",
        prepare=("./tools/build-user.sh --source sdk/nativetest --name nativetest --native",),
    ),
    "audio-preferences": _qemu_scenario(
        "nativepreferences", "NATIVE PREFERENCES PASS", "NATIVE PREFERENCES FAIL",
        "Durable audio defaults and application identity across daemon restart",
        audio_device="multi", capture_fixture=True, clean_audio_preferences=True, no_network=True,
        wav_name="audio-preferences.wav", wav_check="preferences",
        prepare=("./tools/build-user.sh --source sdk/nativetest --name nativetest --native",),
    ),
    "audio-devices": _qemu_scenario(
        "nativedevices", "NATIVE DEVICES PASS", "NATIVE DEVICES FAIL",
        "Independent playback/capture across HDA codecs/controllers, AC97 and virtio",
        audio_device="multi", capture_fixture=True, no_network=True,
        wav_name="audio-devices.wav", wav_check="duplex",
        prepare=("./tools/build-user.sh --source sdk/nativetest --name nativetest --native",),
    ),
    "virtio-capture": _qemu_scenario(
        "nativecapture", "NATIVE CAPTURE PASS", "NATIVE CAPTURE FAIL",
        "Known stereo microphone PCM through native virtio capture",
        audio_device="virtio", capture_fixture=True, no_network=True, wav_name="virtio-capture.wav", wav_check="duplex",
        prepare=("./tools/build-user.sh --source sdk/nativetest --name nativetest --native",),
    ),
    "hda-record": _qemu_scenario(
        "audiorecord", "AUDIO RECORD PASS", "AUDIO RECORD FAIL",
        "Legacy HDA capture read completion (not signal acceptance)",
        audio_device="hda",
    ),
    "virtio-record": _qemu_scenario(
        "audiorecord", "AUDIO RECORD PASS", "AUDIO RECORD FAIL",
        "Legacy virtio-sound capture read completion (not signal acceptance)",
        audio_device="virtio",
    ),
    "audiod-smoke": _qemu_scenario(
        "audiodselftest",
        "AUDIOD SELFTEST PASS",
        "AUDIOD SELFTEST FAIL",
        "Audio daemon mixes two loopback clients",
        120.0,
        audio_device="ac97",
        wav_name="audiod-smoke.wav",
    ),
    "volume-smoke": _qemu_scenario(
        "volumesmoke",
        "SMOKE PASS",
        "SMOKE FAIL",
        "volume CLI sets master volume and persists it",
        120.0,
        audio_device="ac97",
    ),
    "ac97-count": _qemu_scenario(
        "audiostream",
        "AUDIO STREAM PASS",
        "AUDIO STREAM FAIL",
        "AC'97 stream counter",
        120.0,
        audio_device="ac97",
    ),
    "virtio-count": _qemu_scenario(
        "audiostream",
        "AUDIO STREAM PASS",
        "AUDIO STREAM FAIL",
        "Virtio sound counter",
        120.0,
        audio_device="virtio",
    ),
    "virtio-stream": _qemu_scenario(
        "audiostream",
        "AUDIO STREAM PASS",
        "AUDIO STREAM FAIL",
        "Virtio sound streaming audio",
        120.0,
        audio_device="virtio",
        wav_name="virtio-stream.wav",
    ),
    "virtio-record": _qemu_scenario(
        "audiorecord",
        "AUDIO RECORD PASS",
        "AUDIO RECORD FAIL",
        "Virtio sound recording",
        120.0,
        audio_device="virtio",
    ),
    "kbd-smoke": _qemu_scenario(
        "kbdtest",
        "KBD SMOKE PASS",
        "KBD SMOKE FAIL",
        "Keyboard input smoke test",
        180.0,
        "KBD SMOKE READY",
        "kbd",
    ),
    # kbd-smoke's keys on a machine whose only keyboard is QEMU's usb-kbd (no
    # 8042): they reach the guest through xHCI and usb-hid or not at all
    # (docs/roadmaps/done/USB_ROADMAP.md).
    "usb-kbd-smoke": _qemu_scenario(
        "kbdtest",
        "KBD SMOKE PASS",
        "KBD SMOKE FAIL",
        "USB keyboard input through xHCI and usb-hid",
        180.0,
        "KBD SMOKE READY",
        "kbd",
        usb=True,
        no_ps2=True,
    ),
    # usb-kbd-smoke with the keyboard behind a full-speed usb-hub: the hub
    # driver, route strings and a device below the root port.
    "usb-hub-smoke": _qemu_scenario(
        "kbdtest",
        "KBD SMOKE PASS",
        "KBD SMOKE FAIL",
        "USB keyboard behind a USB hub",
        180.0,
        "KBD SMOKE READY",
        "kbd",
        usb=True,
        no_ps2=True,
        usb_hub=True,
    ),
    # usb-hub-smoke on the controllers that came before xHCI
    # (docs/hardware/REAL_HARDWARE.md, HW-2). On ehci and ehci-ohci the
    # full-speed hub is handed by the EHCI to one of its UHCI or OHCI
    # companions.
    **{
        f"{host}-smoke": _qemu_scenario(
            "kbdtest",
            "KBD SMOKE PASS",
            "KBD SMOKE FAIL",
            f"USB keyboard behind a USB hub on {host.upper()}",
            180.0,
            "KBD SMOKE READY",
            "kbd",
            usb=True,
            no_ps2=True,
            usb_hub=True,
            usb_host=host,
        )
        for host in ("uhci", "ohci", "ehci", "ehci-ohci")
    },
    # diskmgmt-smoke's FAT stick on the same controllers: bulk transfers. On
    # ehci the stick stays on the EHCI, at high speed.
    **{
        f"{host}-stick-smoke": _qemu_scenario(
            "diskmgmt-selftest",
            "DISKMGMT SMOKE PASS",
            "DISKMGMT SMOKE FAIL",
            f"Disk Management with its FAT USB stick on {host.upper()}",
            timeout=300.0,
            extra_disk_mib=64,
            fat_disk_mib=16,
            usb_stick_mib=16,
            usb_host=host,
        )
        for host in ("uhci", "ohci", "ehci")
    },
    # kbd-smoke's keys, typed on a USB keyboard plugged in after the old one
    # was pulled out; a stick is plugged in and pulled out on the way
    # (tools/qmp_usb_hotplug.py checks the guest's log for it).
    "usb-hotplug-smoke": _qemu_scenario(
        "kbdtest",
        "KBD SMOKE PASS",
        "KBD SMOKE FAIL",
        "USB keyboard and stick plugged in and pulled out while running",
        180.0,
        "KBD SMOKE READY",
        qmp_callback="./tools/qmp_usb_hotplug.py",
        usb=True,
        no_ps2=True,
    ),
    # usb-hotplug-smoke on EHCI, where the new keyboard is handed to a UHCI
    # companion and the stick stays at high speed, and on OHCI. Both find
    # port changes by reading the ports, with no event ring.
    **{
        f"{host}-hotplug-smoke": _qemu_scenario(
            "kbdtest",
            "KBD SMOKE PASS",
            "KBD SMOKE FAIL",
            f"USB keyboard and stick plugged in and pulled out on {host.upper()}",
            180.0,
            "KBD SMOKE READY",
            qmp_callback="./tools/qmp_usb_hotplug.py",
            usb=True,
            no_ps2=True,
            usb_host=host,
        )
        for host in ("ohci", "ehci", "ehci-ohci")
    },
    # pointer-smoke's moves and click on a machine whose only pointer is
    # QEMU's usb-tablet: absolute positions through the HID report parser.
    "usb-pointer-smoke": _qemu_scenario(
        "mousetest --selftest",
        "POINTER SMOKE PASS",
        "POINTER SMOKE FAIL",
        "USB tablet position and button path",
        120.0,
        ready="POINTER SMOKE READY",
        qmp_driver="pointer",
        usb=True,
        no_ps2=True,
        usb_pointer="tablet",
    ),
    "taskbar-smoke": _qemu_scenario(
        "",
        "TASKBAR SMOKE PASS",
        "TASKBAR SMOKE FAIL",
        "Real desktop taskbar smoke test",
        300.0,
        "handoff: starting /bin/init",
        "taskbar",
        completion="host",
        ready_wait=45.0,
    ),
    # taskbar-smoke on a machine whose only keyboard and pointer are QEMU's
    # usb-kbd and usb-mouse: every relative move and click travels xHCI and
    # the HID report parser.
    "usb-mouse-smoke": _qemu_scenario(
        "",
        "TASKBAR SMOKE PASS",
        "TASKBAR SMOKE FAIL",
        "Desktop taskbar driven by a USB mouse and keyboard",
        300.0,
        "handoff: starting /bin/init",
        "taskbar",
        completion="host",
        ready_wait=45.0,
        usb=True,
        no_ps2=True,
    ),
    # No disk attached: the kernel starts /rescue/bin/init, and the rescue
    # system has to work from the initramfs alone (docs/kernel/FILESYSTEMS.md).
    "rescue-smoke": _qemu_scenario(
        "rescue",
        "RESCUE SMOKE PASS",
        "RESCUE SMOKE FAIL",
        "The rescue system, booted with no disk",
        no_disk=True,
    ),
    "sound-capture-smoke": _qemu_scenario(
        "", "SOUND CAPTURE SMOKE PASS", "SOUND CAPTURE SMOKE FAIL",
        "SUN Sound input gain/mute, stereo recording test and replayed PCM",
        300.0, "handoff: starting /bin/init", "taskbar", completion="host",
        ready_wait=45.0, host_action="visual", visual_scenario="sound_capture", clean_audio_preferences=True,
        audio_device="hda", capture_fixture=True, no_network=True,
        wav_name="sound-capture.wav", wav_check="recording",
    ),
    "sound-smoke": _qemu_scenario(
        "", "SOUND SMOKE PASS", "SOUND SMOKE FAIL",
        "SUN Sound panel from the volume popup, live mixer controls and meters",
        300.0, "handoff: starting /bin/init", "taskbar", completion="host",
        ready_wait=45.0, host_action="visual", visual_scenario="sound", clean_audio_preferences=True,
        prepare=("./tools/build-user.sh --source sdk/nativetest --name nativetest --native",),
    ),
    "volume-popup-offline": _qemu_scenario(
        "", "VOLUME POPUP SMOKE PASS", "VOLUME POPUP SMOKE FAIL",
        "Tray volume popup lists and adjusts native audio streams with no NIC",
        300.0, "handoff: starting /bin/init", "taskbar", completion="host",
        ready_wait=45.0, host_action="visual", visual_scenario="volume_popup", clean_audio_preferences=True,
        no_network=True,
        prepare=("./tools/build-user.sh --source sdk/nativetest --name nativetest --native",),
    ),
    "imgview-test": _qemu_scenario(
        "imgview-selftest", "IMGVIEW SELFTEST PASS", "IMGVIEW SELFTEST FAIL",
        "Native image sections, BMP/PNG/JPEG pixels, reduction, errors and folder navigation",
    ),
    "image-assoc-smoke": _qemu_scenario(
        "", "IMAGE ASSOC SMOKE PASS", "IMAGE ASSOC SMOKE FAIL",
        "Files/desktop Open in Image Viewer and Edit in Paint, including document shortcuts",
        480.0, "handoff: starting /bin/init", "taskbar", completion="host",
        ready_wait=45.0, host_action="visual", visual_scenario="image-assoc",
    ),
    "imgview-smoke": _qemu_scenario(
        "", "IMGVIEW SMOKE PASS", "IMGVIEW SMOKE FAIL",
        "Image Viewer chrome, Open dialog, image pixels, zoom, pan, navigation and fullscreen",
        480.0, "handoff: starting /bin/init", "taskbar", completion="host",
        ready_wait=45.0, host_action="visual", visual_scenario="imgview",
    ),
    "wallpaper-smoke": _qemu_scenario(
        "", "WALLPAPER SMOKE PASS", "WALLPAPER SMOKE FAIL",
        "JPEG wallpaper list/Browse, reduced preview, invalid data and reboot persistence",
        600.0, "handoff: starting /bin/init", "taskbar", completion="host",
        ready_wait=45.0, host_action="visual", visual_scenario="wallpaperjpeg",
    ),
    "sxgui-smoke": _qemu_scenario(
        "",
        "SXGUI SMOKE PASS",
        "SXGUI SMOKE FAIL",
        "SxGUI drawing correctly from the shared library",
        300.0,
        "handoff: starting /bin/init",
        "taskbar",
        completion="host",
        ready_wait=45.0,
        host_action="visual",
        visual_scenario="notepadwheel",
    ),
    # Needs the DoomGeneric port installed and a WAD on the volume, so the only
    # machine requirement is the usual image. Without the port the scenario
    # fails loudly (the WAV is silent) instead of passing without checking
    # anything. The disposable copy drops /games/doom/savegames, which a fresh
    # install lacks: the user's Doom must start without writing to the system's
    # game directory, as it has to on the ISO.
    "doom-music-smoke": _qemu_scenario(
        "",
        "DOOM MUSIC SMOKE PASS",
        "DOOM MUSIC SMOKE FAIL",
        "DoomGeneric's music through libsxmidi, asserted on the captured audio",
        420.0,
        "handoff: starting /bin/init",
        "taskbar",
        audio_device="ac97",
        wav_name="doom-music.wav",
        completion="host",
        ready_wait=45.0,
        prepare=("./tools/restore_doom.sh",),
        remove_paths=("/games/doom/savegames",),
        host_action="visual",
        visual_scenario="doom",
    ),
    "audio-ports-offline": _qemu_scenario(
        "", "AUDIO PORTS OFFLINE PASS", "AUDIO PORTS OFFLINE FAIL",
        "Installed Doom and Celeste reach the native mixer and play PCM without a NIC",
        360.0, "handoff: starting /bin/init", "taskbar",
        audio_device="hda", no_network=True, clean_audio_preferences=True,
        wav_name="audio-ports-offline.wav", completion="host", ready_wait=45.0,
        host_action="visual", visual_scenario="audio_ports_offline",
    ),
    "tcp-smoke": _qemu_scenario(
        "tcptest {port}",
        "TCP SMOKE PASS",
        "TCP SMOKE FAIL",
        "TCP loss, independent progress, concurrent close, FIN retries and TIME_WAIT",
        300.0,
        host_server="tcp",
    ),
    "float-smoke": _qemu_scenario(
        "floatsmoke",
        "FLOAT SMOKE PASS",
        "FLOAT SMOKE FAIL",
        "SSE floating-point smoke test",
        prepare=(
            "./tools/build-user.sh --source sdk/floatsmoke --name floatsmoke --sse",
        ),
    ),
    "pie-smoke": _qemu_scenario(
        "piesmoke",
        "PIE SMOKE PASS",
        "PIE SMOKE FAIL",
        "An SDK PIE program: libmath by DT_NEEDED, libsximg by dlopen",
        prepare=(
            "./tools/build-user.sh --source sdk/piesmoke --name piesmoke --pie --needs libmath.so.0.4",
        ),
    ),
    "cmake-smoke": _qemu_scenario(
        "cmakesmoke",
        "CMAKE SMOKE PASS",
        "CMAKE SMOKE FAIL",
        "A program built with CMake and the SDK's toolchain file, through sxcc",
        prepare=("./sdk/cmakesmoke/build.sh",),
    ),
    "sxbasic-smoke": _qemu_scenario(
        "sxbasic",
        "SXBASIC SMOKE PASS",
        "SXBASIC SMOKE FAIL",
        "SxBASIC: the console sample compiled and run in the guest, against its expected output",
    ),
    "sxbasic-greeter-smoke": _qemu_scenario(
        "sxbforms-greeter",
        "SXBASIC FORMS SMOKE PASS",
        "SXBASIC FORMS SMOKE FAIL",
        "SxBASIC forms: the greeter sample driven by its selftest script, a message box included",
    ),
    "sxbasic-converter-smoke": _qemu_scenario(
        "sxbforms-converter",
        "SXBASIC FORMS SMOKE PASS",
        "SXBASIC FORMS SMOKE FAIL",
        "SxBASIC forms: the converter sample, a ShowModal dialog, a message box over it, a list and a menu",
    ),
    "sxbasic-make-smoke": _qemu_scenario(
        "sxbmake",
        "SXBASIC MAKE SMOKE PASS",
        "SXBASIC MAKE SMOKE FAIL",
        "SxBASIC Make SXE: the converter sample made into /tmp/Converter.sxe, then run with its selftest",
    ),
    "sxbide-smoke": _qemu_scenario(
        "sxbide",
        "SXBIDE SMOKE PASS",
        "SXBIDE SMOKE FAIL",
        "Savan BASIC's environment: the console sample opened, saved as, run, broken at a line, made",
    ),
    "session-smoke": _qemu_scenario(
        "sessiontest",
        "SESSION SMOKE PASS",
        "SESSION SMOKE FAIL",
        "Session manager: the desktop and the session come back when killed",
        300.0,
    ),
    # The elevation prompt answered with real keys (docs/kernel/
    # USERS_AND_PERMISSIONS.md#elevation): windowd logs the prompt, the host
    # presses N and then Y through PS/2, and a forged answer the requester
    # writes down its own pipe must change nothing.
    "elevation-smoke": _qemu_scenario(
        "elevtest",
        "ELEVATION SMOKE PASS",
        "ELEVATION SMOKE FAIL",
        "Elevation broker and prompt: real keys decide, forged answers do not",
        300.0,
        "windowd: consent prompt shown",
        qmp_callback="./tools/qmp_elevation.py",
    ),
    "pthread-test": _qemu_scenario(
        "pthreadtest",
        "PTHREAD TEST PASS",
        "PTHREAD TEST FAIL",
        "POSIX threads: locks, malloc, errno, TLS, poll, exec and fork with threads",
        240.0,
    ),
    "nightwood-smoke": _qemu_scenario(
        "nightwood-selftest",
        "NIGHTWOOD SELFTEST PASS",
        "NIGHTWOOD SELFTEST FAIL",
        "Nightwood (SUN) builds its forest, reaches every photograph and renders it with SxGL",
        180.0,
    ),
    "sxgl-gpu-test": _qemu_scenario(
        "sxgltest",
        "SXGLTEST PASS",
        "SXGLTEST FAIL",
        "SxGL's GPU backend against its software rasterizer (run with --gl for virgl)",
    ),
    "audio-offline": _qemu_scenario(
        "nativetest", "NATIVE TEST PASS", "NATIVE TEST FAIL",
        "SUN playback and mixer controls without a network card",
        audio_device="hda", no_network=True,
        prepare=("./tools/build-user.sh --source sdk/nativetest --name nativetest --native",),
    ),
    "native-network-offline": _qemu_scenario(
        "nativenetwork offline", "NATIVE NETWORK OFFLINE PASS", "NATIVE NETWORK OFFLINE FAIL",
        "SUN socket initialization errors without a network card",
        no_network=True,
        prepare=("./tools/build-user.sh --source sdk/nativetest --name nativetest --native",),
    ),
    "native-network": _qemu_scenario(
        "nativenetwork {port}", "NATIVE NETWORK PASS", "NATIVE NETWORK FAIL",
        "SUN IPv4 routes/errors, independent retries, concurrent streams and close cancellation",
        host_server="tcp",
        prepare=("./tools/build-user.sh --source sdk/nativetest --name nativetest --native",),
    ),
    # The Win32 subsystem's W1 gate (docs/userland/WIN32.md): a SUN parent runs
    # the PE32 fixtures through winhost and compares output and exit status
    # with Windows XP's. Run it under --accel kvm too: TCG does not fault on a
    # lost data selector in 32-bit code (docs/kernel/ARCH.md#segments).
    "win32-hello": _qemu_scenario(
        "win32test",
        "WIN32 HELLO PASS",
        "WIN32 HELLO FAIL",
        "PE32 fixtures through winhost, byte for byte against Windows XP",
    ),
    # SSL's L1 gate (docs/userland/LINUX.md): a SUN parent runs the Linux
    # fixtures through the launcher and compares output and exit status with
    # what Linux gave for the same files, then checks the images it must
    # refuse. The startup fixture makes two million trapped calls, which is
    # slow under TCG.
    "linux-hello": _qemu_scenario(
        "linuxtest",
        "LINUX TEST PASS",
        "LINUX TEST FAIL",
        "Linux fixtures through the launcher, byte for byte against Linux",
        300.0,
    ),
    # SSL's L2 gate for a file kept across a restart: the first boot writes it
    # with the files fixture and syncs, the host resets the machine at the
    # ready token, and the second boot reads it back and removes it.
    "linux-xeyes": _qemu_scenario(
        "linuxxeyes", "LINUX XEYES PASS", "LINUX XEYES FAIL",
        "Pinned Xeyes: Linux pixels, pointer path, resize, relaunch, restart and resource recovery (requires installed runtime)", 300.0,
        ready="LINUX XEYES READY", qmp_callback="tools/linux_xeyes_smoke.py",
        remove_paths=("/system/linux/work/l6-restart", "/users/user/Settings/cursors.cfg"),
        usb=True, no_ps2=True, usb_pointer="tablet",
    ),
    "linux-xclient": _qemu_scenario(
        "linuxxclient", "LINUX XCLIENT PASS", "LINUX XCLIENT FAIL",
        "Unchanged Xlib client: Linux pixels, global pointer, resize and caption close without leaks", 120.0,
        ready="LINUX XCLIENT READY", qmp_callback="tools/linux_xclient_smoke.py",
        usb=True, no_ps2=True, usb_pointer="tablet",
    ),
    "linux-dynamic": _qemu_scenario(
        "linuxdynamic", "LINUX DYNAMIC PASS", "LINUX DYNAMIC FAIL",
        "Unmodified musl jq, shared TLS and more than 128 private file maps", 180.0,
    ),
    "linux-dynamic-missing-lib": _qemu_scenario(
        "linuxnolib", "LINUX DYNAMIC PASS", "LINUX DYNAMIC FAIL",
        "Musl's missing-library messages match Linux byte for byte", 120.0,
        remove_paths=("/system/linux/usr/lib/libonig.so.5",),
    ),
    "linux-dynamic-missing-interp": _qemu_scenario(
        "linuxnointerp", "LINUX DYNAMIC PASS", "LINUX DYNAMIC FAIL",
        "Linux spawn refuses a missing interpreter with ENOENT and no leaked process", 120.0,
        remove_paths=("/system/linux/lib/ld-musl-x86_64.so.1",),
    ),
    "linux-tools": _qemu_scenario(
        "linuxtools", "LINUX TOOLS PASS", "LINUX TOOLS FAIL",
        "Pinned static BusyBox applets match Linux output and errors", 120.0,
        remove_paths=("/system/linux/work/l3-private", "/system/linux/work/l3-mode"),
    ),
    "linux-restart": _qemu_scenario(
        "linuxrestart",
        "LINUX RESTART PASS",
        "LINUX RESTART FAIL",
        "A Linux program's file read back after a guest restart",
        300.0,
        ready="LINUX RESTART READY",
        qmp_callback="tools/linux_restart_smoke.py",
        remove_paths=("/system/linux/work/l2-files.dat", "/system/linux/work/restart-marker"),
    ),
    "power-smoke": _qemu_scenario(
        "nativepower",
        "NATIVE POWER PASS",
        "NATIVE POWER FAIL",
        "The power button leaves the volume clean and unflushed data readable after the next boot",
        300.0,
        ready="NATIVE POWER READY",
        qmp_callback="tools/power_smoke.py",
        prepare=(
            "./tools/build-user.sh --source sdk/nativetest --name nativetest --native",
        ),
    ),
    "power-button-smoke": _qemu_scenario(
        "nativebutton",
        "NATIVE BUTTON PASS",
        "NATIVE BUTTON FAIL",
        "The power button ends the real session through init: a declined close cancels it, the next press powers off",
        300.0,
        ready="NATIVE BUTTON READY",
        qmp_callback="tools/power_button_smoke.py",
        prepare=(
            "./tools/build-user.sh --source sdk/nativetest --name nativetest --native",
        ),
    ),
    "session-end-smoke": _qemu_scenario(
        "nativesessionend",
        "NATIVE SESSION END PASS",
        "NATIVE SESSION END FAIL",
        "A shutdown through init ends the real session: a declined close cancels it, the next one goes through",
        300.0,
        prepare=(
            "./tools/build-user.sh --source sdk/nativetest --name nativetest --native",
        ),
    ),
    "win32-cards": _qemu_scenario(
        "win32cards",
        "WIN32 CARDS PASS",
        "WIN32 CARDS FAIL",
        "The user's Windows XP cards.dll draws every card under winhost as on Windows XP",
        win32_files=("cards.dll",),
    ),
    "win32-window": _qemu_scenario(
        "win32window",
        "WIN32 WINDOW PASS",
        "WIN32 WINDOW FAIL",
        "A Win32 window under windowd: messages against Windows XP, pixels against the golden image",
        240.0,
        ready="WIN32 WINDOW READY",
        qmp_callback="tools/win32_window_smoke.py",
        usb=True,
        no_ps2=True,
        usb_pointer="tablet",
    ),
    "win32-dialog": _qemu_scenario(
        "win32dialog",
        "WIN32 DIALOG PASS",
        "WIN32 DIALOG FAIL",
        "Win32 menus and dialogs under windowd: messages against Windows XP, pixels against the golden images",
        300.0,
        ready="WIN32 DIALOG READY",
        qmp_callback="tools/win32_dialog_smoke.py",
        usb=True,
        no_ps2=True,
        usb_pointer="tablet",
    ),
    "win32-solitaire": _qemu_scenario(
        "win32solitaire",
        "WIN32 SOLITAIRE PASS",
        "WIN32 SOLITAIRE FAIL",
        "The user's Windows XP sol.exe played to a win under windowd, twice: pixels against the harness's",
        900.0,
        ready="WIN32 SOLITAIRE READY",
        qmp_callback="tools/win32_solitaire_smoke.py",
        usb=True,
        no_ps2=True,
        usb_pointer="tablet",
        win32_files=("sol.exe@program files/solitaire", "cards.dll@program files/solitaire"),
    ),
    "native-test": _qemu_scenario(
        "nativetest",
        "NATIVE TEST PASS",
        "NATIVE TEST FAIL",
        "Standalone native syscall ABI test",
        prepare=(
            "./tools/build-user.sh --source sdk/nativetest --name nativetest --native",
        ),
    ),
    # Una dependencia DECLARADA que no esta en el volumen. Es el caso que decide
    # como va a funcionar el reproductor cuando FFmpeg sea libreria: el programa
    # tiene que SEGUIR VIVO y decir cual falta, en vez de desaparecer.
    "needstest-missing": _qemu_scenario(
        "needstest",
        "NEEDSTEST SURVIVED",
        "NEEDSTEST OK",
        "A program with an unresolvable DT_NEEDED still runs and names the library",
        180.0,
        remove_paths=("lib/libneeded.so.0.4",),
    ),

    "mediaplayer-fullscreen": _qemu_scenario(
        "", "SXGUI SMOKE PASS", "SXGUI SMOKE FAIL",
        "Media Player fullscreen controls, window restoration and video aspect ratio",
        300.0, "handoff: starting /bin/init", "taskbar",
        completion="host", ready_wait=45.0, host_action="visual",
        visual_scenario="mediaplayer_fullscreen",
        prepare=("./ports/ffmpeg/install.sh --with-test-media",),
    ),

    # El reproductor sin su motor: tiene que DECIR que falta, no desaparecer. La
    # exit 2 es correcta y el exito del escenario es el mensaje, no el codigo.
    "mediaplayer-nolib": _qemu_scenario(
        "mediaplayer",
        "mediaplayer: falta libffmpeg.so.0.4",
        "MEDIAPLAYER PASS",
        "The Media Player names its missing decoding library instead of crashing",
        180.0,
        prepare=("./ports/ffmpeg/install.sh --with-test-media",),
        remove_paths=("lib/libffmpeg.so.0.4",),
    ),

    # El reproductor sin su motor, en la variante que se ve: la ventana con el aviso.
    # Se quita la LIBRERIA y no hay que quitar ningun programa, porque el reproductor
    # es del arbol y existe siempre que el port se ha construido. La variante sin
    # ventana --stdout y codigo de salida-- es mediaplayer-nolib.
    "mediaplayer-noport": _qemu_scenario(
        "",
        "SXGUI SMOKE PASS",
        "SXGUI SMOKE FAIL",
        "The player opens a window naming the decoding library it could not load",
        300.0,
        "handoff: starting /bin/init",
        "taskbar",
        completion="host",
        ready_wait=45.0,
        host_action="visual",
        visual_scenario="mediaplayer_unavailable",
        prepare=("./ports/ffmpeg/install.sh --with-test-media",),
        remove_paths=("lib/libffmpeg.so.0.4",),
    ),

    # Los cuatro caminos de fallo del cargador. Estos programas existen desde la fase
    # "fallos diagnosticables" y NO los ejecutaba nadie: afirmaban con eprintf al
    # fallar y no tenian token de exito, asi que no habia nada que afirmar. Ya lo
    # tienen, y por eso hay un escenario cada uno.
    #
    # El token de exito es lo que las hace discriminantes: sin el, un programa que
    # solo habla cuando algo va mal pasa tanto si todo bien como si todo mal.
    "brokentest": _qemu_scenario(
        "brokentest",
        "BROKENTEST DIAGNOSED",
        "BROKENTEST OK",
        "A library with an unresolvable symbol names the symbol and the library",
        180.0,
    ),
    "missingtest": _qemu_scenario(
        "missingtest",
        "MISSINGTEST DIAGNOSED",
        "MISSINGTEST OK",
        "A missing file fails at the open step and leaves no half-loaded image",
        180.0,
    ),
    "slottest": _qemu_scenario(
        "slottest",
        "SLOTTEST CAPPED",
        "SLOTTEST OK",
        "The per-process library table refuses the overflow instead of overwriting",
        180.0,
    ),
    "diamondtest": _qemu_scenario(
        "diamondtest",
        "DIAMONDTEST ONCED",
        "DIAMONDTEST OK",
        "A diamond of four libraries loads the shared leaf exactly once",
        180.0,
    ),
    # The shared loader in a SUN process: sunlibtest maps libmath through the
    # same loader POSIX programs use, with no POSIX call on the way.
    "sun-libtest": _qemu_scenario(
        "sunlibtest",
        "SUN LIBRARY TEST PASS",
        "SUN LIBRARY TEST FAIL",
        "A SUN program maps libmath.so through the shared loader",
        180.0,
    ),
    # How a DT_NEEDED resolves (docs/kernel/SHARED_LIBRARIES.md, "DT_SONAME
    # resolution"): with every file present the exact 0.4 wins; with the 0.4s
    # removed the highest newer minor stands in, and an older one never does.
    "sonametest": _qemu_scenario(
        "sonametest",
        "SONAME TEST PASS",
        "SONAME TEST FAIL",
        "A DT_NEEDED loads its exact library when it is there",
        180.0,
    ),
    "sonametest-newer": _qemu_scenario(
        "sonametest",
        "SONAME TEST PASS",
        "SONAME TEST FAIL",
        "A missing library resolves to its highest newer minor, never an older one",
        180.0,
        remove_paths=("lib/libsoname.so.0.4", "lib/libsonamelow.so.0.4"),
    ),
    # syslog(3) from a POSIX program with stdout and stderr closed: the token
    # can only arrive through the system log, under the "posix[pid]" prefix.
    "posix-syslog": _qemu_scenario(
        "syslogtest",
        "]: syslogtest: warning: SYSLOG TEST PASS",
        "SYSLOG TEST FAIL",
        "syslog(3) writes to the system log from POSIX",
        180.0,
    ),
    # The loader's fallback notice from a POSIX program whose standard error is
    # closed: "]: loader:" is the system log's prefix, which a line written to
    # the console would not carry.
    "posix-loader-log": _qemu_scenario(
        "loaderlog",
        "]: loader: libsonamelatest.so.0.1 is not in /lib; falling back to the newest, libsonamelatest.so.0.7",
        "SYSLOG TEST FAIL",
        "A POSIX program's loader notice reaches the system log without stderr",
        180.0,
    ),
    # The loader's failure paths: a fixture per step (-3 to -8), and a STATIC
    # program that links the loader (-11).
    "ldfailtest": _qemu_scenario(
        "ldfailtest",
        "LOADER FAIL TEST PASS",
        "LOADER FAIL TEST FAIL",
        "Each loader failure step, provoked by a file wrong in one way",
        180.0,
    ),
    "ldstatictest": _qemu_scenario(
        "ldstatictest",
        "LOADER STATIC TEST PASS",
        "LOADER STATIC TEST FAIL",
        "A non-PIE program that links the loader is told why it cannot use libraries",
        180.0,
    ),
    "ldimagetest": _qemu_scenario(
        "ldimagetest",
        "LOADER IMAGE TEST PASS",
        "LOADER IMAGE TEST FAIL",
        "A loader that cannot register the program's image says so (step -10)",
        180.0,
    ),
    "dltest": _qemu_scenario(
        "dltest",
        "DLTEST RESOLVED",
        "DLTEST OK",
        "dlopen/dlsym/dladdr keep RTLD_LOCAL libraries apart and roll back a failed load",
        180.0,
    ),

    # Cuanto tarda el cargador con libffmpeg: 6.8 MB, 2334 simbolos. Es lo unico
    # aqui que mide algo en vez de afirmar un resultado, y por eso es el escenario
    # que contesta si la busqueda lineal de simbolos es un problema.
    "ffmpegload": _qemu_scenario(
        "ffmpegload",
        "FFMPEGLOAD OK",
        "FFMPEGLOAD FAIL",
        "Library loader timing with the real 6.8 MB FFmpeg library",
        240.0,
        prepare=("./ports/ffmpeg/install.sh --with-test-media",),
    ),

    # mediaplayer-availability y mediaplayer-missing se fueron con SxMedia.
    #
    # Los dos usaban el modo `--availability` del reproductor, que vivia en
    # sxmedia_ffmpeg.c y borro el commit 26b16e0 ("withdraw SxMedia", 2026-09-25).
    # El modo no existe: mediaplayer.c maneja --gpu-hold, --probe y --selftest, y
    # ningun otro. Los dos escenarios seguian en el catalogo pidiendo tokens que ya
    # no emite nadie, y `init.c` seguia con sus dos ramas.
    #
    # No se notaron porque no son parte del escenario `smoke` por defecto: solo
    # corren si alguien los pide a mano.
    "mediaplayer-selftest": _qemu_scenario(
        "mediaplayer",
        "MEDIAPLAYER PASS",
        "MEDIAPLAYER FAIL",
        "System Media Player delegation to the installed FFmpeg backend",
        300.0,
    ),
    "mediaplayer-display": _qemu_scenario(
        "mediaplayer-show",
        "MEDIAPLAYER PASS",
        "MEDIAPLAYER FAIL",
        "FFmpeg Media Player display frame capture",
        300.0,
        "MEDIAPLAYER DISPLAY READY",
        qmp_callback="./ports/ffmpeg/tests/qmp_display.py",
    ),
    "ffmpeg-smoke": Scenario(
        command="port",
        success="FFMPEG SMOKE PASS",
        failure="FFMPEG SMOKE FAIL",
        description="FFmpeg Media Player two-phase selftest and display smoke",
        port_command="./ports/ffmpeg/smoke.sh",
    ),
    "ccleste-selftest": _qemu_scenario(
        "ccleste-selftest",
        "CCLESTE SELFTEST PASS",
        "CCLESTE SELFTEST FAIL",
        "Celeste Classic asset decoding and headless engine self-test",
        180.0,
    ),
    "gfx2d-test": _host_scenario("gfx2d-test", "Host GFX2D test"),
    "sxgl-test": _host_scenario("sxgl-test", "Host SxGL rasterizer test and benchmark"),
    "audio-test": _host_scenario("audio-test", "Host audio mixer test"),
    "sxmidi-smoke": _host_scenario("sxmidi-test", "Host MIDI synthesizer test"),
    "partition-smoke": _host_scenario("partition-test", "Host partition table test"),
    "sxfs-smoke": _host_scenario("sxfs-volume-test", "Host SxFS volume test"),
}


def get_scenario(name: str) -> Scenario:
    try:
        return SCENARIOS[name]
    except KeyError as exc:
        choices = ", ".join(sorted(SCENARIOS)) or "(none)"
        raise SystemExit(f"unknown smoke scenario '{name}'; available: {choices}") from exc


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("name", nargs="?")
    parser.add_argument("--list", action="store_true")
    parser.add_argument(
        "--field",
        choices=(
            "command",
            "success",
            "failure",
            "timeout",
            "description",
            "prepare",
            "ready",
            "qmp_driver",
            "audio_device",
            "capture_fixture",
            "clean_audio_preferences",
            "wav_name",
            "wav_check",
            "completion",
            "ready_wait",
            "host_server",
            "host_test",
            "remove_paths",
            "qmp_callback",
            "port_command",
            "unsupported",
            "host_action",
            "visual_scenario",
            "requires_virtio",
            "no_network",
            "no_disk",
            "extra_disk_mib",
            "fat_disk_mib",
            "usb",
            "usb_stick_mib",
            "no_ps2",
            "usb_pointer",
            "usb_hub",
            "usb_host",
            "win32_files",
        ),
        required=False,
    )
    args = parser.parse_args()
    if args.list:
        for name, scenario in sorted(SCENARIOS.items()):
            print(f"{name}\t{scenario.description}")
        return 0
    if not args.name or not args.field:
        parser.error("name and --field are required unless --list is used")
    value = getattr(get_scenario(args.name), args.field)
    if args.field == "prepare":
        print("\n".join(value))
    elif args.field in ("remove_paths", "win32_files"):
        print("\n".join(value))
    else:
        print(value)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
