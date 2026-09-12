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
    def __init__(self, port, timeout=60, abs_pointer=False, screen=(1280, 800)):
        # El puntero del guest es relativo (PS/2) o absoluto (virtio-tablet), y
        # eso cambia por completo como se lo lleva a una posicion. Ver move_to.
        self.abs_pointer = abs_pointer
        self.screen = screen
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

    def _abs_move(self, x, y):
        """Puntero absoluto (virtio-tablet): se manda la posicion y listo.

        El eje absoluto de QMP va en el rango normalizado 0..32767 mapeado sobre
        el tamano de pantalla, no en pixeles."""
        width, height = self.screen
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 // max(width - 1, 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 // max(height - 1, 1))}}])
        time.sleep(0.5)

    def move_to(self, x, y):
        """Lleva el cursor a (x, y), por pasos.

        El mouse es RELATIVO y no hay forma de leer donde esta el cursor, asi
        que primero se lo empuja contra la esquina: el WM clampea a [0, ancho-1],
        o sea que pasarse deja el cursor en (0,0), el unico origen conocido.
        Despues se avanza hasta el destino. Todo de a pasos de 64 px.
        """
        if self.abs_pointer:
            self._abs_move(x, y)
            return

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

    def wheel(self, ticks):
        """Gira la rueda `ticks` muescas: positivo hacia arriba.

        QEMU modela la rueda como dos botones que se aprietan y se sueltan
        (wheel-up / wheel-down), no como un eje: hay que mandar el par entero
        por muesca o el guest no ve nada. El backend PS/2 lo traduce al cuarto
        byte del paquete IntelliMouse, el virtio a REL_WHEEL.
        """
        button = "wheel-up" if ticks > 0 else "wheel-down"
        for _ in range(abs(ticks)):
            self.cmd("input-send-event", events=[
                {"type": "btn", "data": {"down": True, "button": button}}])
            self.cmd("input-send-event", events=[
                {"type": "btn", "data": {"down": False, "button": button}}])
            time.sleep(0.15)
        time.sleep(0.6)

    def type_text(self, text):
        # Los qcodes de QEMU no son los caracteres: la barra espaciadora es
        # "spc". Las letras y digitos si coinciden con su caracter.
        #
        # La puntuacion NO entra aca. Un qcode es la TECLA FISICA, y el guest la
        # traduce con su layout activo, que por default es ES: mandar "slash"
        # -- la tecla que dice / en un teclado US -- escribe un guion. Para
        # tipear un simbolo hay que saber su combinacion en el layout del guest,
        # asi que lo hace el escenario, no este helper.
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

    def launch(self, steps, groups=0):
        """Lanza el icono que esta `steps` a la derecha del primero, `groups`
        solapas a la derecha de la primera.

        La barra de menu de sxgui no tiene acceso por teclado, pero el launcher
        si: TAB cambia de grupo, las flechas mueven la seleccion y Enter lanza
        (progman.c on_key). Cambiar de grupo vuelve la seleccion al primer
        icono, asi que los `groups` van antes que los `steps`.

        EL ORDEN YA NO ES EL DE UNA TABLA HORNEADA. progman arma su catalogo
        escaneando los binarios instalados y los ordena alfabeticamente dentro
        de cada grupo (docs/SXE_FORMAT.md, "All Programs"), asi que estos
        indices son posiciones en ese orden. Con las categorias de hoy:

            Accessories: Files, Notepad, Shell
            Diagnostics: Gfx Demo, Key Test, Mouse Test, Widgets
            Games:       Doom                     (solo si esta instalado)
            System:      Add/Remove Programs, System Properties, Task Manager
        """
        for _ in range(groups):
            self.qmp.tap("tab", pause=0.6)
        for _ in range(steps):
            self.qmp.tap("right", pause=0.4)
        self.qmp.tap("ret")
        time.sleep(25)

    def open_notepad(self):
        self.launch(1)

    def open_files(self):
        self.launch(0)

    def open_shell(self):
        self.launch(2)

    def open_appwiz(self):
        # Grupo System: la cuarta solapa cuando Doom esta instalado, que es la
        # precondicion que verifica shoot.ps1 antes de arrancar el escenario.
        self.launch(0, groups=3)

    def open_system_properties(self):
        self.launch(1, groups=3)

    def launch_sibling(self, steps):
        """Lanza otro icono del grupo en el que el launcher ya quedo parado.

        `launch()` solo sirve para el PRIMER lanzamiento de una sesion: cuenta
        solapas desde la primera, y el launcher se queda donde lo dejaron. Para
        abrir una segunda ventana hay que moverse desde donde esta, no desde el
        principio -- contar solapas de nuevo termina en otro grupo.
        """
        for _ in range(steps):
            self.qmp.tap("right", pause=0.4)
        self.qmp.tap("ret")
        time.sleep(25)


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
    # Los dos `down` de arriba ya dejaron la seleccion sobre /disk, que mezcla
    # carpetas con un .ini y un .bmp: es el directorio donde la capa de iconos
    # por tipo resuelve mas de un tipo a la vez, asi que es el que sirve de
    # regresion para el catalogo.
    s.qmp.tap("ret", pause=2.5)
    s.shot("files-disk")


