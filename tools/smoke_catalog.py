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
    audio_device: str = "auto"
    wav_name: str = ""
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


def _qemu_scenario(
    command: str,
    success: str,
    failure: str,
    description: str,
    timeout: float = 180.0,
    ready: str = "",
    qmp_driver: str = "",
    audio_device: str = "auto",
    wav_name: str = "",
    completion: str = "serial",
    ready_wait: float = 0.0,
    host_server: str = "",
    prepare: tuple[str, ...] = (),
    remove_paths: tuple[str, ...] = (),
    qmp_callback: str = "",
    host_action: str = "",
    visual_scenario: str = "",
    requires_virtio: bool = False,
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
        wav_name=wav_name,
        completion=completion,
        ready_wait=ready_wait,
        host_server=host_server,
        prepare=prepare,
        remove_paths=remove_paths,
        qmp_callback=qmp_callback,
        host_action=host_action,
        visual_scenario=visual_scenario,
        requires_virtio=requires_virtio,
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
    "windowd-smoke": _qemu_scenario(
        "windowd-selftest",
        "WINDOWD SMOKE PASS",
        "WINDOWD SMOKE FAIL",
        "Window manager and compositor self-test",
    ),
    # Timeout de 900 s, y no es holgura: este smoke copia un programa entero
    # (390 KB) para tener una fixture con .sxmeta, y esa copia mide 365930 ms en esta
    # imagen. El resto del smoke suma 3 s. A 180 s expiraba siempre y se leia como
    # "el launcher esta roto", que no era el caso: solo faltaba tiempo.
    #
    # La causa esta en el grow-in del fichero, no en el smoke: cada crecimiento
    # commit de metadata, y cada commit escribe dos veces los 1537 sectores de la
    # region, y la transferencia del dispositivo emulado es PIO palabra a palabra.
    # Arreglar eso es otra cosa --ver ATA rw_chunk-- y hasta que se haga, este
    # escenario es lento por construccion.
    "progman-smoke": _qemu_scenario(
        "progman-selftest",
        "PROGMAN SMOKE PASS",
        "PROGMAN SMOKE FAIL",
        "Program Manager registry self-test",
        900.0,
        prepare=("./tools/restore_doom.sh",),
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
    "mines-smoke": _qemu_scenario(
        "mines-selftest",
        "MINES SMOKE PASS",
        "MINES SMOKE FAIL",
        "Mines game and persistent settings self-test",
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
    # Necesita el port DoomGeneric instalado y un WAD en el volumen; por eso el
    # único requisito de maquina es la imagen de siempre. Si el port no esta, el
    # escenario falla en voz alta (el WAV sale en silencio) en vez de pasar sin
    # comprobar nada.
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
        host_action="visual",
        visual_scenario="doom",
    ),
    "tcp-smoke": _qemu_scenario(
        "tcptest {port}",
        "TCP SMOKE PASS",
        "TCP SMOKE FAIL",
        "TCP loss and reordering smoke test",
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
    "native-guihost": _qemu_scenario(
        "guihost",
        "NATIVEGUI HOST PASS",
        "NATIVEGUI HOST FAIL",
        "Native GUI host smoke test",
        prepare=(
            "./subsystems/native/build.sh --name nativegui --source haxe-gui --install",
            "./tools/build-user.sh --source subsystems/native/test/guihost.c --name nativeguihost",
        ),
    ),
    "native-hello": _qemu_scenario(
        "nativehello",
        "NATIVE HELLO PASS",
        "NATIVE HELLO FAIL",
        "Native Haxe hello smoke test",
        prepare=(
            "./subsystems/native/build.sh --name nativehello --source haxe --install",
        ),
    ),
    "native-sxgui": _qemu_scenario(
        "sxguihost",
        "SXGUI HOST PASS",
        "SXGUI HOST FAIL",
        "Native SXGUI host smoke test",
        prepare=(
            "./subsystems/native/build.sh --name sxguiapp --source haxe-sxgui --install",
            "./tools/build-user.sh --source subsystems/native/test/sxguihost.c --name sxguihost",
        ),
    ),
    "gfx2d-test": _host_scenario("gfx2d-test", "Host GFX2D test"),
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
            "wav_name",
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
    elif args.field == "remove_paths":
        print("\n".join(value))
    else:
        print(value)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
