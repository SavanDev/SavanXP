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

    def _rel_step(self, dx, dy):
        """Un salto chico. Grande NO sirve: el mouse del guest es un PS/2
        emulado, sus paquetes llevan deltas de un byte y la cola del controlador
        es corta, asi que un delta de cientos de pixeles se parte en una rafaga
        que el guest no alcanza a drenar y se pierde entera."""
        events = []
        if dx:
            events.append({"type": "rel", "data": {"axis": "x", "value": int(dx)}})
        if dy:
            events.append({"type": "rel", "data": {"axis": "y", "value": int(dy)}})
        if events:
            self.cmd("input-send-event", events=events)
            time.sleep(0.03)

    def move_to(self, x, y):
        """Lleva el cursor a (x, y), por pasos.

        El mouse es RELATIVO y no hay forma de leer donde esta el cursor, asi
        que primero se lo empuja contra la esquina: el WM clampea a [0, ancho-1],
        o sea que pasarse deja el cursor en (0,0), el unico origen conocido.
        Despues se avanza hasta el destino. Todo de a pasos de 64 px.
        """
        step = 64
        for _ in range((1280 // step) + 2):
            self._rel_step(-step, -step)
        time.sleep(0.2)

        moved_x = 0
        moved_y = 0
        while moved_x < x or moved_y < y:
            dx = min(step, x - moved_x) if moved_x < x else 0
            dy = min(step, y - moved_y) if moved_y < y else 0
            self._rel_step(dx, dy)
            moved_x += dx
            moved_y += dy
        time.sleep(0.5)

    def click(self):
        self.cmd("input-send-event", events=[
            {"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.25)
        self.cmd("input-send-event", events=[
            {"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.6)

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
        image = Image.open(ppm).convert("RGB")
        image.save(png)
        os.remove(ppm)
        print("  captura:", os.path.basename(png))
        return image

    def launch(self, steps):
        """Lanza el icono que esta `steps` a la derecha del primero.

        La barra de menu de sxgui no tiene acceso por teclado, pero los iconos
        del launcher si: flechas para moverse y Enter para lanzar. El orden es
        el de k_default_items (progman_registry.c): Shell, Files, Notepad.
        """
        for _ in range(steps):
            self.qmp.tap("right", pause=0.4)
        self.qmp.tap("ret")
        time.sleep(25)

    def open_notepad(self):
        self.launch(2)

    def open_files(self):
        self.launch(1)


# Paleta de sxgui, para leer los pixeles con el mismo vocabulario con el que se
# dibujaron (savanxp/sxgui.h).
FACE = (192, 192, 192)
LIGHT = (255, 255, 255)
SHADOW = (128, 128, 128)

# Geometria de la barra, espejo de windowd_layout.h y taskbar.c. Si alguno de
# los dos cambia, este harness tiene que cambiar con el -- que es justamente la
# gracia: el numero deja de estar solo en el codigo.
TASKBAR_HEIGHT = 28
TASKBAR_MARGIN = 2
TASKBAR_BUTTON_WIDTH = 160
TASKBAR_BUTTON_GAP = 2


class Failure(Exception):
    pass


def button_rect(image, index):
    width, height = image.size
    return (
        TASKBAR_MARGIN + index * (TASKBAR_BUTTON_WIDTH + TASKBAR_BUTTON_GAP),
        height - TASKBAR_HEIGHT + TASKBAR_MARGIN,
        TASKBAR_BUTTON_WIDTH,
        TASKBAR_HEIGHT - (TASKBAR_MARGIN * 2),
    )


def expect_pixel(image, x, y, colour, label):
    got = image.getpixel((x, y))
    if got != colour:
        raise Failure("%s: pixel (%d,%d) es %s, esperaba %s" % (label, x, y, got, colour))


def expect_button(image, index, sunken, label):
    """Un boton hundido invierte el bisel.

    Es la asercion mas util que se puede hacer sobre esta barra: el relieve
    codifica DIRECTAMENTE cual es la ventana activa, asi que verificarlo prueba
    la cadena entera -- click, pedido al WM, cambio de estado, republicacion de
    la lista y repintado del cliente -- sin leer una sola variable.
    """
    x, y, w, h = button_rect(image, index)
    top_left = SHADOW if sunken else LIGHT
    bottom_right = LIGHT if sunken else SHADOW
    estado = "hundido" if sunken else "levantado"
    expect_pixel(image, x, y, top_left, "%s: boton %d %s (borde sup-izq)" % (label, index, estado))
    expect_pixel(image, x + w - 1, y + h - 1, bottom_right,
                 "%s: boton %d %s (borde inf-der)" % (label, index, estado))


def expect_taskbar_present(image, label):
    width, height = image.size
    # Bien a la derecha de los botones: ahi la franja tiene que ser cara pelada.
    expect_pixel(image, width - 40, height - (TASKBAR_HEIGHT // 2), FACE,
                 "%s: la franja de la barra" % label)


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


def scenario_files(s):
    """Explorador: la ventana con mas controles distintos a la vez.

    Barra de direccion, boton, lista de detalles con cabecera de columnas y
    barra de estado de dos paneles: si un margen o una sangria del toolkit se
    va, aca se ve. Se mueve la seleccion con el teclado para que quede a la
    vista tambien la fila resaltada.
    """
    s.open_files()
    s.shot("files-raiz")
    s.qmp.tap("down", pause=0.6)
    s.qmp.tap("down", pause=1.2)
    s.shot("files-seleccion")


def scenario_kbdlayout(s):
    """Selector de layout de teclado: click en el indicador de la taskbar
    abre el popup anclado, click en una fila lo aplica y lo cierra.

    Geometria fija (1280x800, ver build/taskbar.c/windowd.c): el indicador
    mide 32px en el extremo derecho de la franja de 28px; el popup mide
    96x44 y queda anclado justo arriba, pegado al mismo borde derecho.
    """
    s.shot("desktop")

    s.qmp.move_to(1262, 786)
    s.qmp.click()
    time.sleep(1.0)
    s.shot("popup-abierto")

    # Segunda fila del popup ("English").
    s.qmp.move_to(1228, 760)
    s.qmp.click()
    time.sleep(1.0)
    s.shot("layout-en")

    # Confirmar que el driver de verdad cambio: notepad + la tecla fisica que
    # en ES da apostrofe/comilla y en US (recien elegido) da '['.
    s.open_notepad()
    s.qmp.tap("bracket_left")
    time.sleep(0.5)
    s.shot("notepad-en")


def scenario_taskbar(s):
    """Barra de tareas: clicks reales con VERIFICACION de pixeles.

    Los botones van en orden estable por slot, asi que el 0 es Program Manager
    (lanzado con la sesion) y el 1 el bloc de notas. La franja esta al pie y los
    botones miden 160 desde x=2.

    No alcanza con sacar capturas para que las mire alguien: los cuatro bugs que
    tuvo esta barra -- dos de fds, dos de input -- pasaban todos los harnesses
    headless. Lo que se asierta es el BISEL de cada boton, que codifica cual es
    la ventana activa, asi que cada chequeo prueba la cadena entera: click,
    pedido al WM, cambio de estado, republicacion de la lista y repintado.
    """
    image = s.shot("solo-progman")
    expect_taskbar_present(image, "sesion recien arrancada")
    expect_button(image, 0, True, "sesion recien arrancada")

    s.open_notepad()
    image = s.shot("dos-ventanas")
    expect_taskbar_present(image, "con dos ventanas")
    # El bloc de notas se acaba de abrir y tiene el foco.
    expect_button(image, 0, False, "con dos ventanas")
    expect_button(image, 1, True, "con dos ventanas")

    # Click sobre el boton de Program Manager: lo activa.
    x, y, w, h = button_rect(image, 0)
    s.qmp.move_to(x + w // 2, y + h // 2)
    s.shot("cursor-sobre-boton")
    s.qmp.click()
    time.sleep(2.0)
    image = s.shot("activado")
    expect_button(image, 0, True, "despues de activar")
    expect_button(image, 1, False, "despues de activar")

    # El mismo boton otra vez -- que sigue en el mismo lugar porque el orden es
    # estable -- ahora minimiza: la ventana se va y el boton se levanta.
    s.qmp.click()
    time.sleep(2.0)
    image = s.shot("minimizado")
    expect_button(image, 0, False, "despues de minimizar")
    expect_taskbar_present(image, "despues de minimizar")


SCENARIOS = {
    "desktop": scenario_desktop,
    "alttab": scenario_alttab,
    "clipboard": scenario_clipboard,
    "files": scenario_files,
    "taskbar": scenario_taskbar,
    "kbdlayout": scenario_kbdlayout,
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

    try:
        SCENARIOS[opts.scenario](Session(qmp, opts.out))
    except Failure as failure:
        # A stdout y no a stderr: PowerShell convierte el stderr de un comando
        # nativo en registros de error y el `throw` del .ps1 termina tapando el
        # motivo real, que es lo unico que hace falta leer.
        print("FALLO: %s" % failure)
        print("Las capturas quedaron en %s para ver que paso." % opts.out)
        return 1
    print("%s: OK" % opts.scenario)
    return 0


if __name__ == "__main__":
    sys.exit(main())