def scenario_shell(s):
    """Terminal: los tres repintados que hace, uno por captura.

    El prompt se repinta por cada tecla, la salida de un comando entra por el
    sink y empuja el historial, y una salida larga lo hace desbordar. Los tres
    caminos dibujan recortados contra la banda sucia, asi que un error de clip
    deja residuos o texto faltante que se ve directo en la captura.
    """
    s.open_shell()
    s.shot("shell-abierta")
    s.qmp.type_text("help")
    time.sleep(1.0)
    s.shot("shell-tipeando")
    s.qmp.tap("ret", pause=3.0)
    s.shot("shell-salida")
    # Un segundo `help` pasa de largo las lineas visibles: fuerza el descarte
    # por el anillo del historial y el repintado del area de contenido entera.
    s.qmp.type_text("help")
    s.qmp.tap("ret", pause=4.0)
    s.shot("shell-scroll")


def scenario_system(s):
    """Propiedades del sistema y administrador de tareas, sus dos pantallas.

    Las dos viven en el grupo System del launcher, que con Doom instalado es la
    cuarta solapa -- la misma precondicion que 'appwiz', y por el mismo motivo:
    la cantidad de grupos depende de lo que haya instalado.

    Se navega por teclado porque es lo unico que no depende de donde el WM puso
    la ventana. En Propiedades el foco arranca en el control de pestanias, asi
    que la flecha alcanza; en el administrador arranca en la lista de procesos
    y se sube a las pestanias con SHIFT+TAB.

    La segunda ventana NO se abre con launch(): despues del primer lanzamiento
    el launcher se quedo en el grupo System, y volver a contar solapas desde la
    primera caeria en otro grupo.

    NO se confirma el fin de un proceso: este harness saca fotos. Se abre el
    dialogo para ver la advertencia y se cancela con ESC, que ademas es lo que
    hace el boton por defecto.
    """
    s.open_system_properties()
    s.shot("system-general")
    s.qmp.tap("right", pause=2.5)
    s.shot("system-hardware")
    s.qmp.tap("esc", pause=6.0)

    # El launcher quedo en System con la seleccion en Propiedades del sistema:
    # el administrador de tareas es el que sigue.
    s.launch_sibling(1)
    s.shot("taskmgr-processes")
    # De la lista al control de pestanias con SHIFT+TAB, que va al widget
    # ANTERIOR. Contar TABs hacia adelante no sirve: el boton End Process se
    # deshabilita cuando la fila elegida es el proceso ocioso, y un control
    # deshabilitado no recibe foco, asi que la cuenta cambia con la seleccion.
    s.qmp.chord("shift", "tab")
    time.sleep(1.0)
    s.qmp.tap("right", pause=3.0)
    s.shot("taskmgr-performance")
    s.qmp.tap("right", pause=3.0)
    s.shot("taskmgr-networking")
    s.qmp.tap("left", pause=1.0)
    s.qmp.tap("left", pause=2.5)
    # De vuelta a la lista y a la ULTIMA fila, que es el propio administrador:
    # sobre el proceso ocioso -- la fila 0 -- terminar esta deshabilitado a
    # proposito, y ademas el unico riesgo de un Enter de mas aca es que esta
    # ventana se cierre sola.
    # Las pausas son generosas a proposito: bajo TCG el guest va mucho mas lento
    # que el reloj del host, y un Del que llega antes de que la seleccion se haya
    # movido cae sobre el proceso ocioso, donde terminar no hace nada.
    s.qmp.tap("tab", pause=2.0)
    s.qmp.tap("end", pause=4.0)
    s.qmp.tap("delete", pause=4.0)
    s.shot("taskmgr-end-process")
    s.qmp.tap("esc", pause=2.0)


