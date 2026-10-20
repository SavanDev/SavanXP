#!/usr/bin/env python3
"""Boot the i386 GRUB2 ISO on the legacy machine and wait for its serial tokens."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Mismo directorio: el cliente QMP que ya usa el arranque de smokes.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_client import QmpClient

SUCCESS_TOKEN = "I386 BOOT OK"
FAILURE_TOKEN = "I386 BOOT FAIL"
PAGING_TOKEN = "paginacion: identidad 0..1 GiB"
SXFS_TOKEN = "SXFS ROOT OK"
USER_LOADED_TOKEN = "user: /disk/bin/sysinfo cargado"
USER_OUTPUT_TOKEN = "sxfs_files:"
USER_EXIT_TOKEN = "proceso salio con codigo 0"
PS2_TOKEN = "ps2: teclado listo"
# El PIT se programa en cada arranque (sin disco ni programa de por medio),
# asi que su token se exige junto al de la paginacion.
PIT_TOKEN = "pit: canal 0 a 100 Hz"
# Consola de texto sobre el framebuffer EGA: se exige con --expect-key.
EGA_TOKEN = "consola: texto EGA 80x25 en 0x000b8000"
# El shell anuncia su arranque: es la senal para la segunda fase de tecleado
# (--expect-sh), porque el shell solo existe cuando llego a ring 3.
SH32_TOKEN = "sh32: listo"
# El prompt del shell: lo escribe al arrancar y al volver de cada comando.
SH32_PROMPT = "sh32> "

# Scancodes set 1 (make) de las teclas que el test sabe teclear. El kernel los
# imprime tal cual (ps2: scan), y con la decodificacion encima exige el ascii
# que el shell/userland reporta.
KEY_SCANCODES = {
    "a": 0x1E,
    "b": 0x30,
    "c": 0x2E,
    "d": 0x20,
    "e": 0x12,
    "f": 0x21,
    "g": 0x22,
    "h": 0x23,
    "i": 0x17,
    "k": 0x25,
    "l": 0x26,
    "m": 0x32,
    "n": 0x31,
    "o": 0x18,
    "p": 0x19,
    "r": 0x13,
    "s": 0x1F,
    "t": 0x14,
    "u": 0x16,
    "v": 0x2F,
    "w": 0x11,
    "y": 0x15,
    "0": 0x0B,
    "5": 0x06,
    "enter": 0x1C,
    "space": 0x39,
}
KEY_ASCII = {
    "a": 97,
    "b": 98,
    "c": 99,
    "d": 100,
    "e": 101,
    "f": 102,
    "g": 103,
    "h": 104,
    "i": 105,
    "k": 107,
    "l": 108,
    "m": 109,
    "n": 110,
    "o": 111,
    "p": 112,
    "r": 114,
    "s": 115,
    "t": 116,
    "u": 117,
    "v": 118,
    "w": 119,
    "y": 121,
    "0": 48,
    "5": 53,
    "enter": 10,
    "space": 32,
}
# Nombre del test -> qcode de QEMU. La mayoria de los nombres coinciden (QEMU
# llama a las letras como las llamamos aqui), pero Enter y Space no: el qcode
# es `ret` y `spc`, y input-send-event rechaza el nombre nuestro.
KEY_QMP_NAMES = {
    "enter": "ret",
    "space": "spc",
}
# Comandos del shell que el test sabe teclear (--expect-sh), con el patron de
# su respuesta (lo que el comando encontro en el sistema).
SH32_COMMAND_ANSWERS = {
    "uptime": r"uptime \d+ ms",
    "echo hi": r"hi\r?\n",
    "sysinfo": r"sh32: system_info ok, archivos \d+, uptime \d+ ms",
}


def terminate(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def type_keys(qmp_socket: Path | None, keys: tuple[tuple[str, int], ...]) -> None:
    """Tap each key over QMP, leaving the guest time to echo every scancode."""
    if qmp_socket is None:
        raise RuntimeError("no QMP socket to type on")
    deadline = time.monotonic() + 20
    while not qmp_socket.exists() and time.monotonic() < deadline:
        time.sleep(0.2)
    with QmpClient(str(qmp_socket), timeout=10.0) as client:
        for name, _scancode in keys:
            client.tap(KEY_QMP_NAMES.get(name, name))
            time.sleep(0.2)


def command_to_keys(command: str) -> tuple[tuple[str, int], ...]:
    """Un comando del shell a taps individuales: un tap por caracter, con el
    enter del final. Letras y '0'/'5'/espacio ya estan en los mapas; cualquier
    otra cosa es un error de programacion del propio test."""
    out: list[tuple[str, int]] = []
    for character in command:
        if character == " ":
            name = "space"
        else:
            name = character
        if name not in KEY_SCANCODES:
            raise RuntimeError(f"command character has no scancode mapping: {character!r}")
        out.append((name, KEY_SCANCODES[name]))
    out.append(("enter", KEY_SCANCODES["enter"]))
    return tuple(out)


def shell_transcript(text: str) -> str:
    """Lo que escribio el shell, sin el log del teclado. El kernel y el shell
    escriben en el mismo puerto serie, asi que el log del kernel ("ps2: ...")
    se intercala con la salida del proceso y parte el eco de una linea en
    trozos: quitarlo devuelve el eco entero, que es lo que el usuario ve en
    la pantalla."""
    return re.sub(r"ps2: [^\r\n]*\r?\n?", "", text)


def shell_session_pattern(commands: tuple[str, ...]) -> str:
    """La sesion completa del shell, sobre la transcripcion: por cada
    comando, el prompt, el eco de lo tecleado, la linea que cierra el enter y
    la respuesta del comando. El orden importa: sin el, la respuesta de un
    comando podria satisfacerse con la de otro."""
    parts: list[str] = []
    for command in commands:
        parts.append(re.escape(SH32_PROMPT))
        parts.append(re.escape(command))
        parts.append(r"\r?\n")
        parts.append(SH32_COMMAND_ANSWERS[command])
    return r".*?".join(parts)


def type_shell(qmp_socket: Path | None, commands: tuple[str, ...]) -> None:
    """Teclea cada comando del shell con su enter, dejando al proceso leer,
    ejecutar y responder entre comando y comando (el shell es un proceso en
    ring 3 con solo sti;hlt detras, no hay pipeline que llenar)."""
    if qmp_socket is None:
        raise RuntimeError("no QMP socket to type on")
    deadline = time.monotonic() + 20
    while not qmp_socket.exists() and time.monotonic() < deadline:
        time.sleep(0.2)
    with QmpClient(str(qmp_socket), timeout=10.0) as client:
        for command in commands:
            for name, _scancode in command_to_keys(command):
                client.tap(KEY_QMP_NAMES.get(name, name))
                time.sleep(0.2)
            # El comando ya entro: el proceso tardo un par de ticks en leerlo
            # y responder, y el siguiente no se teclea encima de esa respuesta.
            time.sleep(1.0)


def run_boot(
    qemu: str,
    args: list[str],
    serial: Path,
    timeout: float,
    require: tuple[str, ...] = (),
    detail: str = "",
    qmp_socket: Path | None = None,
    keys: tuple[tuple[str, int], ...] = (),
    shell_commands: tuple[str, ...] = (),
    patterns: tuple[re.Pattern[str], ...] = (),
) -> None:
    """Boot the ISO and wait for the bootstrap token.

    The token is printed after paging is on, so it alone already implies the
    bootstrap ran to the end. The paging line is asserted too: a token that
    shows up before the page tables are enabled is exactly the failure a
    change to the entry path would cause. With a disk attached, the SxFS
    mount line is required as well.

    The required tokens are part of the wait, not a check after it: with
    --expect-user the userland output and its exit line arrive *after*
    "I386 BOOT OK", so returning on the bootstrap token alone would report a
    program that is still running.

    The keyboard comes in two phases because there are two consumers.
    --expect-key taps scancodes as soon as the kernel reports the keyboard up,
    which is before any program runs. Later, once the shell is up
    (SH32_TOKEN), --expect-sh types shell commands: the shell only exists
    then, and the program that ran before it left its stdin empty.
    """
    serial.unlink(missing_ok=True)
    process = subprocess.Popen(
        args,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    deadline = time.monotonic() + timeout
    text = ""
    typed = False
    shell_typed = False
    try:
        while time.monotonic() < deadline:
            text = serial.read_text(encoding="utf-8", errors="replace") if serial.exists() else ""
            if FAILURE_TOKEN in text:
                lines = [line for line in text.splitlines() if line.startswith("savanxp/i386")]
                reported = lines[-1] if lines else "no reason reported"
                raise RuntimeError(f"bootstrap halted: {reported}")
            if keys and not typed and PS2_TOKEN in text:
                # El teclado se teclea en cuanto el kernel dice que esta listo,
                # que es antes de que arranque cualquier programa: las teclas
                # caen en la cola y el primer programa las descarta al entrar
                # (stdin es estado por proceso). Teclearlas despues dejaria
                # una ventana en la que el shell hereda una tecla de la fase
                # anterior y la teclea como parte de su primera linea.
                type_keys(qmp_socket, keys)
                typed = True
            if SUCCESS_TOKEN in text:
                if PAGING_TOKEN not in text:
                    raise RuntimeError(f"booted without enabling paging: {PAGING_TOKEN!r} missing")
                if PIT_TOKEN not in text:
                    raise RuntimeError(f"booted without the timer: {PIT_TOKEN!r} missing")
                if shell_commands and not shell_typed:
                    # Segunda fase: los comandos del shell entran cuando el
                    # shell esta esperando stdin, ni un tap antes.
                    if SH32_TOKEN not in text:
                        time.sleep(0.2)
                        continue
                    type_shell(qmp_socket, shell_commands)
                    shell_typed = True
                if all(token in text for token in require) and all(
                    pattern.search(shell_transcript(text)) for pattern in patterns
                ):
                    print(f"i386 bootstrap: PASS{detail}")
                    return
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited with {process.returncode} before the bootstrap token")
            time.sleep(0.2)
        missing = [token for token in (SUCCESS_TOKEN, *require) if token not in text]
        if missing:
            suffix = f"; missing {missing[0]!r}"
        else:
            unmatched = [
                pattern.pattern
                for pattern in patterns
                if not pattern.search(shell_transcript(text))
            ]
            suffix = (
                f"; the shell session did not complete (unmatched {unmatched[0][:60]!r})"
                if unmatched
                else ""
            )
        tail = " | ".join(
            line.strip()[:80] for line in text.splitlines()[-6:] if line.strip()
        )
        raise RuntimeError(
            f"bootstrap did not reach its tokens within {timeout:g}s{suffix}; last serial: {tail}"
        )
    finally:
        terminate(process)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", required=True, type=Path)
    parser.add_argument(
        "--disk-image",
        type=Path,
        default=None,
        help="attach this image as the primary IDE disk and require the SxFS mount",
    )
    parser.add_argument(
        "--expect-user",
        action="store_true",
        help="require that the kernel loaded the userland program, that it printed and that it exited",
    )
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument(
        "--expect-key",
        action="append",
        default=[],
        metavar="KEY",
        help=(
            "tap KEY over QMP once the kernel reports the keyboard up and require "
            "its scancode (repeatable): " + ", ".join(sorted(KEY_SCANCODES))
        ),
    )
    parser.add_argument(
        "--expect-sh",
        action="append",
        default=[],
        metavar="COMMAND",
        help=(
            "type COMMAND into the i386 shell once it reports ready and require "
            "its echo plus its answer (repeatable): " + ", ".join(sorted(SH32_COMMAND_ANSWERS))
        ),
    )
    parser.add_argument("--accel", choices=("tcg", "kvm"), default="tcg")
    args = parser.parse_args()
    iso = args.iso.resolve()
    if not iso.is_file():
        raise SystemExit(f"ISO does not exist: {iso}")
    disk_image = args.disk_image.resolve() if args.disk_image is not None else None
    if disk_image is not None and not disk_image.is_file():
        raise SystemExit(f"disk image does not exist: {disk_image}")
    qemu_override = os.environ.get("SAVANXP_QEMU")
    qemu = shutil.which(qemu_override or "qemu-system-i386")
    if qemu_override and not qemu:
        candidate = Path(qemu_override)
        if candidate.is_file() and os.access(candidate, os.X_OK):
            qemu = str(candidate)
    if not qemu:
        raise SystemExit("qemu-system-i386 was not found in PATH or SAVANXP_QEMU")
    with tempfile.TemporaryDirectory(prefix="savanxp-i386-boot.") as temporary:
        serial = Path(temporary) / "serial.log"
        command = [
            qemu,
            "-machine",
            "pc",
            "-accel",
            args.accel,
            "-m",
            "1024M",
            "-cpu",
            "qemu32",
            "-display",
            "none",
            "-boot",
            "d",
            "-cdrom",
            str(iso),
            "-serial",
            f"file:{serial}",
            "-no-reboot",
            "-no-shutdown",
        ]
        if disk_image is not None:
            # snapshot=on: el arranque nunca escribe en la imagen persistente.
            command += ["-drive", f"file={disk_image},format=raw,if=ide,snapshot=on"]
        require = (SXFS_TOKEN,) if disk_image is not None else ()
        detail = " (SxFS root mounted)" if disk_image is not None else ""
        if args.expect_user:
            # El programa del MVP imprime su informe y sale; el kernel informa
            # de la salida. Las dos cosas son el anillo 3 entero: cargar la
            # imagen, entrar, llamar al sistema y volver por EXIT. Los campos
            # de timer prueban que system_info responde con el PIT real.
            require += (USER_LOADED_TOKEN, USER_OUTPUT_TOKEN, USER_EXIT_TOKEN,
                        "backend: pit", "frequency_hz: 100")
            detail = " (userland program ran)"
        keys: tuple[tuple[str, int], ...] = ()
        qmp_socket: Path | None = None
        if args.expect_key:
            unknown = [key for key in args.expect_key if key not in KEY_SCANCODES]
            if unknown:
                raise SystemExit(
                    f"unknown key(s) {unknown}; supported: {', '.join(sorted(KEY_SCANCODES))}"
                )
            keys = tuple((key, KEY_SCANCODES[key]) for key in args.expect_key)
            # El kernel imprime el scancode crudo en hexadecimal de ocho digitos.
            require += tuple(f"ps2: scan 0x{scancode:08x}" for _key, scancode in keys)
            require += (EGA_TOKEN,)
            detail = f" (keyboard: {', '.join(name for name, _code in keys)})"
            qmp_socket = Path(temporary) / "qmp.sock"
            command += ["-qmp", f"unix:{qmp_socket},server=on,wait=off"]
        shell_commands: tuple[str, ...] = ()
        patterns: tuple[re.Pattern[str], ...] = ()
        if args.expect_sh:
            # Cada comando trae su eco (lo que el shell repitio de stdin) y su
            # respuesta (lo que el comando encontro en el sistema).
            unknown = [cmd for cmd in args.expect_sh if cmd not in SH32_COMMAND_ANSWERS]
            if unknown:
                raise SystemExit(
                    f"unknown command(s) {unknown}; supported: {', '.join(sorted(SH32_COMMAND_ANSWERS))}"
                )
            shell_commands = tuple(args.expect_sh)
            if not qmp_socket:
                qmp_socket = Path(temporary) / "qmp.sock"
                command += ["-qmp", f"unix:{qmp_socket},server=on,wait=off"]
            require += (SH32_TOKEN,)
            # Una sola sesion: eco, respuesta y prompt de cada comando, en ese
            # orden. La respuesta de un comando no puede valer para otro.
            patterns = (re.compile(shell_session_pattern(shell_commands), re.DOTALL),)
            detail = f" (shell: {', '.join(shell_commands)})"
        run_boot(
            qemu, command, serial, args.timeout, require, detail, qmp_socket, keys,
            shell_commands, patterns,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
