"""Inyector QMP de build.ps1 kbd-smoke: manda una secuencia fija de teclas.

Recorte de la clase Qmp de tools/shoot_session.py (sin PIL, sin screenshots,
sin mouse). No hace ninguna asercion -- el PASS/FAIL lo decide enteramente
subsystems/posix/userland/kbdtest.c comparando lo que le llego a /dev/input0
contra su propio guion esperado; este script solo "mueve las manos" por
input-send-event contra el puerto QMP que build.ps1 ya dejo escuchando.
"""

import argparse
import json
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    opts = parser.parse_args()

    qmp = Qmp(opts.port)

    # Tiene que calzar 1:1 con g_checkpoints en kbdtest.c: letra simple, la
    # misma letra con Shift (tabla shifted), Ctrl+c (bit de modificador sin
    # tocar el ascii), flecha derecha (camino extendido 0xE0) y Enter.
    qmp.tap("a")
    qmp.chord("shift", "a")
    qmp.chord("ctrl", "c")
    qmp.tap("right")
    qmp.tap("ret")

    print("kbd_smoke: secuencia enviada")
    return 0


if __name__ == "__main__":
    sys.exit(main())
