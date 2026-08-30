"""Driver QMP de tools/shoot.ps1: manda teclas y saca capturas.

No arranca ni apaga QEMU -- de eso se encarga el .ps1, que es donde vive la
definicion de la maquina. Aca solo se habla QMP contra un puerto ya abierto.

Los eventos van por `input-send-event` y no por el `sendkey` del monitor HMP
porque hace falta SOSTENER un modificador: capturar el switcher de Alt+Tab
abierto, o hacer un Ctrl+C, exige que la modificadora siga apretada mientras
llega la letra, y sendkey siempre suelta.
"""

import argparse
import io
import json
import os
import socket
import sys
import time


class Qmp(object):
    def __init__(self, port, timeout=60):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise RuntimeError("no se pudo conectar a QMP en el puerto %d" % port)
                time.sleep(0.2)
        self.buf = b""
        self._read()                    # saludo
        self.cmd("qmp_capabilities")

    def _read(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("QMP cerro la conexion")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def cmd(self, name, **args):
        payload = {"execute": name}
        if args:
            payload["arguments"] = args
        self.sock.sendall((json.dumps(payload) + "\r\n").encode("utf-8"))
        while True:
            msg = self._read()
            if "error" in msg:
                raise RuntimeError("QMP %s: %s" % (name, msg["error"]))
            if "return" in msg:
                return msg["return"]
            # los eventos asincronos no son respuesta a nada: se ignoran

    def key(self, qcode, down):
        self.cmd("input-send-event", events=[{
            "type": "key",
            "data": {"down": down, "key": {"type": "qcode", "data": qcode}},
        }])

    def tap(self, qcode, pause=0.12):
        self.key(qcode, True)
        time.sleep(0.05)
        self.key(qcode, False)
        time.sleep(pause)

    def chord(self, modifier, qcode):
        """Acorde real: la modificadora baja, la tecla va y viene, y sube."""
        self.key(modifier, True)
        time.sleep(0.15)
        self.tap(qcode)
        time.sleep(0.15)
        self.key(modifier, False)
        time.sleep(0.4)

    def type_text(self, text):
        # Los qcodes de QEMU no son los caracteres: la barra espaciadora es
        # "spc". Las letras y digitos si coinciden con su caracter.
        for ch in text:
            self.tap("spc" if ch == " " else ch, pause=0.12)


class Session(object):
    def __init__(self, qmp, out_dir):
        self.qmp = qmp
        self.out = out_dir
        self.index = 0

    def shot(self, name):
        self.index += 1
        stem = "%02d-%s" % (self.index, name)
        ppm = os.path.join(self.out, stem + ".ppm")
        png = os.path.join(self.out, stem + ".png")
        if os.path.exists(ppm):
            os.remove(ppm)
        self.qmp.cmd("screendump", filename=ppm)
        for _ in range(60):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                time.sleep(0.3)
                break
            time.sleep(0.2)
        else:
            raise RuntimeError("screendump no produjo " + ppm)
        from PIL import Image
        Image.open(ppm).save(png)
        os.remove(ppm)
        print("  captura:", os.path.basename(png))

    def open_notepad(self):
        """Lanza el bloc de notas desde progman SOLO con teclado.

        La barra de menu de sxgui no tiene acceso por teclado, pero los iconos
        del launcher si: flechas para moverse y Enter para lanzar.
        """
        self.qmp.tap("right", pause=0.4)
        self.qmp.tap("right", pause=0.4)
        self.qmp.tap("ret")
        time.sleep(25)


def scenario_desktop(s):
    s.shot("sesion")


def scenario_alttab(s):
    s.open_notepad()
    s.shot("notepad-abierto")
    # Con Alt sostenido el switcher queda abierto y se puede capturar; un tap
    # normal lo abriria y cerraria entre dos frames.
    s.qmp.key("alt", True)
    time.sleep(0.2)
    s.qmp.tap("tab")
    time.sleep(1.5)
    s.shot("switcher")
    s.qmp.key("alt", False)
    time.sleep(2.0)
    s.shot("cambiado")


def scenario_clipboard(s):
    s.open_notepad()
    s.qmp.type_text("hola mundo")
    time.sleep(1.0)
    s.qmp.chord("shift", "home")
    time.sleep(1.0)
    s.shot("seleccion")
    s.qmp.chord("ctrl", "c")
    time.sleep(0.8)
    s.qmp.tap("end")
    time.sleep(0.8)
    s.qmp.chord("ctrl", "v")
    time.sleep(2.0)
    s.shot("pegado")


SCENARIOS = {
    "desktop": scenario_desktop,
    "alttab": scenario_alttab,
    "clipboard": scenario_clipboard,
}


def wait_for_marker(path, marker, timeout=240):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            with io.open(path, "r", encoding="utf-8", errors="replace") as fh:
                if marker in fh.read():
                    return True
        time.sleep(0.5)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--scenario", required=True, choices=sorted(SCENARIOS))
    parser.add_argument("--boot-wait", type=float, default=45.0)
    opts = parser.parse_args()

    qmp = Qmp(opts.port)
    marker = "handoff: starting /bin/init"
    if not wait_for_marker(opts.serial, marker):
        print("no aparecio '%s' en %s" % (marker, opts.serial), file=sys.stderr)
        return 1
    # El handoff es el arranque de init; la sesion tarda mas en estar pintada.
    time.sleep(opts.boot_wait)

    SCENARIOS[opts.scenario](Session(qmp, opts.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