def scenario_appwiz(s):
    """Agregar o quitar programas: la ventana que desinstala.

    Se llega por el grupo System del launcher, que es donde la declara su
    .sxres. La lista muestra lo instalado FUERA de la imagen del sistema, asi
    que con Doom instalado tiene exactamente una fila -- por eso shoot.ps1
    exige que este antes de arrancar: sin el, la captura seria de una lista
    vacia y no se veria ni el icono, ni el tamano, ni la casilla de datos.

    NO se aprieta Remove: este harness saca fotos, no desinstala. El que valida
    el borrado es 'build.ps1 appwiz-smoke'.
    """
    s.open_appwiz()
    s.shot("appwiz")
    # TAB lleva el foco de la lista a la casilla de datos y el espacio la marca:
    # con Doom seleccionado ahi se lee el path de los WAD, que es la mitad
    # interesante de la ventana.
    s.qmp.tap("tab", pause=5.0)
    s.qmp.tap("spc", pause=6.0)
    s.shot("appwiz-datos")
    # Y la confirmacion, que es donde se ve que el dialogo dice exactamente que
    # se va a borrar. Se llega con el mouse y no con TAB porque llegar al boton
    # por teclado no resulto reproducible en este harness; coordenadas fijas,
    # como en el escenario del layout de teclado, que la ventana la ubica el WM
    # siempre en el mismo lugar (1280x800, ver windowd_layout.h).
    s.qmp.move_to(530, 551)
    s.qmp.click()
    time.sleep(6.0)
    s.shot("appwiz-confirmacion")
    # Se cancela con ESC: el boton por defecto del dialogo es Cancel justamente
    # para que un Enter de mas no borre nada. Este harness saca fotos.
    s.qmp.tap("esc", pause=3.0)
    # Un ESC mas cierra appwiz y descubre la grilla del launcher, que es donde
    # se mira el rotulo de dos lineas: un nombre largo tiene que partirse por un
    # espacio y quedar centrado, no recortado por los dos lados.
    s.qmp.tap("esc", pause=8.0)
    s.shot("progman-rotulo")


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


def scenario_bench(s):
    """Carga para windowd-stats: danio chico y continuo, a ritmo de pipeline.

    No asierta nada visual -- mide. Gfx Demo mueve una caja 8 px por flecha y
    presenta solo esa region, y junta en un solo frame todas las flechas que
    llegaron mientras esperaba al compositor. Una rafaga mas rapida de lo que
    drena el pipeline lo deja saturado: lo que sale en la linea de stats es el
    throughput del camino de display, no el ritmo de un humano. Va a pantalla
    completa (launch_flags=fullscreen) pero compuesto, que es el camino a medir.

    Las teclas van sin la pausa de tap(): con 50 ms entre bajar y subir el
    techo serian 20 frames por segundo impuestos por el harness.
    """
    s.launch(0, groups=1)            # Diagnostics -> Gfx Demo
    start = time.time()
    deadline = start + 20.0
    direction = "right"
    sent = 0
    while time.time() < deadline:
        for _ in range(40):
            s.qmp.key(direction, True)
            s.qmp.key(direction, False)
            time.sleep(0.003)
        sent += 40
        direction = "left" if direction == "right" else "right"
    elapsed = time.time() - start
    print("bench: %d pulsaciones en %.1f s (%.0f/s)" % (sent, elapsed, sent / elapsed), flush=True)
    time.sleep(4.0)                  # la ultima linea de stats sale cada 2 s
    s.shot("bench-final")


def scenario_saturate(s):
    """Como bench, pero con la entrada mas rapida que el pipeline.

    bench resulto limitado por la ENTRADA, no por el pipeline: con una tecla
    por comando QMP, windowd pasaba ~98% de cada frame ocioso esperando la
    proxima, asi que ningun cambio en el camino de display podia mover sus
    fps. Aca cada comando lleva 16 pulsaciones: Gfx Demo junta en un frame
    todas las que encuentra, asi que si hay siempre alguna pendiente al
    terminar un frame no duerme nunca y los fps pasan a ser el techo del
    camino de display.

    OJO con la cifra de pulsaciones: son las ENVIADAS. El PS/2 emulado acepta
    ~16 bytes en cola y descarta el resto, asi que las entregadas son menos.
    La prueba de que se salio del regimen limitado por la entrada no es esa
    cifra sino la ocupacion de windowd en la linea de stats.
    """
    s.launch(0, groups=1)            # Diagnostics -> Gfx Demo
    start = time.time()
    deadline = start + 20.0
    direction = "right"
    sent = 0
    commands = 0
    while time.time() < deadline:
        events = []
        for _ in range(16):
            for down in (True, False):
                events.append({"type": "key", "data": {
                    "down": down, "key": {"type": "qcode", "data": direction}}})
        s.qmp.cmd("input-send-event", events=events)
        sent += 16
        commands += 1
        direction = "left" if direction == "right" else "right"
    elapsed = time.time() - start
    print("saturate: %d pulsaciones en %.1f s (%.0f/s), %d comandos QMP (%.0f/s)"
          % (sent, elapsed, sent / elapsed, commands, commands / elapsed), flush=True)
    time.sleep(4.0)                  # la ultima linea de stats sale cada 2 s
    s.shot("saturate-final")


def find_list_area(image, min_width=150, min_height=60):
    """Area util de la lista: el rectangulo blanco mas alto de la pantalla.

    Se busca en la captura en vez de calcular la geometria porque la posicion
    de la ventana la decide el WM (centrada + cascada sobre el area de trabajo)
    y el alto de la lista lo decide el size hint de la app: replicar esa cadena
    aca seria copiar tres archivos de layout para llegar a un punto que el
    propio dibujo ya delata. El campo de texto tambien es blanco, pero mide
    veintipico de alto -- de ahi el min_height.
    """
    width, height = image.size
    pixels = image.load()
    runs = []                        # (y, x0, x1) del tramo blanco mas largo de la fila
    # El texto negro de cada fila parte el blanco en pedazos, asi que los
    # tramos separados por menos que un par de glifos cuentan como uno solo.
    # Sin esto la lista no se encuentra: cada renglon con texto queda hecho
    # tiras de 40 px.
    max_gap = 24

    for y in range(height):
        best = None
        spans = []
        x = 0
        while x < width:
            if pixels[x, y] != LIGHT:
                x += 1
                continue
            start = x
            while x < width and pixels[x, y] == LIGHT:
                x += 1
            if spans and start - spans[-1][1] <= max_gap:
                spans[-1] = (spans[-1][0], x)
            else:
                spans.append((start, x))
        for span in spans:
            if span[1] - span[0] >= min_width and (best is None or span[1] - span[0] > best[2] - best[1]):
                best = (y, span[0], span[1])
        if best is not None:
            runs.append(best)

    # Agrupar filas consecutivas cuyos runs se solapan: cada grupo es un panel.
    best_group = None
    group = []
    for run in runs:
        if group and run[0] == group[-1][0] + 1 and run[1] < group[-1][2] and run[2] > group[-1][1]:
            group.append(run)
        else:
            group = [run]
        if len(group) >= min_height and (best_group is None or len(group) > len(best_group)):
            best_group = list(group)

    if best_group is None:
        raise Failure("no se encontro ningun panel blanco donde probar la rueda")
    x0 = max(run[1] for run in best_group)
    x1 = min(run[2] for run in best_group)
    return (x0, best_group[0][0], x1, best_group[-1][0] + 1)


def scenario_wheel(s):
    """La rueda, de punta a punta: QEMU -> driver -> WM -> toolkit.

    Widgets es la unica ventana con una lista mas larga que su caja (14 items,
    7 filas visibles), asi que es la que puede probar que la rueda scrollea de
    verdad y no solo que el evento llega.

    La asercion fuerte no es "algo cambio" sino la vuelta: girar hacia arriba
    mas muescas de las que se bajo tiene que dejar la lista EXACTAMENTE como
    estaba, porque arriba de todo el scroll clampea en cero. Eso prueba las dos
    direcciones, el signo, y que no se pierde ni se duplica ningun tick -- un
    tick de mas o de menos deja la lista corrida y los pixeles no coinciden.
    """
    s.launch(3, groups=1)            # Diagnostics -> Widgets
    image = s.shot("wheel-ventana")
    area = find_list_area(image)
    print("  lista en", area)
    s.qmp.move_to((area[0] + area[2]) // 2, (area[1] + area[3]) // 2)

    before = s.shot("wheel-arriba-de-todo").crop(area).tobytes()
    s.qmp.wheel(-2)                  # dos muescas abajo = seis filas
    scrolled = s.shot("wheel-abajo").crop(area).tobytes()
    if scrolled == before:
        raise Failure("la rueda hacia abajo no movio la lista")

    s.qmp.wheel(5)                   # mas de las que bajo: clampea arriba
    back = s.shot("wheel-de-vuelta").crop(area).tobytes()
    if back != before:
        raise Failure("la lista no volvio al tope: ticks perdidos, duplicados o signo al reves")


def scenario_notepadwheel(s):
    """Verificacion ad hoc del scroll del editor de Notepad (rueda + barra)."""
    s.open_notepad()
    for i in range(1, 41):
        s.qmp.type_text("linea%02d" % i)
        s.qmp.tap("ret")
    time.sleep(1.0)
    image = s.shot("notepad-lleno")
    area = find_list_area(image)
    print("  editor en", area)
    s.qmp.move_to((area[0] + area[2]) // 2, (area[1] + area[3]) // 2)

    # Tipear deja el caret al final: el editor ya esta scrolleado al fondo, asi
    # que la rueda hay que probarla subiendo primero -- bajar ya esta clampeado.
    before = s.shot("notepad-abajo").crop(area).tobytes()
    s.qmp.wheel(2)                    # dos muescas arriba
    scrolled = s.shot("notepad-rueda-arriba").crop(area).tobytes()
    if scrolled == before:
        raise Failure("la rueda no scrolleo el editor de notepad")

    s.qmp.wheel(-20)                  # de vuelta al fondo, de sobra
    back = s.shot("notepad-rueda-vuelta").crop(area).tobytes()
    if back != before:
        raise Failure("el editor no volvio al fondo con la rueda: ticks perdidos, duplicados o signo al reves")

    # Flecha de ARRIBA de la barra incrustada: abajo ya esta clampeado por el
    # mismo motivo que la rueda. find_list_area mide el panel blanco y se pasa
    # un par de pixeles hacia la barra (el borde entre los dos no es un blanco
    # limpio), asi que el punto de click va hacia ADENTRO del area detectada y
    # no area[2] + margen -- eso ultimo cae en el borde de la ventana. La base
    # se toma con el mouse YA en posicion para que el cursor mismo, al no
    # moverse entre las dos capturas, no cuente como "cambio".
    sb_x = area[2] - 8
    sb_y = area[1] + 6
    s.qmp.move_to(sb_x, sb_y)
    before_click = s.shot("notepad-antes-del-click").crop(area).tobytes()
    s.qmp.click()
    s.qmp.click()
    s.qmp.click()
    clicked = s.shot("notepad-scrollbar-click").crop(area).tobytes()
    if clicked == before_click:
        raise Failure("clickear la barra de scroll no movio el editor")


def scenario_spin(s):
    """Carga que SI satura el pipeline: el cliente dibuja sin esperar la entrada.

    bench y saturate resultaron limitados por la entrada -- un frame por tecla
    o por comando QMP -- y con windowd ocioso la mayor parte de cada frame no
    distinguen un pipeline rapido de uno lento. Aca Gfx Demo entra en su modo
    automatico (tecla S): la caja avanza en cada vuelta y gfx_present_region
    bloquea hasta que el compositor consumio el frame anterior, asi que los
    fps son el techo del camino de display y nada mas.
    """
    s.launch(0, groups=1)            # Diagnostics -> Gfx Demo
    s.qmp.tap("s")                   # modo automatico: encendido
    time.sleep(20.0)
    s.qmp.tap("s")                   # apagado: la sesion vuelve a quedar quieta
    time.sleep(4.0)                  # la ultima linea de stats sale cada 2 s
    s.shot("spin-final")


SCENARIOS = {
    "desktop": scenario_desktop,
    "alttab": scenario_alttab,
    "clipboard": scenario_clipboard,
    "files": scenario_files,
    "shell": scenario_shell,
    "appwiz": scenario_appwiz,
    "system": scenario_system,
    "taskbar": scenario_taskbar,
    "wheel": scenario_wheel,
    "notepadwheel": scenario_notepadwheel,
    "kbdlayout": scenario_kbdlayout,
    "bench": scenario_bench,
    "saturate": scenario_saturate,
    "spin": scenario_spin,
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
    parser.add_argument("--abs-pointer", action="store_true",
                        help="el guest tiene un puntero absoluto (virtio-tablet), no PS/2")
    opts = parser.parse_args()

    qmp = Qmp(opts.port, abs_pointer=opts.abs_pointer)
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
