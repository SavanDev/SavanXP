"""Shared QMP driver for the visual desktop scenarios.

The native launcher is `tools/shoot.sh`; this module only speaks QMP against
an already-running QEMU instance.

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
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import glyphs

# Las tablas de fuente salen de la libreria construida, no del .inc: lo que se
# afirma es lo que el proceso mapea de verdad. Se leen una vez.
_FONTS = {}


def fonts():
    if not _FONTS:
        library = Path(os.environ.get("SAVANXP_OUTPUT_ROOT", "build")) / "diskfs" / "lib" / "libsxgfx.so.0.4"
        if not library.exists():
            library = Path("build/diskfs/lib/libsxgfx.so.0.4")
        try:
            _FONTS.update(glyphs.load(library))
        except (glyphs.FontError, OSError) as exc:
            raise Failure(
                "no se pudieron leer las tablas de fuente de %s: %s\n"
                "Este harness afirma sobre los glifos que dibuja libsxgfx; sin las tablas\n"
                "no puede afirmar nada, y seguiria pasando sin comprobar nada." % (library, exc)
            ) from None
    return _FONTS


class Qmp(object):
    def __init__(self, socket_path, timeout=60, abs_pointer=False, screen=(1280, 800)):
        # El puntero del guest es relativo (PS/2) o absoluto (virtio-tablet), y
        # eso cambia por completo como se lo lleva a una posicion. Ver move_to.
        self.abs_pointer = abs_pointer
        self.screen = screen
        deadline = time.time() + timeout
        while True:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(5)
            try:
                self.sock.connect(str(socket_path))
                break
            except OSError:
                self.sock.close()
                if time.time() > deadline:
                    raise RuntimeError("no se pudo conectar a QMP en %s" % socket_path)
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

    def click(self, button="left"):
        self.cmd("input-send-event", events=[
            {"type": "btn", "data": {"down": True, "button": button}}])
        time.sleep(0.25)
        self.cmd("input-send-event", events=[
            {"type": "btn", "data": {"down": False, "button": button}}])
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

    # The keys of the guest's default layout (Spanish) for the punctuation a
    # command line needs: (qcode, shifted). A qcode is the physical key, so a
    # symbol is wherever ES puts it, not where a US keyboard does.
    ES_KEYS = {
        " ": ("spc", False),
        ";": ("comma", True),
        ":": ("dot", True),
        ".": ("dot", False),
        ",": ("comma", False),
        "-": ("slash", False),
        "_": ("slash", True),
        "/": ("7", True),
        "=": ("0", True),
        '"': ("2", True),
        ">": ("less", True),
        "<": ("less", False),
    }

    def type_line(self, text):
        """Types a command line on the guest's ES layout: letters, digits and
        the punctuation in ES_KEYS. Upper case letters go with Shift."""
        for ch in text:
            if ch in self.ES_KEYS:
                qcode, shifted = self.ES_KEYS[ch]
            elif ch.isalpha() and ch.isupper():
                qcode, shifted = ch.lower(), True
            elif ch.isalnum():
                qcode, shifted = ch, False
            else:
                raise ValueError("type_line: no key for %r on the ES layout" % ch)
            if shifted:
                self.key("shift", True)
                time.sleep(0.05)
                self.tap(qcode, pause=0.05)
                self.key("shift", False)
                time.sleep(0.1)
            else:
                self.tap(qcode, pause=0.12)


class Session(object):
    def __init__(self, qmp, out_dir, serial=None):
        self.qmp = qmp
        self.out = out_dir
        # The guest's serial log, for a scenario that restarts the machine and
        # has to know the new boot reached init.
        self.serial = serial
        self.index = 0
        self.progman_open = False

    def open_progman(self):
        """Opens Program Manager from the Start menu, as a user would.

        Only what tests the Start menu or Program Manager themselves goes this
        way; every other program is opened by name through the console (run).
        The menu is mouse only and its list may be longer than the menu, so the
        row is found by its text: first as the menu opens, then after resting
        the pointer on the down arrow strip, which scrolls the list to its end
        (System, where Program Manager lives, is the last group).
        """
        if self.progman_open:
            return
        probe = self.shot("before-progman")
        self.qmp.move_to(*start_button_center(probe))
        self.qmp.click()
        image = wait_menu_open(self, "menu-for-progman")
        mx, my, mw, mh = startmenu_rect(image)
        box = (mx + 2 + STARTMENU_STRIP_WIDTH, my + 2, mx + mw - 2, my + 2 + STARTMENU_VIEW_HEIGHT)
        origin = find_text(image, box, "Program Manager", TEXT, FACE, solid_only=True)[0]
        if origin is None:
            up, down = startmenu_arrow_points(image)
            # The strip's left end, so the cursor neither covers the arrow nor
            # hovers a row (a hovered row is repainted white on blue).
            self.qmp.move_to(down[0] - 90, down[1] + 2)
            for _ in range(10):
                time.sleep(1.0)
                image = self.shot("menu-progman-scrolled")
                if image.getpixel(down) == SHADOW:
                    break
            origin = find_text(image, box, "Program Manager", TEXT, FACE, solid_only=True)[0]
        if origin is None:
            raise Failure("the Start menu does not list Program Manager")
        self.qmp.move_to(origin[0] + 20, origin[1] + 6)
        self.qmp.click()
        time.sleep(25)
        self.progman_open = True

    def open_console(self, timeout=40.0):
        """Opens a Command Prompt with Ctrl+Alt+T and waits until it shows.

        The console is the way into every program now (docs/desktop/CONSOLE.md,
        "In the smokes"): one hotkey, no menu to find by its text and no icon
        grid to count. It is ready when the middle of the screen, where the WM
        centres it, turns into the black of its screen buffer.
        """
        def centre_black(image):
            centre = [image.getpixel((640 + dx, 380 + dy)) for dx in (-40, 0, 40) for dy in (-20, 0, 20)]
            return all(pixel == (0, 0, 0) for pixel in centre)

        # A program that already paints the middle black (Doom, a demo) would
        # make the check pass before the console is there: then only time says
        # when it is.
        already_black = centre_black(self.shot("before-console"))
        self.qmp.key("ctrl", True)
        self.qmp.key("alt", True)
        time.sleep(0.15)
        self.qmp.tap("t")
        time.sleep(0.15)
        self.qmp.key("alt", False)
        self.qmp.key("ctrl", False)
        if already_black:
            time.sleep(10.0)
            return self.shot("console-open")
        deadline = time.time() + timeout
        while time.time() < deadline:
            time.sleep(1.0)
            image = self.shot("console-wait")
            if centre_black(image):
                time.sleep(1.0)
                return image
        raise Failure("the Command Prompt did not open with Ctrl+Alt+T")

    def run(self, command, wait=25):
        """Starts `command` from a Command Prompt that closes behind it.

        `start <command>; exit`: the console opens, launches the program
        without waiting for it, and exits, so the program is the only new
        window and has the focus.
        """
        self.open_console()
        self.qmp.type_line("start %s; exit" % command)
        self.qmp.tap("ret")
        time.sleep(wait)

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

    # Every program by its name, through the Command Prompt (run). Before the
    # console these walked Program Manager's icon grid and counted positions
    # in a list sorted by name, which moved whenever a program was added.
    def open_calc(self):
        self.run("calc")

    def open_files(self):
        self.run("filesapp")

    def open_notepad(self):
        self.run("notepad")

    def open_paint(self):
        self.run("paint")

    def open_webview(self):
        self.run("webview")

    def open_mediaplayer(self):
        self.run("mediaplayer")

    def open_mines(self):
        self.run("mines")

    def open_ccleste(self):
        # An external port: sh finds it in /bin.
        self.run("ccleste")

    def open_doom(self):
        # External too. The scenario that needs Doom checks it is installed.
        self.run("doomgeneric")

    def open_appwiz(self):
        self.run("appwiz")

    def open_system_properties(self):
        self.run("aboutapp")

    def open_task_manager(self):
        self.run("taskmgr")


# Paleta de sxgui, para leer los pixeles con el mismo vocabulario con el que se
# dibujaron (savanxp/sxgui.h).
FACE = (192, 192, 192)
LIGHT = (255, 255, 255)
SHADOW = (128, 128, 128)
DARK = (0, 0, 0)
# El editor pinta su campo con SXGUI_COLOR_FIELD y el texto con SXGUI_COLOR_TEXT,
# que son SXGUI_COLOR_FACE y SXGUI_COLOR_TEXT en sxchrome.h. Espejo, como los de
# la barra de tareas.
FIELD = (255, 255, 255)
TEXT = (0, 0, 0)
SELECT = (0, 0, 128)
SELECT_TEXT = (255, 255, 255)

# El azul de los links (WEBVIEW_LINK_COLOR en webview.c): la asercion lo usa
# con solid_only porque el repintado azul va SOBRE el negro y los bordes
# antialiaseados no salen del blend contra fondo que modela find_text.
LINK = (0, 0, 255)

# Interlineado del toolkit, no de la fuente: sxgui_row_height() en sxgui.c es
# gfx_text_height() + 4. La fuente Noto viene con SX_NOTO_ASCENT 14 y DESCENT 4, o
# sea un LINE_HEIGHT de 18 sin ningun leading, y el toolkit suma 4 encima. Mismo
# convenio que la barra: si sxgui_row_height cambia, esto cambia con el.
SXGUI_ROW_LEADING = 4

# The taskbar's geometry, mirrored from windowd_layout.h and desktop_taskbar.c.
# When either changes this harness has to change with it -- which is the point:
# the number no longer lives only in the code. The two highlight lines along the
# top edge count toward the top margin, so it is 3 and the bottom one is 1.
TASKBAR_HEIGHT = 28
TASKBAR_MARGIN = 2
TASKBAR_MARGIN_TOP = 3
TASKBAR_MARGIN_BOTTOM = 1
TASKBAR_START_WIDTH = 64
TASKBAR_BUTTON_WIDTH = 160
TASKBAR_BUTTON_GAP = 2
STARTMENU_WIDTH = 232
STARTMENU_HEIGHT = 512
STARTMENU_STRIP_WIDTH = 24
STARTMENU_MARGIN = 2
# The NT 4 proportions of desktop_startmenu.c: 28px rows, and a footer of two
# etched separator lines, the gap under them and two rows.
STARTMENU_ROW_HEIGHT = 28
STARTMENU_FOOTER_GAP = 5
STARTMENU_FOOTER_HEIGHT = 2 + STARTMENU_FOOTER_GAP + 2 * STARTMENU_ROW_HEIGHT
# The view is the menu minus its 2px margins and that footer; when the catalog
# overflows it, a 12px arrow strip sits at its top and bottom, each arrow 7px
# wide centred in the strip.
STARTMENU_VIEW_HEIGHT = STARTMENU_HEIGHT - 2 * STARTMENU_MARGIN - STARTMENU_FOOTER_HEIGHT
STARTMENU_SCROLL_STRIP_HEIGHT = 12


class Failure(Exception):
    pass


def button_rect(image, index):
    width, height = image.size
    base = TASKBAR_MARGIN + TASKBAR_START_WIDTH + TASKBAR_BUTTON_GAP
    return (
        base + index * (TASKBAR_BUTTON_WIDTH + TASKBAR_BUTTON_GAP),
        height - TASKBAR_HEIGHT + TASKBAR_MARGIN_TOP,
        TASKBAR_BUTTON_WIDTH,
        TASKBAR_HEIGHT - TASKBAR_MARGIN_TOP - TASKBAR_MARGIN_BOTTOM,
    )


def start_button_center(image):
    width, height = image.size
    return (
        TASKBAR_MARGIN + TASKBAR_START_WIDTH // 2,
        height - TASKBAR_HEIGHT // 2,
    )


def startmenu_rect(image):
    width, height = image.size
    return (
        TASKBAR_MARGIN,
        height - TASKBAR_HEIGHT - STARTMENU_HEIGHT,
        STARTMENU_WIDTH,
        STARTMENU_HEIGHT,
    )


def expect_pixel(image, x, y, colour, label):
    got = image.getpixel((x, y))
    if got != colour:
        raise Failure("%s: pixel (%d,%d) es %s, esperaba %s" % (label, x, y, got, colour))


def expect_button(image, index, active, label):
    """An active window's button is sunken and its face dithered.

    The most useful assertion about this bar: the relief encodes DIRECTLY which
    window is active, so checking it proves the whole chain -- click, request
    to the WM, state change, list republication and the client's repaint --
    without reading a single variable.

    The NT 4 frame (taskbar_button_frame): raised is white above and to the
    left and black below and to the right; sunken is grey and white. The face
    of the active button alternates white and face grey, starting white at its
    top left corner, which a button merely held down does not have.
    """
    x, y, w, h = button_rect(image, index)
    top_left = SHADOW if active else LIGHT
    bottom_right = LIGHT if active else DARK
    state = "sunken" if active else "raised"
    expect_pixel(image, x, y, top_left, "%s: button %d %s (top-left edge)" % (label, index, state))
    expect_pixel(image, x + w - 1, y + h - 1, bottom_right,
                 "%s: button %d %s (bottom-right edge)" % (label, index, state))
    expect_pixel(image, x + 2, y + 2, LIGHT if active else FACE,
                 "%s: button %d face (dither)" % (label, index))
    expect_pixel(image, x + 3, y + 2, FACE,
                 "%s: button %d face" % (label, index))


def expect_taskbar_present(image, label):
    width, height = image.size
    # Right of the buttons and left of the tray (114 px wide, 2 px from the
    # edge): the strip has to be bare face there. Further right is the clock,
    # whose digits change.
    expect_pixel(image, width - TASKBAR_MARGIN - 114 - 20, height - (TASKBAR_HEIGHT // 2), FACE,
                 "%s: the taskbar strip" % label)


def find_text(image, box, text, colour, background, which="body", solid_only=False):
    """Locates a drawn string, or reports the closest thing to it.

    Returns (origin, best_at, matched, total), where origin is the text origin --
    the arguments gfx_blit_text was called with -- or None when nothing matched.
    `matched` counts the glyph's own pixels that came out right at the best offset,
    which is what lets the failure message say something useful.

    No se busca una posicion esperada: se busca ALGUNA. La afirmacion es que las
    letras estan escritas asi, en ese color, sobre ese fondo, en algun punto del
    area -- no que estan donde el codigo dice. Asi el harness no depende de la
    geometria interna del control que se esta mirando, que es justo lo que cambia
    cada vez que se toca el toolkit.

    solid_only mira unicamente los pixeles del glifo con cobertura 255, que
    gfx_pixel_blend escribe tal cual. Es lo que hay que usar cuando el fondo no es
    plano -- el titulo de la ventana activa es un degradado, y una mezcla contra
    un degradado no se puede predecir con un solo color. Se pierde el borde
    anti-aliasing, pero el nucleo del glifo sigue identificando la letra y la
    posicion, que es lo que se queria afirmar.
    """
    font = fonts()[which]
    pattern = glyphs.render(font, 0, 0, text, colour, background, (0, 0, 1 << 20, 1 << 20))
    if solid_only:
        pattern = {at: rgb for at, rgb in pattern.items() if rgb == tuple(colour)}
    if not pattern:
        raise Failure("el texto %r no tiene un solo pixel: nada que afirmar" % text)

    x0, y0, x1, y1 = box
    region = image.crop((x0, y0, x1, y1))
    pixels = region.load()
    region_w, region_h = region.size

    # Ancla: el pixel del patron cuyo color aparece MENOS veces en la region. Un
    # color solido es comun en cualquier interfaz --el negro del texto hay que
    # descartarlo--, mientras que un tono de anti-aliasing casi no aparece en
    # ningun otro sitio, asi que las posiciones candidatas son pocas en vez de miles.
    counts = {}
    for value in list(region.getdata()):
        counts[value] = counts.get(value, 0) + 1
    anchor_dx, anchor_dy = min(pattern, key=lambda k: (counts.get(pattern[k], 0), k))
    anchor_colour = pattern[anchor_dx, anchor_dy]

    items = list(pattern.items())
    best_matched = 0
    best_at = None
    for py in range(region_h):
        for px in range(region_w):
            if pixels[px, py] != anchor_colour:
                continue
            ox = px - anchor_dx
            oy = py - anchor_dy
            matched = 0
            for (dx, dy), expected in items:
                sx = ox + dx
                sy = oy + dy
                if not (0 <= sx < region_w and 0 <= sy < region_h):
                    continue
                if pixels[sx, sy] == expected:
                    matched += 1
            if matched == len(pattern):
                return ((x0 + ox, y0 + oy), None, matched, len(pattern))
            if matched > best_matched:
                best_matched = matched
                best_at = (x0 + ox, y0 + oy)
    return (None, best_at, best_matched, len(pattern))


def expect_text(image, box, text, colour, background, label, which="body", solid_only=False):
    """Falla si la cadena no esta dibujada, y dice cuanto le faltaba si casi lo estaba."""
    if tuple(colour) == tuple(background):
        # Texto del color del fondo es indistinguible de no dibujar nada: todos sus
        # pixels salen igual que el fondo, asi que "esta escrito" y "no esta escrito"
        # dejan la misma pantalla. Afirmar eso seria un paso en falso con forma de
        # comprobacion -- y hay una trampa concreta aqui, porque SXGUI_COLOR_FACE
        # es tambien el fondo de la barra de tareas, de modo que buscar una etiqueta
        # en FACE sobre FACE coincide en cualquier parte de la pantalla.
        #
        # Si el texto se dibuja con el color equivocado, esto tiene que decirse, no
        #decir "no lo encontre".
        raise Failure(
            "%s: no se puede afirmar %r en %s sobre el mismo color %s: pintar texto del "
            "color del fondo es lo mismo que no pintar nada. Si el codigo dibuja el "
            "texto en ese color, esto es un fallo del codigo y no del harness." % (
                label, text, colour, background,
            )
        )
    origin, best_at, matched, total = find_text(image, box, text, colour, background, which, solid_only)
    if origin is not None:
        return (origin[0], origin[1])
    where = "(%d,%d)" % best_at if best_at else "ningun sitio"
    raise Failure(
        "%s: %r no aparece escrito en pantalla. Lo mejor que hubo fueron %d de %d "
        "pixeles del glifo, en %s. Son el texto equivocado, el color equivocado, o "
        "nada dibujado." % (label, text, matched, total, where)
    )


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


def scenario_calc(s):
    """Calculadora: la pantalla alineada a la derecha y el bloque numerico.

    Se teclea por el KEYPAD a proposito. Es el unico camino que no depende del
    layout activo del guest -- kp_add/kp_multiply/kp_divide dan siempre + * /
    (kernel/ps2.cpp, translate_keypad) -- y ademas es el que una persona usaria
    para hacer una cuenta. Los digitos del keypad necesitan NumLock puesto, que
    arranca apagado.
    """
    s.open_calc()
    s.shot("calc-abierta")
    s.qmp.tap("num_lock", pause=0.6)
    for key in ("kp_1", "kp_2", "kp_5"):
        s.qmp.tap(key, pause=0.3)
    s.qmp.tap("kp_divide", pause=0.4)
    s.qmp.tap("kp_8", pause=0.4)
    s.qmp.tap("kp_enter", pause=1.8)
    s.shot("calc-division")
    # Raiz del resultado: el caso que llena la pantalla de decimales y deja ver
    # que el numero crece hacia la izquierda.
    s.qmp.tap("q", pause=1.8)
    s.shot("calc-raiz")


def scenario_mines(s):
    """Buscaminas: el tablero en sus tres estados dibujables a voluntad.

    Todo por TECLADO, que es lo que lo vuelve determinista: con el mouse habria
    que acertarle a una celda de 16 px calculando la posicion de la ventana, y
    el mismo gesto daria una captura distinta en cada corrida. Las flechas mueven
    el cursor, Enter descubre y F pone bandera (mines.c, on_key).

    Lo que hay que mirar: los dos contadores de LEDs y la cara en el panel, el
    bisel levantado de las celdas tapadas contra el hundido del tablero, los
    colores de los numeros y la bandera roja.
    """
    s.open_mines()
    s.shot("mines-abierto")

    # La primera flecha solo hace aparecer el cursor en (0,0); las siguientes lo
    # mueven. Se va al centro, que es donde una primera jugada abre una cascada
    # grande en vez de una sola celda contra el borde.
    s.qmp.tap("down", pause=0.4)
    for _ in range(4):
        s.qmp.tap("right", pause=0.25)
    for _ in range(4):
        s.qmp.tap("down", pause=0.25)
    s.qmp.tap("ret", pause=1.5)
    s.shot("mines-descubierto")

    # Bandera en una celda tapada del borde: el contador de minas tiene que
    # bajar de 010 a 009 en la misma captura.
    for _ in range(4):
        s.qmp.tap("up", pause=0.25)
    s.qmp.tap("f", pause=1.2)
    s.shot("mines-bandera")

    # About en Principiante, el caso que motivo las ventanas con dueno
    # (docs/desktop/OWNED_WINDOWS.md): el dialogo mide casi el doble que la ventana del
    # tablero. Lo que hay que mirar: sale ENTERO, con marco y titulo del WM,
    # centrado sobre el buscaminas y encima de el. ESC lo cierra desde el
    # teclado, que el WM le rutea al dialogo por ser la ventana activa.
    s.qmp.move_to(660, 332)
    s.qmp.click()
    s.shot("mines-menu-ayuda")
    s.qmp.move_to(700, 354)
    s.qmp.click()
    time.sleep(1.5)
    s.shot("mines-about")
    s.qmp.tap("esc", pause=1.2)
    s.shot("mines-about-cerrado")

    # Cambio de nivel, POR MOUSE: el menu no tiene acceso por teclado y los
    # niveles no tienen atajo. Va al final para no correrle las coordenadas a
    # las capturas de arriba.
    #
    # Lo que hay que mirar en las dos ultimas: la ventana tiene que CRECER con
    # el tablero de Experto en vez de quedarse del tamano de Principiante con
    # el tablero grande recortado. Es la mitad "el programa sigue eligiendo su
    # tamano" del flag de ventana fija (docs/desktop/WM_SUBSYSTEM.md).
    s.qmp.move_to(613, 332)
    s.qmp.click()
    s.shot("mines-menu-nivel")

    s.qmp.move_to(632, 433)
    s.qmp.click()
    time.sleep(1.0)
    s.shot("mines-experto")

    # Alt+F4 con teclas reales: el primero cierra el About (es la ventana
    # activa, y a un dialogo se le pide cancelar), el segundo el buscaminas.
    # La ventana crecio con Experto pero su esquina no se movio, asi que el
    # menu Help sigue en el mismo lugar.
    s.qmp.move_to(660, 332)
    s.qmp.click()
    s.qmp.move_to(700, 354)
    s.qmp.click()
    time.sleep(1.5)
    s.shot("mines-experto-about")
    s.qmp.chord("alt", "f4")
    time.sleep(1.0)
    s.shot("mines-alt-f4-dialogo")
    s.qmp.chord("alt", "f4")
    time.sleep(1.5)
    s.shot("mines-alt-f4-ventana")


def scenario_paint(s):
    """Paint: la caja a la izquierda, los colores arriba, el lienzo al centro.

    Todo por TECLADO, igual que mines: 1-7 cambian de herramienta y la barra
    de estado lo refleja. Se afirma el texto de la barra en algun sitio de la
    pantalla (find_text), no en una posicion: el harness no depende de la
    geometria interna de la ventana, que es lo que cambia cada vez que se toca
    el layout.
    """
    s.open_paint()
    # solid_only en las cinco: el borde antialiaseado de estos glifos sale
    # hasta 8 niveles distinto entre el host y el guest (medido: 283/287 en
    # "(untitled)", 4 pixeles de franja). El nucleo con cobertura 255 sigue
    # identificando cada letra y su posicion, que es lo que se afirma.
    image = s.shot("paint-abierta")
    expect_text(image, (0, 0, 1280, 800), "(untitled)", TEXT, FACE, "la barra de estado de paint",
                solid_only=True)
    expect_text(image, (0, 0, 1280, 800), "Pencil", TEXT, FACE, "la herramienta inicial de paint",
                solid_only=True)
    expect_text(image, (0, 0, 1280, 800), "undo 0", TEXT, FACE, "el undo vacio de paint",
                solid_only=True)
    s.qmp.tap("2", pause=2.0)
    image = s.shot("paint-pincel")
    expect_text(image, (0, 0, 1280, 800), "Brush 9px", TEXT, FACE, "el pincel en la barra de paint",
                solid_only=True)
    s.qmp.tap("5", pause=2.0)
    image = s.shot("paint-cubo")
    expect_text(image, (0, 0, 1280, 800), "Fill", TEXT, FACE, "el cubo en la barra de paint",
                solid_only=True)
    s.qmp.tap("8", pause=2.0)
    image = s.shot("paint-elipse")
    expect_text(image, (0, 0, 1280, 800), "Ellipse", TEXT, FACE, "la elipse en la barra de paint",
                solid_only=True)


def files_choose_view(s, image, item):
    """Picks `item` ("Large Icons" or "Details") from Files' View menu."""
    title = find_text(image, (0, 0, 1280, 770), "View", TEXT, FACE)[0]
    if title is None:
        raise Failure("Files has no View menu")
    s.qmp.move_to(title[0] + 6, title[1] + 4)
    s.qmp.click()
    time.sleep(1.0)
    image = s.shot("files-view-menu")
    entry = find_text(image, (0, 0, 1280, 770), item, TEXT, FACE)[0]
    if entry is None:
        raise Failure("the View menu has no %s" % item)
    s.qmp.move_to(entry[0] + 6, entry[1] + 4)
    s.qmp.click()
    time.sleep(1.2)
    return s.shot("files-view-" + item.lower().replace(" ", "-"))


def scenario_files(s):
    """Files: the window with the most different controls at once.

    Address bar, button, the folder as Large Icons and then as a details list
    with column headers, and a two-pane status bar: if a toolkit margin or
    indent slips, it shows here. The folder opens with nothing selected; the
    first arrow selects the first icon and the next one moves right, so the
    highlighted item is in view too.
    """
    s.open_files()
    image = s.shot("files-raiz")
    if find_text(image, (0, 0, 1280, 770), "Type", TEXT, FACE)[0] is not None:
        raise Failure("Files does not open as Large Icons")
    s.qmp.tap("down", pause=0.6)
    s.qmp.tap("right", pause=1.2)
    s.shot("files-seleccion")
    image = files_choose_view(s, image, "Details")
    expect_text(image, (0, 0, 1280, 770), "Type", TEXT, FACE,
                "View > Details shows the column headers")
    # Back to Large Icons, in a folder bigger than the window: End has to
    # scroll the grid so the last item is in view, with the bar beside it.
    files_choose_view(s, image, "Large Icons")
    s.qmp.chord("alt", "f4")
    time.sleep(1.5)
    s.run("filesapp /bin")
    s.qmp.tap("end", pause=1.2)
    s.shot("files-bin-end")
    # The profile mixes folders with .cfg and .ini files, so it is where the
    # per-type icon layer resolves more than one type at once, and the shots
    # above serve as the catalog's regression at both sizes.


def scenario_filesmenu(s):
    """An application's context menu (docs/desktop/DESKTOP_ICONS.md#phase-4).

    Files opens on the user's profile, where the Desktop folder is. A right
    click on its icon's name opens the item's menu in a popup window of its own; the
    window keeps the keyboard, so the arrows and Enter choose Send To
    Desktop, and the desktop shows the new shortcut once it re-reads its
    folder.
    """
    WHITE = (255, 255, 255)
    s.open_files()
    image = s.shot("files")
    origin = find_text(image, (0, 0, 1280, 770), "Desktop", TEXT, FIELD)[0]
    if origin is None:
        raise Failure("Files does not list the Desktop folder")
    s.qmp.move_to(origin[0] + 10, origin[1] + 6)
    s.qmp.click("right")
    time.sleep(1.5)
    image = s.shot("files-menu")
    expect_text(image, (0, 0, 1280, 770), "Send To Desktop", TEXT, FACE,
                "a right click on a row opens its menu")
    # Down from nothing is Open; the separator is skipped on the way.
    s.qmp.tap("down", pause=0.4)
    s.qmp.tap("down", pause=0.6)
    image = s.shot("files-menu-keyboard")
    expect_text(image, (0, 0, 1280, 770), "Send To Desktop", SELECT_TEXT, SELECT,
                "the window's keys move through its menu")
    s.qmp.tap("ret", pause=1.0)
    image = s.shot("files-sent")
    if find_text(image, (0, 0, 1280, 770), "Send To Desktop", TEXT, FACE)[0]:
        raise Failure("the menu is still open after choosing an item")

    # Near the list's right edge the menu has to leave the window: it is a
    # window of its own, not drawn inside Files (which would flip it left).
    # The Type column of the details view is the ruler.
    image = files_choose_view(s, image, "Details")
    header = find_text(image, (0, 0, 1280, 770), "Type", TEXT, FACE)[0]
    if header is None:
        raise Failure("Files has no Type column to measure from")
    click = (header[0] + 120, header[1] + 220)
    s.qmp.move_to(*click)
    s.qmp.click("right")
    time.sleep(1.5)
    image = s.shot("files-menu-outside")
    expect_text(image, (click[0], click[1], click[0] + 200, click[1] + 80), "Up One Level", TEXT, FACE,
                "the folder's menu opens at the pointer")
    outside = (click[0] + 100, click[1] + 8)
    if image.getpixel(outside) != FACE:
        raise Failure("the menu does not reach %r, outside the window: it is not a popup of its own"
                      % (outside,))
    s.qmp.tap("esc", pause=1.0)
    # Close Files and let the desktop re-read its folder.
    s.qmp.chord("alt", "f4")
    time.sleep(4.0)
    image = s.shot("desktop-shortcut")
    expect_text(image, (0, 0, 400, 400), "Desktop", WHITE, FACE,
                "the desktop shows the shortcut Send To made", solid_only=True)


def files_click_text(s, image, text, label, box=(0, 0, 1280, 770)):
    """Clicks a toolbar button or a menu item by its caption, searched in
    `box`: a short caption such as "Up" also matches stray pixels elsewhere."""
    origin = find_text(image, box, text, TEXT, FACE)[0]
    if origin is None:
        raise Failure(label)
    s.qmp.move_to(origin[0] + 6, origin[1] + 4)
    s.qmp.click()
    time.sleep(1.2)


def scenario_filestoolbar(s):
    """Files' toolbar and address bar (docs/desktop/FILES.md).

    Back and Forward start disabled (their captions are etched, so the
    black-on-grey text is not found). Typing /bin in the address bar goes
    there; Back returns to the profile, Forward to /bin again and Up to the
    root. With nothing selected, Properties describes the folder itself, and
    the Views button drops down the view menu.
    """
    s.open_files()
    image = s.shot("toolbar")
    # Properties is always enabled: its caption gives the toolbar's row.
    caption = find_text(image, (0, 0, 1280, 770), "Properties", TEXT, FACE)[0]
    if caption is None:
        raise Failure("the toolbar has no Properties")
    toolbar = (0, caption[1] - 4, 1280, caption[1] + 20)
    if find_text(image, toolbar, "Back", TEXT, FACE)[0] is not None:
        raise Failure("Back is enabled with no history")
    field = find_text(image, (0, 0, 1280, 770), "/users/user", TEXT, FIELD)[0]
    if field is None:
        raise Failure("the address bar does not show the folder")
    s.qmp.move_to(field[0] + 200, field[1] + 4)
    s.qmp.click()
    time.sleep(0.6)
    for _ in range(len("/users/user")):
        s.qmp.tap("backspace", pause=0.08)
    s.qmp.type_line("/bin")
    s.qmp.tap("ret", pause=1.5)
    image = s.shot("address-bin")
    expect_text(image, (0, 0, 1280, 770), "/bin", TEXT, FIELD, "Enter in the address bar goes there")
    expect_text(image, (0, 0, 1280, 770), "aboutapp", TEXT, FIELD, "the list shows /bin")

    files_click_text(s, image, "Back", "Back is not enabled after going somewhere", toolbar)
    image = s.shot("back")
    expect_text(image, (0, 0, 1280, 770), "/users/user", TEXT, FIELD, "Back returns to the profile")
    files_click_text(s, image, "Forward", "Forward is not enabled after Back", toolbar)
    image = s.shot("forward")
    expect_text(image, (0, 0, 1280, 770), "aboutapp", TEXT, FIELD, "Forward goes to /bin again")
    files_click_text(s, image, "Up", "Up is not enabled under the root", toolbar)
    image = s.shot("up")
    expect_text(image, (0, 0, 1280, 770), "wallpapers", TEXT, FIELD, "Up goes to the root")

    files_click_text(s, image, "Properties", "the toolbar has no Properties", toolbar)
    time.sleep(0.8)
    image = s.shot("properties")
    expect_text(image, (0, 0, 1280, 770), "Contains:", TEXT, FACE,
                "Properties with nothing selected describes the folder")
    s.qmp.tap("ret", pause=1.0)

    image = s.shot("properties-closed")
    files_click_text(s, image, "Views", "the toolbar has no Views", toolbar)
    image = s.shot("views-menu")
    files_click_text(s, image, "Details", "the Views button does not drop down the views")
    image = s.shot("views-details")
    expect_text(image, (0, 0, 1280, 770), "Type", TEXT, FACE, "Views > Details shows the column headers")


def scenario_taskmgrmenu(s):
    """A SUN program's context menu (docs/desktop/DESKTOP_ICONS.md#phase-4).

    Task Manager runs on sxngui, whose frame opens the menu as a popup window
    too. A right click on Notepad's row of the Applications tab opens Switch
    To / End Task; the window keeps the keyboard, so Up and Enter choose End
    Task, and Notepad closes.
    """
    s.open_notepad()
    s.open_task_manager()
    # Task Manager sizes itself after it opens, and the WM places it again:
    # measure only once two shots agree.
    image = s.shot("taskmgr")
    for _ in range(5):
        time.sleep(2.0)
        again = s.shot("taskmgr")
        if list(again.getdata()) == list(image.getdata()):
            break
        image = again
    # The Applications list, measured from its Status header: the row names
    # the window by its title, "notepad".
    header = find_text(image, (0, 0, 1280, 770), "Status", TEXT, FACE)[0]
    if header is None:
        raise Failure("Task Manager does not show the Applications tab")
    rows = (max(0, header[0] - 520), header[1] + 10, header[0], header[1] + 200)
    row = None
    for colour, background in ((TEXT, FIELD), (SELECT_TEXT, SELECT)):
        row = find_text(image, rows, "notepad", colour, background)[0]
        if row is not None:
            break
    if row is None:
        raise Failure("Task Manager does not list Notepad in Applications")
    click = (row[0] + 10, row[1] + 6)
    s.qmp.move_to(*click)
    s.qmp.click("right")
    time.sleep(1.5)
    image = s.shot("taskmgr-menu")
    near = (click[0], click[1], click[0] + 220, click[1] + 90)
    expect_text(image, near, "Switch To", TEXT, FACE,
                "a right click on a task opens its menu at the pointer")
    # Up from nothing wraps to the last row, End Task.
    s.qmp.tap("up", pause=0.6)
    image = s.shot("taskmgr-menu-keyboard")
    expect_text(image, near, "End Task", SELECT_TEXT, SELECT,
                "the window's keys move through its menu")
    s.qmp.tap("ret", pause=4.0)
    # The cursor off the list: over a selected row it could pass for text.
    s.qmp.move_to(1200, 100)
    time.sleep(0.5)
    image = s.shot("taskmgr-ended")
    # Absence is asserted in black only: the UI font has no anti-aliasing,
    # so white text on the selection would match any white part of the list.
    if find_text(image, rows, "notepad", TEXT, FIELD)[0] or \
            find_text(image, (0, 772, 1280, 800), "notepad", TEXT, FACE)[0]:
        raise Failure("End Task from the menu left Notepad running")

    # Task Manager's own row, near the list's right edge: the menu has to
    # leave the window, so it is a popup of its own and not drawn inside.
    click = (header[0] + 100, header[1] + 24)
    s.qmp.move_to(*click)
    s.qmp.click("right")
    time.sleep(1.5)
    image = s.shot("taskmgr-menu-outside")
    expect_text(image, (click[0], click[1], click[0] + 220, click[1] + 90), "Switch To", TEXT, FACE,
                "the menu opens at the pointer near the edge")
    outside = (click[0] + 75, click[1] + 8)
    if image.getpixel(outside) != FACE:
        raise Failure("the menu does not reach %r, outside the window: it is not a popup of its own"
                      % (outside,))
    s.qmp.tap("esc", pause=1.0)


def scenario_console(s):
    """Command Prompt: opened with its hotkey, a builtin, a program's output,
    the history, scrolling past a screen, and Ctrl+C on a program that would
    never end."""
    s.open_console()
    s.shot("console-open")
    s.qmp.type_line("help")
    time.sleep(1.0)
    s.shot("console-typing")
    s.qmp.tap("ret", pause=3.0)
    s.shot("console-help")
    s.qmp.type_line("ls /bin")
    s.qmp.tap("ret", pause=4.0)
    s.shot("console-ls")
    s.qmp.tap("up", pause=0.8)
    s.qmp.tap("up", pause=0.8)
    s.shot("console-history")
    s.qmp.tap("esc", pause=0.5)
    s.qmp.tap("pgup", pause=1.0)
    s.shot("console-scrolled")
    s.qmp.type_line("ticker A 1000 300")
    s.qmp.tap("ret", pause=4.0)
    s.shot("console-ticker")
    s.qmp.key("ctrl", True)
    time.sleep(0.1)
    s.qmp.tap("c")
    s.qmp.key("ctrl", False)
    time.sleep(3.0)
    s.shot("console-interrupted")


def scenario_system(s):
    """System Properties and Task Manager, two pages of each.

    Both are opened by name through the console. The pages are walked by
    keyboard, the only way that does not depend on where the WM put the
    window: in System Properties the focus starts on the tab control, so the
    arrow is enough; in Task Manager it starts on the process list and goes up
    to the tabs with SHIFT+TAB.

    No process is ever ended: this harness takes pictures. The dialog is
    opened to see the warning and cancelled with ESC, which is also what its
    default button does.
    """
    s.open_system_properties()
    s.shot("system-general")
    s.qmp.tap("right", pause=2.5)
    s.shot("system-hardware")
    s.qmp.tap("esc", pause=6.0)

    s.open_task_manager()
    # It opens on Applications, with the focus on the task list.
    s.shot("taskmgr-applications")
    # From the list to the tab control with SHIFT+TAB, which goes to the
    # PREVIOUS widget. Counting TABs forward does not work: a disabled button
    # takes no focus, and which ones are disabled depends on the selection.
    s.qmp.chord("shift", "tab")
    time.sleep(1.0)
    s.qmp.tap("right", pause=3.0)
    s.shot("taskmgr-processes")
    s.qmp.tap("right", pause=3.0)
    s.shot("taskmgr-performance")
    s.qmp.tap("right", pause=3.0)
    s.shot("taskmgr-networking")
    s.qmp.tap("left", pause=1.0)
    s.qmp.tap("left", pause=2.5)
    # Back to the process list (TAB skips the hidden Applications controls)
    # and to its LAST row, the Task Manager itself: on the idle process -- row
    # 0 -- ending is disabled on purpose, and the only risk of one Enter too
    # many here is this window closing itself.
    # The pauses are generous on purpose: under TCG the guest runs much slower
    # than the host clock, and a Del that arrives before the selection moved
    # lands on the idle process, where ending does nothing.
    s.qmp.tap("tab", pause=2.0)
    s.qmp.tap("end", pause=4.0)
    s.qmp.tap("delete", pause=4.0)
    s.shot("taskmgr-end-process")
    s.qmp.tap("esc", pause=2.0)


def scenario_display(s):
    """Display Properties, both tabs (docs/desktop/DISPLAY_PROPERTIES.md).

    Opened by name through the console. The focus starts on the wallpaper
    list; SHIFT+TAB takes it back to the tab control and the arrow walks to
    Settings. ESC is Cancel, so nothing is written.
    """
    s.run("display")
    image = s.shot("display-desktop")
    expect_text(image, (0, 0, image.size[0], image.size[1]), "Background:", TEXT, FACE,
                "the Desktop tab lists the wallpapers")
    s.qmp.chord("shift", "tab")
    time.sleep(1.0)
    s.qmp.tap("right", pause=3.0)
    image = s.shot("display-settings")
    expect_text(image, (0, 0, image.size[0], image.size[1]), "Screen resolution", TEXT, FACE,
                "the Settings tab shows the resolution")
    s.qmp.tap("esc", pause=2.0)


def scenario_effects(s):
    """Apply/Cancel, independent flags and a real restart of saved effects."""
    labels = ("Show shadows under windows", "Show shadow under mouse pointer")

    def click_text(text):
        image = s.shot("find-" + text.split()[0].lower())
        origin = find_text(image, (image.width // 4, image.height // 8, image.width * 3 // 4, image.height * 7 // 8), text, TEXT, FACE)[0]
        if origin is None:
            raise Failure("missing effects control: " + text)
        s.qmp.move_to(origin[0] + 8, origin[1] + 5)
        s.qmp.click()
        time.sleep(1)

    def open_settings():
        s.run("display")
        s.qmp.chord("shift", "tab")
        s.qmp.tap("right", pause=2)

    def open_effects(expected):
        click_text("Effects...")
        image = s.shot("effects-options")
        for label, checked in zip(labels, expected):
            origin = find_text(image, (image.width // 4, image.height // 8, image.width * 3 // 4, image.height * 7 // 8), label, TEXT, FACE)[0]
            if origin is None:
                raise Failure("missing effects checkbox: " + label)
            # 13px checkbox, then the toolkit's 6px gap. Only its interior
            # counts, so the dark sunken border cannot masquerade as a tick.
            x, y = origin
            tick = any(image.getpixel((xx, yy)) == TEXT
                       for yy in range(y + 3, y + 10) for xx in range(x - 16, x - 9))
            if tick != checked:
                raise Failure("unexpected checkbox state: %s = %s" % (label, tick))

    open_settings()
    open_effects((True, True))
    click_text(labels[0])
    s.qmp.tap("esc", pause=1)
    open_effects((True, True))  # Inner Cancel discards edits.
    click_text(labels[0])
    s.qmp.tap("ret", pause=1)
    s.qmp.tap("esc", pause=2)  # Outer Cancel discards staged edits.
    open_settings()
    open_effects((True, True))
    current = (True, True)
    applied = {}
    for flags in ((False, True), (False, False), (True, False), (True, True), (False, False)):
        # The preceding dialog is open; change only mismatching options.
        for i, desired in enumerate(flags):
            if desired != current[i]:
                click_text(labels[i])
        s.qmp.tap("ret", pause=1)
        click_text("Apply")
        applied[flags] = s.shot("effects-applied-%d%d" % flags)
        open_effects(flags)
        current = flags
    s.qmp.tap("esc", pause=1)
    s.qmp.tap("esc", pause=2)

    # Compare the same sheet on the same wallpaper, with the pointer shadow
    # disabled in both shots. Inspect only pixels outside its opaque frame.
    off, on = applied[(False, False)], applied[(True, False)]
    face = [(x, y) for y in range(100, off.height - 100)
            for x in range(off.width // 4, off.width * 3 // 4) if off.getpixel((x, y)) == FACE]
    if not face:
        raise Failure("effects: no property sheet found")
    left, right = min(x for x, y in face) - 2, max(x for x, y in face) + 2
    top, bottom = min(y for x, y in face) - 3, max(y for x, y in face) + 2
    darkened = 0
    for y in range(max(0, top - 12), min(off.height, bottom + 16)):
        for x in range(max(0, left - 12), min(off.width, right + 16)):
            if left <= x <= right and top <= y <= bottom:
                continue
            before, after = off.getpixel((x, y)), on.getpixel((x, y))
            darkened += all(a <= b for a, b in zip(after, before)) and after != before
    if darkened < 100:
        raise Failure("effects: enabling window shadows did not darken the outside of the frame")

    marker = "handoff: starting /bin/init"
    with io.open(s.serial, encoding="utf-8", errors="replace") as log:
        boots = log.read().count(marker)
    s.qmp.cmd("set-action", reboot="reset")
    s.qmp.cmd("system_reset")
    deadline = time.time() + 240
    while True:
        with io.open(s.serial, encoding="utf-8", errors="replace") as log:
            if log.read().count(marker) > boots:
                break
        if time.time() > deadline:
            raise Failure("effects persistence: machine did not restart")
        time.sleep(0.5)
    time.sleep(75)
    open_settings()
    open_effects((False, False))
    s.qmp.tap("esc", pause=1)
    s.qmp.tap("esc", pause=1)


def scenario_resolution(s):
    """Changing the resolution (docs/desktop/DISPLAY_PROPERTIES.md).

    Display Properties' Settings tab, by keyboard: SHIFT+TAB to the tabs, the
    arrow to Settings, TAB to the slider, Home and two steps right is
    1024x768, Enter is OK. The screen changes mode at once and asks whether to
    keep it; Enter is Yes. Then once more to 800x600 with Apply, and no answer:
    after the countdown the screen is back at 1024x768. Last, a restart: the
    desktop sets the kept 1024x768 again.
    """
    def open_settings():
        s.run("display")
        s.qmp.chord("shift", "tab")
        time.sleep(1.0)
        s.qmp.tap("right", pause=2.0)
        s.qmp.tap("tab", pause=1.0)

    before = s.shot("native")
    open_settings()
    s.qmp.tap("home", pause=1.0)
    s.qmp.tap("right", pause=1.0)
    s.qmp.tap("right", pause=2.0)
    image = s.shot("slider-1024")
    expect_text(image, (0, 0, image.size[0], image.size[1]), "1024 by 768 pixels", TEXT, FACE,
                "the slider names the mode it is on")
    s.qmp.tap("ret", pause=8.0)
    image = s.shot("keep-settings")
    if image.size != (1024, 768):
        raise Failure("after OK the screen should be 1024x768, not %dx%d" % image.size)
    expect_text(image, (0, 0, 1024, 768), "Keep these settings?", TEXT, FACE,
                "a new mode asks to be kept")
    s.qmp.tap("ret", pause=6.0)
    image = s.shot("kept")
    if image.size != (1024, 768):
        raise Failure("Yes should keep 1024x768, the screen is %dx%d" % image.size)
    expect_taskbar_present(image, "the taskbar follows the new width")

    open_settings()
    s.qmp.tap("left", pause=2.0)
    s.qmp.tap("tab", pause=0.5)   # colour quality
    s.qmp.tap("tab", pause=0.5)   # Effects
    s.qmp.tap("tab", pause=0.5)   # OK
    s.qmp.tap("tab", pause=0.5)   # Cancel
    s.qmp.tap("tab", pause=0.5)   # Apply
    s.qmp.tap("spc", pause=8.0)
    image = s.shot("trial-800")
    if image.size != (800, 600):
        raise Failure("Apply should try 800x600, the screen is %dx%d" % image.size)
    time.sleep(25.0)
    image = s.shot("reverted")
    if image.size != (1024, 768):
        raise Failure("without an answer the screen should go back to 1024x768, not %dx%d" % image.size)
    s.qmp.tap("esc", pause=5.0)

    # The kept mode is the user's: after a restart the desktop sets it again.
    # QEMU runs with -no-reboot, which turns a reset into a stop: set-action
    # makes this one a real restart. The new boot has to reach init, or the
    # shot below would be the old session's last frame.
    if s.serial is None:
        raise Failure("the restart needs the serial log")
    marker = "handoff: starting /bin/init"
    with io.open(s.serial, "r", encoding="utf-8", errors="replace") as fh:
        boots = fh.read().count(marker)
    s.qmp.cmd("set-action", reboot="reset")
    s.qmp.cmd("system_reset")
    deadline = time.time() + 240
    while True:
        with io.open(s.serial, "r", encoding="utf-8", errors="replace") as fh:
            if fh.read().count(marker) > boots:
                break
        if time.time() > deadline:
            raise Failure("the machine did not boot again after the reset")
        time.sleep(0.5)
    time.sleep(75.0)
    image = s.shot("after-restart")
    if image.size != (1024, 768):
        raise Failure("after a restart the saved 1024x768 should be back, the screen is %dx%d" % image.size)
    expect_taskbar_present(image, "the taskbar at the saved width")


def scenario_appwiz(s):
    """Agregar o quitar programas: la ventana que desinstala.

    Se llega por el grupo System del launcher, que es donde la declara su
    .sxres. La lista muestra lo instalado FUERA de la imagen del sistema, asi
    que con Doom instalado tiene exactamente una fila; sin el, la captura seria
    de una lista vacia y no se veria ni el icono, ni el tamano, ni la casilla
    de datos.

    NO se aprieta Remove: este harness saca fotos, no desinstala. El que valida
    el borrado es `build.sh smoke appwiz-smoke`.
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


def scenario_logview(s):
    """Log Viewer (docs/userland/LOG_VIEWER.md): the system log, its
    categories, a kernel subsystem picked from them, and Find."""
    s.run("logview")
    image = s.shot("logview")
    # "All lines" starts selected, so its neighbours are what is on white.
    if find_text(image, (0, 0, 1280, 770), "Problems", TEXT, FIELD)[0] is None:
        raise Failure("Log Viewer does not list its categories")
    kernel = find_text(image, (0, 0, 1280, 770), "Kernel", TEXT, FIELD)[0]
    if kernel is None:
        raise Failure("Log Viewer has no Kernel category")
    if find_text(image, (0, 0, 1280, 770), "Message", TEXT, FACE)[0] is None:
        raise Failure("Log Viewer's list has no header")
    # The row under Kernel is its first subsystem.
    s.qmp.move_to(kernel[0] + 20, kernel[1] + 20)
    s.qmp.click()
    time.sleep(1.5)
    image = s.shot("logview-subsystem")
    if find_text(image, (0, 0, 1280, 770), "Programs", TEXT, FIELD)[0] is None:
        raise Failure("Log Viewer shows no programs: the system's lines are everyone's")
    # The first line of that subsystem: the panel below shows it whole.
    header = find_text(image, (0, 0, 1280, 770), "Message", TEXT, FACE)[0]
    if header is None:
        raise Failure("Log Viewer's list lost its header")
    s.qmp.move_to(header[0] + 40, header[1] + 22)
    s.qmp.click()
    time.sleep(1.5)
    s.qmp.chord("ctrl", "c")
    time.sleep(1.0)
    image = s.shot("logview-copied")
    if find_text(image, (0, 0, 1280, 770), "Copied line", TEXT, FACE)[0] is None:
        raise Failure("Ctrl+C did not copy the selected line")


def scenario_diskmgmt(s):
    """Disk Management (docs/userland/DISK_MANAGEMENT.md): the volume list,
    the disk map, Properties, then Initialize and New Partition on a blank
    disk through the real consent prompt.

    shoot.sh attaches a blank 64 MB disk (serial SXBLANK0) for this scenario
    only; the system's disks are never written. The writes themselves are
    checked by `build.sh smoke diskmgmt-smoke`; this looks at the dialogs and
    the map after each step.
    """
    def settle(name):
        image = s.shot(name)
        for _ in range(5):
            time.sleep(2.0)
            again = s.shot(name)
            if list(again.getdata()) == list(image.getdata()):
                break
            image = again
        return image

    def select_on_map(point, status, image):
        # The pointer is relative and a step can be lost: click, then check
        # the status bar names what was clicked, and try again if not.
        for _ in range(3):
            s.qmp.move_to(*point)
            s.qmp.click()
            time.sleep(1.5)
            image = s.shot("diskmgmt-select")
            if find_text(image, (0, 600, 1280, 680), status, TEXT, FACE)[0] is not None:
                return image
        raise Failure("the map did not select %r" % status)

    def context_item(downs):
        # Shift+F10 opens the selection's menu; each Down moves one item.
        s.qmp.chord("shift", "f10")
        time.sleep(1.5)
        for _ in range(downs):
            s.qmp.tap("down", pause=0.4)
        s.qmp.tap("ret", pause=3.0)

    def consent(name):
        # The prompt arms after a moment; Y is the only answer that counts.
        time.sleep(3.0)
        s.shot(name)
        s.qmp.tap("y", pause=0.0)
        time.sleep(8.0)

    s.run("diskmgmt")
    image = settle("diskmgmt")
    header = find_text(image, (0, 0, 1280, 770), "File System", TEXT, FACE)[0]
    if header is None:
        raise Failure("Disk Management does not show its volume list")
    expect_text(image, (0, header[1], 1280, 770), "Online", TEXT, FACE,
                "each disk's box says it is online")
    expect_text(image, (0, header[1], 1280, 770), "Healthy (System)", TEXT, FIELD,
                "the root volume is drawn on the map as the system's")
    # The root's row, by its mount point, then Alt+Enter for Properties.
    row = find_text(image, (0, header[1] + 8, 1280, header[1] + 120), "/", TEXT, FIELD)[0]
    if row is None:
        raise Failure("Disk Management does not list the root volume")
    s.qmp.move_to(row[0] + 2, row[1] + 6)
    s.qmp.click()
    time.sleep(1.5)
    s.qmp.chord("alt", "ret")
    time.sleep(3.0)
    image = s.shot("diskmgmt-properties")
    expect_text(image, (0, 0, 1280, 800), "Mounted at:", TEXT, FACE,
                "Properties shows where the volume is mounted")
    s.qmp.tap("esc", pause=2.0)

    # The blank disk: select its box, then Initialize from its context menu.
    image = settle("diskmgmt-blank")
    box = find_text(image, (0, header[1], 1280, 770), "Not Initialized", TEXT, FACE)[0]
    if box is None:
        raise Failure("the blank disk is not shown as Not Initialized")
    select_on_map((box[0] + 4, box[1] + 4), "QEMU HARDDISK, 64 MB", image)
    context_item(1)
    image = s.shot("diskmgmt-initialize")
    expect_text(image, (0, 0, 1280, 800), "with a GPT partition table?", TEXT, FACE,
                "Initialize asks before writing")
    # No has the focus; Tab moves it to Yes.
    s.qmp.tap("tab", pause=0.5)
    s.qmp.tap("ret", pause=0.5)
    consent("diskmgmt-initialize-consent")
    image = settle("diskmgmt-initialized")
    expect_text(image, (0, header[1], 1280, 770), "Free space", TEXT, FIELD,
                "the initialized disk shows its free space")

    # New Partition from the same box: second in its menu, after the now
    # disabled Initialize (Down stops on disabled items, as in Windows).
    context_item(2)
    image = s.shot("diskmgmt-new-partition")
    expect_text(image, (0, 0, 1280, 800), "Format as SxFS", TEXT, FACE,
                "New Partition offers to format")
    s.qmp.tap("ret", pause=1.0)
    consent("diskmgmt-new-partition-consent")
    image = settle("diskmgmt-partitioned")
    expect_text(image, (0, header[1], 1280, 770), "New Volume", TEXT, FIELD,
                "the new partition is on the map with its label")

    # Mount it: select its segment, then Mount... (Format, Delete, Mount).
    volume = find_text(image, (0, header[1] + 120, 1280, 770), "New Volume", TEXT, FIELD)[0]
    if volume is None:
        raise Failure("the new partition is not on the map")
    select_on_map((volume[0] + 4, volume[1] + 4), "SavanXP SxFS", image)
    context_item(3)
    image = s.shot("diskmgmt-mount")
    expect_text(image, (0, 0, 1280, 800), "Mount the volume as a folder under /mnt:", TEXT, FACE,
                "Mount asks for the folder")
    s.qmp.tap("ret", pause=1.0)
    consent("diskmgmt-mount-consent")
    image = settle("diskmgmt-mounted")
    # In the list, where its row is the selected one: on the map the pointer
    # and the selection's hatch sit over the text.
    expect_text(image, (0, header[1], 1280, header[1] + 120), "/mnt/new-volume", SELECT_TEXT, SELECT,
                "the mounted volume is named by its mount point")


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


def scenario_contextmenu(s):
    """The desktop's context menu (docs/desktop/DESKTOP_ICONS.md).

    A right click on the background opens it with its corner at the pointer;
    near the bottom-right corner it opens to the left of and above the
    pointer instead. Hovering a row highlights it. The right click gave the
    desktop the keyboard, so the arrows and Enter choose Properties, which
    opens Display Properties (docs/desktop/DISPLAY_PROPERTIES.md). There Home
    picks "(None)" and Enter is OK, which leaves the background a plain colour.
    """
    before = s.shot("desktop")
    probe = (300, 200)

    s.qmp.move_to(400, 300)
    s.qmp.click("right")
    time.sleep(1.0)
    image = s.shot("menu-open")
    expect_text(image, (400, 300, 660, 500), "Arrange Icons", TEXT, FACE,
                "the menu opens at the pointer")
    expect_text(image, (400, 300, 660, 500), "Open Command Prompt", TEXT, FACE,
                "the menu lists the Command Prompt")

    # The first row: below the raised border, half a row down, and in the
    # check gutter so the cursor does not cover the text.
    s.qmp.move_to(408, 312)
    time.sleep(1.0)
    image = s.shot("menu-hover")
    expect_text(image, (400, 300, 660, 330), "Arrange Icons", SELECT_TEXT, SELECT,
                "hovering highlights the row")

    # A right click elsewhere closes it and opens it again there; at the
    # corner it has to flip so it stays above the taskbar.
    s.qmp.move_to(1240, 760)
    s.qmp.click("right")
    time.sleep(1.0)
    image = s.shot("menu-flipped")
    expect_text(image, (900, 480, 1240, 760), "Properties", TEXT, FACE,
                "near the corner the menu opens left of and above the pointer")

    # Up from nothing wraps to the last row, Properties.
    s.qmp.tap("up", pause=0.4)
    image = s.shot("menu-keyboard")
    expect_text(image, (900, 480, 1240, 760), "Properties", SELECT_TEXT, SELECT,
                "the arrows move through the menu")
    s.qmp.tap("ret", pause=6.0)
    image = s.shot("display-properties")
    if find_text(image, (900, 480, 1240, 760), "Open Command Prompt", TEXT, FACE)[0]:
        raise Failure("the menu is still open after running an item")
    expect_text(image, (0, 0, image.size[0], image.size[1]), "Background:", TEXT, FACE,
                "Properties opens Display Properties on its Desktop tab")

    # The focus starts on the wallpaper list: Home is "(None)", Enter is OK.
    s.qmp.tap("home", pause=2.0)
    s.qmp.tap("ret", pause=4.0)
    after = s.shot("wallpaper-none")
    if after.getpixel(probe) != (0, 128, 128):
        raise Failure("with no wallpaper the background at %r should be teal, not %r (it was %r)"
                      % (probe, after.getpixel(probe), before.getpixel(probe)))


# The desktop's grid (desktop_view.c): cells of 76x74 from (6, 6), the 32x32
# icon centred at the top of its cell and the label 36 px down.
def desktop_cell_icon(column, row):
    return (6 + column * 76 + 38, 6 + row * 74 + 18)


def desktop_cell_box(column, row):
    x = 6 + column * 76
    y = 6 + row * 74
    return (x, y + 30, x + 76, y + 74)


def scenario_desktopicons(s):
    """The icons on the desktop (docs/desktop/DESKTOP_ICONS.md, phases 2-3).

    The first session gets three shortcuts in the Desktop folder. Click
    selects, double click opens, the background takes the keyboard (arrows,
    F2 to rename), dragging moves an icon to another cell, the icon's menu
    deletes it after a question, and the background's menu makes a folder.
    """
    WHITE = (255, 255, 255)
    image = s.shot("desktop")
    for row, name in enumerate(("Command", "Files", "Notepad")):
        expect_text(image, desktop_cell_box(0, row), name, WHITE, FACE,
                    "the default shortcut %s" % name, solid_only=True)

    # Click selects: the label takes the selection colour.
    s.qmp.move_to(*desktop_cell_icon(0, 2))
    s.qmp.click()
    time.sleep(1.0)
    image = s.shot("notepad-selected")
    expect_text(image, desktop_cell_box(0, 2), "Notepad", SELECT_TEXT, SELECT,
                "a click selects the icon")

    # Double click opens: Notepad's window shows up in the middle.
    centre = (640, 380)
    s.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    s.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(0.1)
    s.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    s.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(6.0)
    opened = s.shot("notepad-opened")
    if opened.getpixel(centre) == image.getpixel(centre):
        raise Failure("a double click on Notepad opened no window over %r" % (centre,))
    s.qmp.chord("alt", "f4")
    time.sleep(2.0)

    # The background takes the keyboard: Up moves the selection to Files,
    # F2 renames it in place.
    s.qmp.move_to(600, 500)
    s.qmp.click()
    s.qmp.move_to(*desktop_cell_icon(0, 2))
    s.qmp.click()
    time.sleep(0.8)
    s.qmp.tap("up", pause=0.6)
    image = s.shot("keyboard-up")
    expect_text(image, desktop_cell_box(0, 1), "Files", SELECT_TEXT, SELECT,
                "Up moves the selection")
    s.qmp.tap("f2", pause=0.6)
    for _ in range(5):
        s.qmp.tap("backspace", pause=0.1)
    s.qmp.type_text("browse")
    s.qmp.tap("ret", pause=1.0)
    image = s.shot("renamed")
    expect_text(image, desktop_cell_box(0, 1), "browse", SELECT_TEXT, SELECT,
                "F2 renames the icon")

    # Drag it two columns to the right.
    s.qmp.move_to(*desktop_cell_icon(0, 1))
    s.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(0.3)
    target = desktop_cell_icon(2, 1)
    s.qmp.move_to(target[0], target[1])
    time.sleep(0.5)
    s.qmp.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(1.0)
    s.qmp.move_to(600, 500)
    time.sleep(0.5)
    image = s.shot("dragged")
    expect_text(image, desktop_cell_box(2, 1), "browse", SELECT_TEXT, SELECT,
                "dragging moves the icon to the cell it is dropped on")

    # The icon's menu: Delete asks first; Enter answers Yes.
    s.qmp.move_to(*desktop_cell_icon(2, 1))
    s.qmp.click("right")
    time.sleep(1.0)
    image = s.shot("icon-menu")
    expect_text(image, (160, 80, 460, 300), "Delete", TEXT, FACE, "the icon's menu lists Delete")
    s.qmp.tap("up", pause=0.4)
    s.qmp.tap("ret", pause=1.5)
    image = s.shot("delete-question")
    expect_text(image, (400, 250, 900, 550), "Delete 'browse'?", TEXT, FACE,
                "Delete asks before deleting")
    s.qmp.tap("ret", pause=3.0)
    image = s.shot("deleted")
    if find_text(image, (0, 0, 400, 300), "browse", SELECT_TEXT, SELECT)[0] or \
            find_text(image, (0, 0, 400, 300), "browse", WHITE, FACE, solid_only=True)[0]:
        raise Failure("the icon is still there after deleting it")

    # The background's menu makes a folder and starts its name being edited.
    s.qmp.move_to(600, 500)
    s.qmp.click("right")
    time.sleep(1.0)
    s.qmp.tap("down", pause=0.3)
    s.qmp.tap("down", pause=0.3)
    s.qmp.tap("down", pause=0.3)
    image = s.shot("menu-new-folder")
    expect_text(image, (600, 500, 900, 700), "New Folder", SELECT_TEXT, SELECT,
                "the background's menu offers New Folder")
    s.qmp.tap("ret", pause=2.0)
    s.qmp.tap("ret", pause=1.5)
    image = s.shot("new-folder")
    found = False
    for column in range(3):
        for row in range(4):
            if find_text(image, desktop_cell_box(column, row), "New Folder", SELECT_TEXT, SELECT)[0]:
                found = True
    if not found:
        raise Failure("New Folder made no icon")


def scenario_taskbar(s):
    """Taskbar: real clicks, with pixel VERIFICATION.

    The buttons keep a stable order by slot, so 0 is Calculator (opened
    first) and 1 is Notepad, both through the console, which has closed by
    the time either is captured. The strip is at the bottom,
    window buttons are 160 wide from x=68 (after the 64-wide Start button) and
    the layout indicator sits at the far right.

    Taking screenshots for someone to look at is not enough: the four bugs this
    taskbar had -- two with fds, two with input -- passed every headless
    harness. What is asserted is each button's BEVEL, which encodes which window
    is active, so every check proves the whole chain: click, request to the WM,
    state change, list republication and repaint.
    """
    s.open_calc()
    image = s.shot("calc-only")
    expect_taskbar_present(image, "Calculator only")
    expect_button(image, 0, True, "Calculator only")

    s.open_notepad()
    image = s.shot("dos-ventanas")
    expect_taskbar_present(image, "con dos ventanas")
    # El bloc de notas se acaba de abrir y tiene el foco.
    expect_button(image, 0, False, "con dos ventanas")
    expect_button(image, 1, True, "con dos ventanas")

    # The label of the inactive button is written in these letters, black on
    # the button's plain face. The active one is not checked: it is bold over
    # the dither, and find_text models a single background colour.
    #
    # The window's title is NOT asserted, although it is in the same shot. It
    # sits on a 24-band gradient with an accent blended more on the left than
    # on the right, and predicting it from here means copying the arithmetic of
    # windowd_render.c. Asserting only the glyph pixels with full coverage,
    # which do not depend on the background, leaves too few to notice even a
    # change of font. That is pending work, not coverage that seems to exist.
    bx, by, bw, bh = button_rect(image, 0)
    expect_text(
        image, (bx, by, bx + bw, by + bh), "Calculator", TEXT, FACE,
        "the taskbar label",
    )


    # A click on Calculator's button activates it.
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


    # With an application open, a right click on blank taskbar space must
    # give the menu the keyboard. Escape dismisses it without launching.
    s.qmp.move_to(1000, 786)
    s.qmp.click("right")
    image = s.shot("taskbar-context-menu")
    expect_text(image, (950, 730, 1280, 772), "Task Manager...", TEXT, FACE,
                "the taskbar context menu offers Task Manager")
    s.qmp.tap("esc", pause=1.0)
    image = s.shot("taskbar-context-dismissed")
    if find_text(image, (950, 730, 1280, 772), "Task Manager...", TEXT, FACE)[0] is not None:
        raise Failure("Escape did not dismiss the taskbar context menu")

    # Open it again and choose the row with the mouse, through to a real
    # Task Manager window with its Applications tab.
    s.qmp.click("right")
    s.qmp.move_to(1040, 758)
    s.qmp.click()
    time.sleep(12.0)
    image = s.shot("taskbar-task-manager")
    expect_text(image, (0, 0, image.size[0], 730), "Applications", TEXT, FACE,
                "the taskbar menu launches Task Manager")


def wait_menu_open(s, stem="menu-abierto"):
    """Polling hasta ver cara en el contenido del menu (ver scenario_startmenu).

    El punto esta a la derecha de la franja lateral y sobre la primera
    cabecera, antes de su texto: el menu lo pinta FACE siempre.
    """
    image = None
    for _ in range(15):
        time.sleep(2.0)
        image = s.shot(stem)
        mx, my, mw, mh = startmenu_rect(image)
        if image.getpixel((mx + 2 + STARTMENU_STRIP_WIDTH + 2, my + 4)) == FACE:
            break
    return image


def startmenu_arrow_points(image):
    """Centre of the 7px base row of the up and down arrows.

    The arrow is 4 rows tall, centred in its 12px strip: the up arrow's base is
    its last row (strip top + 7), the down arrow's its first (strip top + 4).
    """
    mx, my, mw, mh = startmenu_rect(image)
    view_x = mx + 2 + STARTMENU_STRIP_WIDTH
    view_width = STARTMENU_WIDTH - 4 - STARTMENU_STRIP_WIDTH
    centre_x = view_x + (view_width - 7) // 2 + 3
    up_strip_y = my + 2
    down_strip_y = my + 2 + STARTMENU_VIEW_HEIGHT - STARTMENU_SCROLL_STRIP_HEIGHT
    return (centre_x, up_strip_y + 7), (centre_x, down_strip_y + 4)


def check_startmenu_arrows(s, opened):
    """The arrow strips of a Start menu whose catalog does not fit.

    Freshly opened, the list is at the top: the up arrow is etched grey and the
    down arrow black. Resting the pointer on the down strip scrolls on its own
    (no further pointer events arrive) until the end, where the two swap. A
    catalog that fits has no strips, and then there is nothing to check.
    """
    up, down = startmenu_arrow_points(opened)
    if opened.getpixel(up) == FACE and opened.getpixel(down) == FACE:
        print("startmenu: the catalog fits, no arrow strips to check")
        return
    expect_pixel(opened, up[0], up[1], SHADOW, "menu just opened: the up arrow is greyed")
    expect_pixel(opened, down[0], down[1], TEXT, "menu just opened: the down arrow is live")
    # On the strip's left end, so the cursor does not cover the arrow.
    s.qmp.move_to(down[0] - 90, down[1] + 2)
    image = None
    for _ in range(10):
        time.sleep(1.0)
        image = s.shot("menu-flecha-abajo")
        if image.getpixel(down) == SHADOW:
            break
    expect_pixel(image, down[0], down[1], SHADOW, "rested on the down arrow: the list reached its end")
    expect_pixel(image, up[0], up[1], TEXT, "rested on the down arrow: the up arrow is live")


def scenario_startmenu(s):
    """Menu Inicio recortado: boton Start, lista plana y toggle.

    Se abre con click en Start, se verifica un pixel de cara del marco (el
    menu pinta FACE en todo su rect) y se cierra con otro click en Start.
    Sin aserciones de contenido todavia: el catalogo depende de lo instalado
    (Doom, FFmpeg) y las filas no caen en coordenadas fijas -- igual que
    gears, hoy esto es para MIRAR.
    """
    s.shot("sin-menu")
    # El tamano del display sale de una captura cualquiera; el boton Start
    # esta abajo a la izquierda por construccion (taskbar.c).
    probe = s.shot("medida")
    x, y = start_button_center(probe)
    s.qmp.move_to(x, y)
    s.qmp.click()
    # El menu escanea el catalogo antes del primer frame (igual que progman
    # al arrancar); bajo TCG eso tarda. Polling hasta ver su marco en cara,
    # en vez de un sleep fijo que seria lento siempre o corto a veces.
    image = wait_menu_open(s)
    mx, my, mw, mh = startmenu_rect(image)
    expect_pixel(image, mx + 2 + STARTMENU_STRIP_WIDTH + 2, my + 4, FACE, "menu abierto: contenido en cara")
    # First program row (Calculator, the first Accessories item), below the
    # 20px header from the 2px margin. A list that overflows puts a 12px arrow
    # strip above it, so the row is [22,50) or [34,62): y 40 is in both.
    s.qmp.move_to(mx + 100, my + 40)
    s.qmp.click()
    time.sleep(3.0)
    launched = s.shot("menu-lanzo")
    mlx, mly, mlw, mlh = startmenu_rect(launched)
    if launched.getpixel((mlx + 2 + STARTMENU_STRIP_WIDTH + 2, mly + 4)) == FACE:
        raise Failure("el menu sigue abierto tras clickear Calculator: el launch no salio")
    # La app tarda en presentar bajo TCG; la captura es para mirar, no afirma.
    time.sleep(9.0)
    s.shot("calc-desde-menu")
    # Reabrir tras un launch prueba que el menu relanza limpio despues de
    # haber salido (el toggle con el menu vivo lo cierra, como arriba).
    x, y = start_button_center(launched)
    s.qmp.move_to(x, y)
    s.qmp.click()
    reopened = wait_menu_open(s, "menu-reabierto")
    rx, ry, rw, rh = startmenu_rect(reopened)
    expect_pixel(reopened, rx + 2 + STARTMENU_STRIP_WIDTH + 2, ry + 4, FACE, "menu reabierto: contenido en cara")
    check_startmenu_arrows(s, reopened)
    # Two-step confirmation: Shut Down arms the action ("Yes, shut down"),
    # No cancels it. The real Yes powers off the VM and is not clicked.
    footer_y = ry + STARTMENU_MARGIN + STARTMENU_VIEW_HEIGHT + 2 + STARTMENU_FOOTER_GAP
    # Leave room for ascenders and descenders (the 'y' in "Yes, shut down").
    # find_text requires every pattern pixel to fit inside the region.
    shutdown_box = (rx + 40, footer_y + 2, rx + 230, footer_y + STARTMENU_ROW_HEIGHT)
    s.qmp.move_to(rx + 129, footer_y + STARTMENU_ROW_HEIGHT // 2)
    s.qmp.click()
    time.sleep(1.0)
    # Keep the pointer at the row's right edge: selection stays active without
    # covering glyphs whose pixels the assertion needs.
    s.qmp.move_to(rx + 210, footer_y + STARTMENU_ROW_HEIGHT // 2)
    time.sleep(0.5)
    armed = s.shot("menu-armado")
    # The pointer selects the row (white on blue). solid_only checks the glyph
    # core; an antialiased edge cannot be predicted with one colour.
    expect_text(armed, shutdown_box, "Yes, shut down", SELECT_TEXT, SELECT, "el pie pide confirmacion",
                solid_only=True)
    s.qmp.move_to(rx + 129, footer_y + STARTMENU_ROW_HEIGHT + STARTMENU_ROW_HEIGHT // 2)
    s.qmp.click()
    time.sleep(1.0)
    s.qmp.move_to(rx + 210, footer_y + STARTMENU_ROW_HEIGHT + STARTMENU_ROW_HEIGHT // 2)
    time.sleep(0.5)
    disarmed = s.shot("menu-desarmado")
    expect_text(disarmed, shutdown_box, "Shut Down", TEXT, FACE, "el No desarma el pie", solid_only=True)
    # Y el toggle lo vuelve a cerrar.
    s.qmp.move_to(*start_button_center(disarmed))
    s.qmp.click()
    time.sleep(2.0)
    closed = s.shot("menu-cerrado")
    # Ctrl+Esc is a hotkey the desktop registers: it opens the menu with the
    # keyboard, whichever window has the focus, and closes it again. Closed is
    # judged against the shot before opening, since what lies under the menu
    # (the calculator launched above, the wallpaper) is not a fixed colour.
    kx, ky, kw, kh = startmenu_rect(closed)
    probe = (kx + 2 + STARTMENU_STRIP_WIDTH + 2, ky + 4)
    s.qmp.chord("ctrl", "esc")
    by_key = wait_menu_open(s, "menu-ctrl-esc")
    expect_pixel(by_key, probe[0], probe[1], FACE, "Ctrl+Esc opened the menu")
    s.qmp.chord("ctrl", "esc")
    time.sleep(2.0)
    reclosed = s.shot("menu-ctrl-esc-cerrado")
    expect_pixel(reclosed, probe[0], probe[1], closed.getpixel(probe), "Ctrl+Esc again closed the menu")
    # Ctrl+Shift+Esc opens Task Manager (for looking: its window has no fixed
    # place to assert on).
    s.qmp.key("ctrl", True)
    s.qmp.key("shift", True)
    time.sleep(0.15)
    s.qmp.tap("esc")
    time.sleep(0.15)
    s.qmp.key("shift", False)
    s.qmp.key("ctrl", False)
    time.sleep(12.0)
    s.shot("taskmgr-ctrl-shift-esc")


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
    s.run("gfxdemo")
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
    s.run("gfxdemo")
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
    s.run("widgetsdemo")
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


# Recorta `box` dejando un hueco del color del campo donde el compositor pinta el
# cursor, para comparar el CONTENIDO y no el puntero que esta encima. El cursor
# no se mueve entre las dos capturas, asi que el hueco cae en el mismo sitio en
# ambas y solo come pixeles suyos. Para el por que, ver scenario_notepadwheel.
def crop_without_cursor(image, box, cursor_x, cursor_y, size=40):
    cropped = image.crop(box)
    cropped.paste(
        FIELD,
        (cursor_x - box[0] - size // 2, cursor_y - box[1] - size // 2, size, size),
    )
    return cropped.tobytes()


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

    # Lo que se leyo hasta aqui era "algo se movio". Esto es "esta linea esta
    # escrita con estas letras, en negro, sobre blanco": el glifo de cada caracter
    # sale de la tabla que trae libsxgfx y se compara pixel a pixel contra la
    # pantalla.
    #
    # Sin esto, pintar el texto con SXGUI_COLOR_FACE -- invisible sobre el campo
    # blanco -- pasaba: el scroll se comprueba con el pulgar de la barra, y el
    # pulgar se movia igual. Un toolkit que dibuja las lineas en el color del
    # fondo no se nota en ningun otro chequeo de este archivo.
    #
    # Dos lineas y no la ultima: el caret queda al final de "linea40" y pisa el
    # ultimo pixel del cero, que es un gris de cobertura 2 y el caret lo tapa con
    # negro. Ese pixel es real y esta ahi, pero afirmar sobre el obliga a
    # modelar el caret, y un modelo del caret aqui no compra nada.
    #
    # Dos lineas consecutivas en vez de una: ademas del glifo y del color, esto
    # afirma el interlineado, que tiene que ser la fila del toolkit. Un solo
    # acierto podria ser suerte de la posicion; dos seguidas, no.
    row_height = fonts()["body"].line_height + SXGUI_ROW_LEADING
    penultimate = expect_text(image, area, "linea39", TEXT, FIELD, "el editor de notepad")
    third_last = expect_text(image, area, "linea38", TEXT, FIELD, "el editor de notepad")
    if penultimate[0] != third_last[0]:
        raise Failure(
            "las lineas del editor no arrancan en la misma x: %d y %d" % (third_last[0], penultimate[0])
        )
    step = penultimate[1] - third_last[1]
    if step != row_height:
        raise Failure(
            "el interlineado del editor es %d y la fila del toolkit es %d: las lineas no "
            "se pintan donde dicen las metrics de libsxgfx" % (step, row_height)
        )
    print("  'linea38'/'linea39' en x=%d, interlineado %d" % (penultimate[0], step))

    # Tipear deja el caret al final: el editor ya esta scrolleado al fondo, asi
    # que la rueda hay que probarla subiendo primero -- bajar ya esta clampeado.
    #
    # Las dos aserciones de la rueda afirman TEXTO, no bytes del recorte. El
    # recorte incluye el cursor que el compositor dibuja encima, y ese glifo
    # depende de lo que hay debajo (flecha sobre el campo, I-beam sobre el area
    # de texto): dos capturas con el editor exactamente igual dan bytes
    # distintos y se leian como "la rueda no volvio al fondo". Decir que linea26
    # esta escrita afirma lo mismo -- el editor esta en su ultima pagina -- sin
    # depender de lo que se dibuja encima. Y el fallo dice que linea falta, que
    # es justo lo que distingue ticks perdidos de ticks duplicados.
    #
    # linea26 es la primera de la ultima pagina, y linea20 la primera tras dos
    # muescas: el editor avanza tres lineas por muesca.
    before = s.shot("notepad-abajo")
    expect_text(before, area, "linea26", TEXT, FIELD, "el editor de notepad al fondo")
    s.qmp.wheel(2)                    # dos muescas arriba
    scrolled = s.shot("notepad-rueda-arriba")
    expect_text(
        scrolled, area, "linea20", TEXT, FIELD,
        "el editor de notepad tras dos muescas arriba",
    )

    s.qmp.wheel(-20)                  # de vuelta al fondo, de sobra
    back = s.shot("notepad-rueda-vuelta")
    expect_text(
        back, area, "linea26", TEXT, FIELD,
        "el editor de notepad de vuelta al fondo",
    )

    # Flecha de ARRIBA de la barra incrustada: abajo ya esta clampeado por el
    # mismo motivo que la rueda. find_list_area mide el panel blanco y se pasa
    # un par de pixeles hacia la barra (el borde entre los dos no es un blanco
    # limpio), asi que el punto de click va hacia ADENTRO del area detectada y
    # no area[2] + margen -- eso ultimo cae en el borde de la ventana.
    sb_x = area[2] - 8
    sb_y = area[1] + 6
    s.qmp.move_to(sb_x, sb_y)
    before_click = crop_without_cursor(s.shot("notepad-antes-del-click"), area, sb_x, sb_y)
    s.qmp.click()
    s.qmp.click()
    s.qmp.click()
    clicked = crop_without_cursor(s.shot("notepad-scrollbar-click"), area, sb_x, sb_y)
    if clicked == before_click:
        raise Failure("clickear la barra de scroll no movio el editor")


def scenario_webview(s):
    """Web Viewer v0.1: it starts, opens a local file and paints it.

    Without arguments it shows its welcome screen (synthetic paragraphs, no
    parser); F3 opens the dialog, /welcome.html is typed, and what appears was
    produced by the real parser: the h1 in the title font and a list item in
    the body's, asserted pixel by pixel against libsxgfx.

    The slash is typed as Shift+7: the guest's active layout is ES and the
    qcode is the physical key, so "slash" would type a dash (see type_text).
    """
    s.open_webview()
    welcome = s.shot("webview-bienvenida")
    width, height = welcome.size
    expect_text(welcome, (0, 0, width, height), "Web Viewer", TEXT, FIELD,
                "la bienvenida de webviewer", which="title")
    s.qmp.tap("f3", pause=2.0)
    s.qmp.chord("shift", "7")
    s.qmp.type_text("welcome")
    s.qmp.tap("dot", pause=0.3)
    s.qmp.type_text("html")
    s.qmp.tap("ret", pause=4.0)
    page = s.shot("webview-pagina")
    width, height = page.size
    expect_text(page, (0, 0, width, height), "Welcome to SavanXP", TEXT, FIELD,
                "el h1 de welcome.html", which="title")
    # solid_only: donde los bordes antialiaseados de dos glifos se solapan, el
    # pintor mezcla en secuencia sobre el framebuffer y el modelo del harness
    # contra el fondo (ver gfx_pixel_blend): esos pixeles difieren en un poco
    # y no distinguen un fallo. El nucleo identifica letra y posicion igual.
    expect_text(page, (0, 0, width, height), "Lists like this one", TEXT, FIELD,
                "un item de lista de welcome.html", solid_only=True)
    # v0.2: la barra de direcciones lleva a una segunda pagina y Back/Forward
    # pasean por el historial. El foco quedo en la barra al cerrar el dialogo,
    # asi que Ctrl+A la selecciona entera antes de tipear la otra ruta.
    s.qmp.chord("ctrl", "a")
    s.qmp.chord("shift", "7")
    s.qmp.type_text("about")
    s.qmp.tap("dot", pause=0.3)
    s.qmp.type_text("html")
    s.qmp.tap("ret", pause=4.0)
    second = s.shot("webview-segunda")
    width, height = second.size
    expect_text(second, (0, 0, width, height), "Going further", TEXT, FIELD,
                "el h1 de about.html", which="title")
    s.qmp.chord("alt", "left")
    time.sleep(3.0)
    back = s.shot("webview-atras")
    width, height = back.size
    expect_text(back, (0, 0, width, height), "Welcome to SavanXP", TEXT, FIELD,
                "volver atras a welcome.html", which="title")
    s.qmp.chord("alt", "right")
    time.sleep(3.0)
    fwd = s.shot("webview-adelante")
    width, height = fwd.size
    expect_text(fwd, (0, 0, width, height), "Going further", TEXT, FIELD,
                "volver adelante a about.html", which="title")
    scenario_webview_http(s)


def scenario_webview_http(s):
    """Web Viewer v0.3: fetch HTTP, links, source y redirect.

    Levanta el fixture HTTP del host (el guest lo ve en 10.0.2.2 por slirp),
    tipea la URL en la barra, sigue un link con el mouse, mira el fuente con
    Ctrl+U y atraviesa un 302. Cada paso afirma glifos contra libsxgfx.
    """
    root = Path(__file__).resolve().parent
    server = subprocess.Popen(
        [sys.executable, str(root / "webview_http_server.py"),
         "--dir", str(root / "webview-test-www")],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    dns = subprocess.Popen(
        [sys.executable, str(root / "webview_dns_server.py")],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        ready = server.stdout.readline().split()
        if len(ready) != 2 or ready[0] != "READY":
            raise Failure("el fixture HTTP no anuncio su puerto: %r" % ready)
        dready = dns.stdout.readline().split()
        if len(dready) != 2 or dready[0] != "READY":
            raise Failure("el fixture DNS no anuncio su puerto: %r" % dready)
        hostport = "10.0.2.2:%s" % ready[1]
        dnsport = dready[1]
        s.qmp.tap("tab", pause=1.0)
        s.qmp.chord("ctrl", "a")
        type_http_url(s, hostport, "/index.html")
        s.qmp.tap("ret", pause=6.0)
        net = s.shot("webview-http")
        width, height = net.size
        expect_text(net, (0, 0, width, height), "It works over HTTP", TEXT, FIELD,
                    "el h1 traido por HTTP", which="title", solid_only=True)
        # El link: su origen lo da la misma busqueda de glifos, en azul.
        link_at = expect_text(net, (0, 0, width, height), "second page", LINK, FIELD,
                              "el link en azul", solid_only=True)
        s.qmp.move_to(link_at[0] + 30, link_at[1] + 8)
        s.qmp.click()
        time.sleep(5.0)
        second = s.shot("webview-link")
        width, height = second.size
        expect_text(second, (0, 0, width, height), "Second page", TEXT, FIELD,
                    "el h1 tras seguir el link", which="title", solid_only=True)
        # Fuente del documento en red, y vuelta.
        s.qmp.chord("ctrl", "u")
        time.sleep(2.0)
        source = s.shot("webview-fuente")
        width, height = source.size
        expect_text(source, (0, 0, width, height), "Second page", TEXT, FIELD,
                    "el titulo en el fuente", solid_only=True)
        s.qmp.chord("ctrl", "u")
        time.sleep(2.0)
        rendered = s.shot("webview-otra-vez")
        width, height = rendered.size
        expect_text(rendered, (0, 0, width, height), "Second page", TEXT, FIELD,
                    "el h1 al salir del fuente", which="title", solid_only=True)
        # Redirect 302 a /index.html: cae en el destino y el historial guarda
        # la URL final (Back vuelve a second, no a old).
        s.qmp.tap("tab", pause=1.0)
        s.qmp.chord("ctrl", "a")
        type_http_url(s, hostport, "/old.html")
        s.qmp.tap("ret", pause=6.0)
        redir = s.shot("webview-redirect")
        width, height = redir.size
        expect_text(redir, (0, 0, width, height), "It works over HTTP", TEXT, FIELD,
                    "el destino del redirect", which="title", solid_only=True)
        s.qmp.chord("alt", "left")
        time.sleep(3.0)
        back = s.shot("webview-redirect-atras")
        width, height = back.size
        expect_text(back, (0, 0, width, height), "Second page", TEXT, FIELD,
                    "Back tras el redirect", which="title", solid_only=True)
        # v0.4: una pagina con CSS. El h1 rojo y centrado prueba regla por tag
        # (y de paso el fondo crema del body: el patron exige ese fondo); el
        # verde alineado a la derecha prueba style=""; el resto, runs inline.
        # El bold de doble golpe solo se mira: no hay patron de glifo para el.
        s.qmp.tap("tab", pause=1.0)
        s.qmp.chord("ctrl", "a")
        type_http_url(s, hostport, "/style.html")
        s.qmp.tap("ret", pause=6.0)
        styled = s.shot("webview-css")
        width, height = styled.size
        CREAM = (255, 255, 204)
        expect_text(styled, (0, 0, width, height), "Styled page", (204, 0, 0), CREAM,
                    "el h1 con regla de tag", which="title", solid_only=True)
        expect_text(styled, (0, 0, width, height), "This paragraph is plain.",
                    (51, 51, 51), CREAM, "el color del body", solid_only=True)
        expect_text(styled, (0, 0, width, height), "Right and green", (0, 128, 0), CREAM,
                    "el style inline", solid_only=True)
        expect_text(styled, (0, 0, width, height), "font color", (128, 0, 128), CREAM,
                    "el color de font", solid_only=True)
        expect_text(styled, (0, 0, width, height), "underline", (51, 51, 51), CREAM,
                    "el subrayado propio", solid_only=True)
        # DNS: el resolver apunta al fixture con una URL dns:// (override
        # tipeable en la instancia ya abierta) y luego la pagina por nombre.
        # No por shell: lanzar con argumentos (ni pelado ni absoluto) abre
        # ventana alguna y deja un "webview" rojo en stderr — pendiente del
        # milestone del shell, no de este. Cada Enter saca el foco de la
        # barra, asi que cada tipeo arranca con tab (de -1 va al campo).
        def type_dns_url(url):
            s.qmp.type_text("dns")
            s.qmp.chord("shift", "dot")
            s.qmp.chord("shift", "7")
            s.qmp.chord("shift", "7")
            for ch in url:
                if ch == ".":
                    s.qmp.tap("dot", pause=0.2)
                elif ch == ":":
                    s.qmp.chord("shift", "dot")
                elif ch == "/":
                    s.qmp.chord("shift", "7")
                else:
                    s.qmp.tap(ch, pause=0.12)
        s.qmp.tap("tab", pause=1.0)
        s.qmp.chord("ctrl", "a")
        type_dns_url("10.0.2.2:" + dnsport)
        s.qmp.tap("ret", pause=3.0)
        resolver = s.shot("webview-resolver")
        width, height = resolver.size
        expect_text(resolver, (0, 0, width, height), "Resolver:", TEXT, FACE,
                    "el override del resolver")
        s.qmp.tap("tab", pause=1.0)
        s.qmp.chord("ctrl", "a")
        type_http_url(s, "test.savanxp:" + hostport.split(":")[1], "/index.html")
        s.qmp.tap("ret", pause=6.0)
        named = s.shot("webview-nombre")
        width, height = named.size
        expect_text(named, (0, 0, width, height), "It works over HTTP", TEXT, FIELD,
                    "el h1 por nombre DNS", which="title", solid_only=True)
        # NXDOMAIN: el fixture responde 3 a lo desconocido y la barra lo dice.
        s.qmp.tap("tab", pause=1.0)
        s.qmp.chord("ctrl", "a")
        type_http_url(s, "nosuch.savanxp", "/")
        s.qmp.tap("ret", pause=5.0)
        nxdomain = s.shot("webview-nxdomain")
        width, height = nxdomain.size
        expect_text(nxdomain, (0, 0, width, height), "No such host", TEXT, FACE,
                    "el aviso de NXDOMAIN", solid_only=True)
    finally:
        server.terminate()
        server.wait()
        dns.terminate()
        dns.wait()


def type_http_url(s, hostport, path):
    """Tipea http://hostport/path con el layout ES del guest.

    Los simbolos van por su combinacion en el layout activo (ver type_text):
    "/" es Shift+7 y ":" es Shift+punto.
    """
    s.qmp.type_text("http")
    s.qmp.chord("shift", "dot")
    s.qmp.chord("shift", "7")
    s.qmp.chord("shift", "7")
    for ch in hostport + path:
        if ch == ".":
            s.qmp.tap("dot", pause=0.2)
        elif ch == ":":
            s.qmp.chord("shift", "dot")
        elif ch == "/":
            s.qmp.chord("shift", "7")
        else:
            s.qmp.tap(ch, pause=0.12)


def scenario_gears(s):
    """Captura la demo de engranajes: el consumidor del batch 0 de SxGL.

    No asierta pixeles todavia -- lo que hace falta de este escenario hoy es
    MIRAR el resultado, que es la unica forma de ver si el winding, el backface
    culling y el depth test estan del lado correcto. Un rasterizador con el
    culling invertido no falla: dibuja el interior de la pieza, y eso solo se
    nota a ojo.

    Tres capturas: girando, pausada y rotada, porque las flechas mueven la
    camara y ahi es donde se ve si la silueta se arma bien desde otro angulo.
    """
    # Its .sxres shows on the window it opens: the caption and the taskbar
    # button carry the icon and the name the manifest declares.
    s.run("gears")
    time.sleep(3.0)
    s.shot("gears-girando")
    s.qmp.tap("s")                   # pausa: la captura queda estable
    time.sleep(1.0)
    s.shot("gears-pausada")
    for _ in range(6):
        s.qmp.tap("right", pause=0.3)
    time.sleep(1.5)
    s.shot("gears-rotada")


def scenario_nightwood(s):
    """Nightwood, the SUN game on SxGL: the title, a few steps into the forest,
    and the still pose with the Warden in view.

    Like gears this is for looking: winding, the flashlight's beam, the fog
    and the glow of the eyes are judged by eye. The logic and a rendered frame
    are asserted by nightwood-smoke.
    """
    s.run("nightwood", wait=15)
    time.sleep(3.0)
    s.shot("nightwood-title")
    s.qmp.tap("ret")
    time.sleep(2.0)
    s.qmp.key("w", True)
    time.sleep(2.5)
    s.qmp.key("w", False)
    s.qmp.key("left", True)
    time.sleep(1.0)
    s.qmp.key("left", False)
    time.sleep(1.0)
    s.shot("nightwood-forest")
    # Mouse look: the pointer is captured while playing, so relative motion
    # turns the head and the cursor stays hidden.
    if s.qmp.abs_pointer:
        s.qmp.move_to(400, 400)
        s.qmp.move_to(700, 400)
    else:
        for _ in range(30):
            s.qmp._rel_step(10, 0)
    time.sleep(1.0)
    s.shot("nightwood-mouselook")
    s.qmp.tap("esc")
    time.sleep(1.0)
    s.shot("nightwood-paused")
    s.qmp.tap("esc")
    time.sleep(0.5)
    s.qmp.tap("esc")
    time.sleep(2.0)
    s.run("nightwood --still", wait=15)
    time.sleep(3.0)
    s.shot("nightwood-warden")


def scenario_spin(s):
    """Carga que SI satura el pipeline: el cliente dibuja sin esperar la entrada.

    bench y saturate resultaron limitados por la entrada -- un frame por tecla
    o por comando QMP -- y con windowd ocioso la mayor parte de cada frame no
    distinguen un pipeline rapido de uno lento. Aca Gfx Demo entra en su modo
    automatico (tecla S): la caja avanza en cada vuelta y gfx_present_region
    bloquea hasta que el compositor consumio el frame anterior, asi que los
    fps son el techo del camino de display y nada mas.
    """
    s.run("gfxdemo")
    s.qmp.tap("s")                   # modo automatico: encendido
    time.sleep(20.0)
    s.qmp.tap("s")                   # apagado: la sesion vuelve a quedar quieta
    time.sleep(4.0)                  # la ultima linea de stats sale cada 2 s
    s.shot("spin-final")


def scenario_ccleste(s):
    """Celeste Classic: el port completo sobre el escritorio de verdad.

    Necesita el port y sus assets instalados. Se abre por el launcher, que es
    el unico camino que da una superficie de cliente al compositor, y con
    launch_flags=fullscreen la ventana sale al tamano de la presentacion.

    Lo que hay que mirar: la pantalla de titulo dibujada con la hoja de tiles y
    la fuente del port, escalada por entero y centrada, y la paleta PICO-8 de 16
    colores. Despues se entra a la habitacion para comprobar que el mapa, las
    colisiones y el fondo ya estan.
    """
    s.open_ccleste()
    s.shot("ccleste-titulo")
    # La pantalla de titulo se descarta con jump o dash (celeste.c, is_title),
    # no con Enter: el mismo prompt "x+c" que dibuja arriba es el que dice que
    # tecla apretar. Z es jump.
    s.qmp.tap("z", pause=3.0)
    s.shot("ccleste-habitacion")
    # Un salto a la derecha pega contra la pared de la habitacion, que es donde
    # el dash tiene que dejar al personaje pegado sin atravesarla.
    s.qmp.tap("right", pause=1.0)
    s.qmp.tap("x", pause=1.0)
    s.shot("ccleste-dash")


def scenario_mediaplayer_unavailable(s):
    """El reproductor sin su motor: tiene que abrir una ventana y explicar.

    Se lanza desde el lanzador, no ejecutandolo a mano, porque la mitad de lo que se
    comprueba aqui es que la entrada exista y se pueda abrir: la posicion que usa
    open_mediaplayer() es una afirmacion sobre el catalogo, y este escenario es lo
    que la hace verdad.

    Antes quitaba el binario del port y el aviso lo ponia el lanzador. Ya no hay
    lanzador --el reproductor es un programa del arbol-- y lo que falta es la
    LIBRERIA, asi que el aviso lo pone el propio reproductor desde run_window. Es el
    mismo texto en el mismo sitio, y el escenario que lo cubre ahora es el que
    verifica que el reproductor sobreviva a su motor faltante.

    Se afirma el TEXTO de la ventana y no el rotulo de la barra de tareas. El rotulo
    son 14 pixeles con la fuente actual y no detecta ni un cambio de fuente (esta
    medido y escrito en scenario_taskbar); el aviso son dos lineas, y ademas es lo
    unico que el reproductor pinta cuando no hay motor.
    """
    s.open_mediaplayer()
    image = s.shot("mediaplayer-sin-motor")
    area = (0, 0, image.width, image.height)
    # El aviso se pinta en ROJO sobre el NEGRO del area de video, no en el color de
    # texto del toolkit: es g.message, y mediaplayer.c lo dibuja con
    # gfx_rgb(230, 120, 110) porque es un error (la misma pinta "Cannot open ...").
    # Con TEXT/FIELD --que es lo que-pinaban los textos-- la busqueda da 0 de 335.
    WARNING = (230, 120, 110)
    expect_text(
        image, area, "libffmpeg", WARNING, (0, 0, 0),
        "el aviso nombra la libreria que falta",
    )
    expect_text(
        image, area, "reproduccion", WARNING, (0, 0, 0),
        "el aviso dice que no hay motor de reproduccion",
    )


def _wav_left(path):
    """Canal izquierdo del PCM crudo que graba QEMU (`-audiodev wav`).

    QEMU escribe el header al abrir y lo finaliza al cerrar, asi que cuando el
    runner todavia no mato la VM el archivo no esta cerrado y `wave` lo rechaza:
    se busca el chunk 'data' a mano en vez de confiar en el tamano declarado.
    Devuelve None si el archivo no esta.
    """
    import array
    try:
        blob = Path(path).read_bytes()
    except OSError:
        return None
    marker = blob.find(b"data")
    pcm = blob[marker + 8:] if marker >= 0 else blob[44:]
    samples = array.array("h")
    samples.frombytes(pcm[: len(pcm) // 2 * 2])
    return samples[0::2]


def _wav_signal(path, seconds=8.0, rate=48000, window_s=0.25):
    """Mayor RMS por ventana en la cola del WAV que graba QEMU."""
    left = _wav_left(path)
    if left is None:
        return None
    span = int(rate * seconds)
    tail = left[-span:] if len(left) > span else left
    window = max(1, int(rate * window_s))
    best = 0.0
    for start in range(0, max(0, len(tail) - window), window):
        seg = tail[start:start + window]
        if not seg:
            continue
        mean = sum((x / 32768.0) ** 2 for x in seg) / len(seg)
        best = max(best, mean ** 0.5)
    return best


def _wav_residue_s(path, seconds=6.0, rate=48000, window_s=0.02, min_crossings=8):
    """Cuanto sigue sonando DESPUES de la ultima musica real, en segundos.

    Un cliente vivo manda musica que se mueve: sus ventanas cruzan por cero. Lo
    que deja un cliente que se fue no: el demonio repite el ultimo frame (PLC),
    o sea una meseta constante, y despues silencio exacto. El residuo es la
    distancia entre la ultima ventana con movimiento y el ultimo sample no nulo
    -- exactamente lo que el demonio sigue inventando para un stream muerto, que
    no es musica de nadie. Devuelve None si no hay WAV.
    """
    left = _wav_left(path)
    if left is None:
        return None
    span = int(rate * seconds)
    tail = left[-span:] if len(left) > span else left
    window = max(1, int(rate * window_s))
    last_active = 0
    for start in range(0, len(tail), window):
        seg = tail[start:start + window]
        if not seg:
            continue
        crossings = 0
        previous = seg[0]
        for value in seg[1:]:
            if (value < 0) != (previous < 0):
                crossings += 1
            previous = value
        if crossings >= min_crossings:
            last_active = start + len(seg)
    last_nonzero = 0
    for index in range(len(tail) - 1, -1, -1):
        if tail[index] != 0:
            last_nonzero = index + 1
            break
    return (last_nonzero - last_active) / float(rate)


def scenario_doom(s):
    """DoomGeneric sobre el escritorio real: entra al juego, suena y se cierra.

    Necesita el port y su WAD instalados. Se abre por el launcher, que es el
    unico camino que da una superficie de cliente al compositor. Lo que se
    afirma es la MUSICA, y no se puede ver en una captura: el nivel 1 arranca
    D_E1M1, que el port renderiza con libsxmidi y suena en loop. La evidencia es
    el WAV que graba QEMU, asi que un port que dibuja pero no suena falla aca.

    Y despues se cierra la ventana con Alt+F4, que es el camino del usuario: el
    WM le manda el pedido de cierre y mata al cliente con SIGKILL, asi que no hay
    apagado ordenado de nadie y lo unico que puede acortar la cola es el demonio
    de audio. El WAV tiene que quedar en silencio apenas se va la ultima musica;
    lo que suene despues de eso es audio que ya nadie pidio.
    """
    s.open_doom()
    s.shot("doom-titulo")
    # Enter abre el menu; otro confirma New Game; para Doom 1 quedan el episodio
    # y la dificultad. Cuatro Enter dejan el nivel cargando con su musica.
    for _ in range(4):
        s.qmp.tap("ret", pause=1.5)
    time.sleep(20)
    s.shot("doom-nivel")

    wav = Path(s.out).resolve().parent.parent / "doom-music.wav"
    level = _wav_signal(wav)
    if level is None:
        raise Failure(
            "no quedo el WAV de QEMU en %s: sin el no hay forma de afirmar que "
            "la musica suena" % wav
        )
    if level < 0.01:
        raise Failure(
            "the port is silent: the highest RMS of the last 8 s is %.4f; the "
            "WAV exists and is silent. This scenario needs the DoomGeneric "
            "port installed and a WAD in /games/doom." % level
        )
    print("  musica: RMS %.4f en la cola de %s" % (level, wav.name))

    s.qmp.chord("alt", "f4")
    time.sleep(4.0)
    s.shot("doom-cerrado")

    residue = _wav_residue_s(wav)
    if residue is None:
        raise Failure("el WAV %s desaparecio entre la musica y el cierre" % wav)
    print("  cierre: %.3f s de audio despues de la ultima musica" % residue)
    if residue > 0.15:
        raise Failure(
            "el audio sigue %.3f s despues de cerrar la ventana (tope 0.15 s): "
            "el demonio arrastra el stream de un cliente que ya no existe." % residue
        )


def find_caption(image, text):
    """Origin of `text` drawn in TEXT on FACE, matching the background too.

    The body font is one-bit, so find_text alone also matches any solid black
    area -- a game's HUD behind a dialog. This compares every pixel of the
    text's box, the face colour between the strokes included.
    """
    pattern = glyphs.render(fonts()["body"],0,0,text,TEXT,FACE,(0,0,1 << 20,1 << 20))
    right = max(dx for dx,_ in pattern)+1
    bottom = max(dy for _,dy in pattern)+1
    cells = [((dx,dy),pattern.get((dx,dy),FACE)) for dy in range(bottom) for dx in range(right)]
    # Check the background cells first: in a black area they fail at once.
    cells.sort(key=lambda cell: cell[1] != FACE)
    first = min(pattern, key=lambda k: (k[1],k[0]))
    width,height = image.size
    pixels = image.load()
    for y in range(height-bottom):
        for x in range(width-right):
            if pixels[x,y] != TEXT:
                continue
            ox,oy = x-first[0],y-first[1]
            if ox < 0 or oy < 0:
                continue
            if all(pixels[ox+dx,oy+dy] == colour for (dx,dy),colour in cells):
                return ox,oy
    return None


def open_sound_sheet(s, name, launch=None):
    """Open the Sound property sheet and return its content origin.

    The "Device volume" caption on the Volume page is drawn at content
    (24,87); every other coordinate in the Sound scenarios is relative to it.
    `launch` opens the sheet some other way than the console.
    """
    if launch is None:
        s.run("sound.sxe")
    else:
        launch()
    s.qmp.move_to(1000,120)
    for _ in range(15):
        time.sleep(1.0)
        image = s.shot(name)
        anchor = find_caption(image,"Device volume")
        if anchor is not None:
            return anchor[0]-24,anchor[1]-87
    raise Failure("Sound did not open (%s)" % name)


def sound_tab(s, x, y, label, rel_from=None):
    """Click a tab of the Sound sheet. The labels sit in content row y8..31.

    With `rel_from` the pointer walks there in small relative steps from that
    known position, which is fast enough to use while a 3 s test is playing.
    """
    image = s.shot("sound-tabs-"+label.lower())
    origin = find_text(image,(x+8,y+8,x+432,y+32),label,TEXT,FACE)[0]
    if origin is None:
        raise Failure("Sound has no %s tab" % label)
    target = (origin[0]+8,origin[1]+5)
    if rel_from is None or s.qmp.abs_pointer:
        s.qmp.move_to(*target)
    else:
        dx,dy = target[0]-rel_from[0],target[1]-rel_from[1]
        while dx or dy:
            sx = max(-60,min(60,dx)); sy = max(-60,min(60,dy))
            s.qmp._rel_step(sx,sy); dx -= sx; dy -= sy
    s.qmp.click()
    return target


def sound_app_row(image, x, y, name):
    """Origin of an application row in the Applications page list, or None."""
    for foreground,background in ((TEXT,FIELD),(SELECT_TEXT,SELECT)):
        origin = find_text(image,(x+20,y+63,x+420,y+213),name,foreground,background)[0]
        # White glyph pixels alone can also match an empty white field.
        # A selected row must have the blue selection background as well.
        if origin is not None and (foreground != SELECT_TEXT or
                image.getpixel((origin[0]-2,origin[1]+3)) == SELECT):
            return origin
    return None


def scenario_audio_ports_offline(s):
    """Exercise the installed external binaries, with no network fallback.

    Each game must appear in Sound and produce actual output while it is the
    only playback client. This catches old embedded UDP-only SDK runtimes.
    """
    wav = Path(s.out).resolve().parent.parent / "audio-ports-offline.wav"

    def check_port(name):
        x,y = open_sound_sheet(s,name+"-sound-open")
        sound_tab(s,x,y,"Applications")
        s.qmp.move_to(1000,120)
        time.sleep(1.0)
        image = s.shot(name+"-sound-list")
        if sound_app_row(image,x,y,name) is None:
            raise Failure("installed %s is absent from the mixer without a NIC" % name)
        expect_text(image,(x+20,y+366,x+420,y+396),"1 application",TEXT,FACE,
                    "only the real external port is a playback client")
        time.sleep(9.0)
        level = _wav_signal(wav)
        if level is None or level < 0.001:
            raise Failure("installed %s has no audible PCM without a NIC: RMS %s" % (name,level))
        print("  offline %s: mixer row present, captured RMS %.4f" % (name,level))
        s.qmp.chord("alt","f4")  # Sound
        time.sleep(2.0)
        s.qmp.chord("alt","f4")  # Game
        time.sleep(3.0)

    s.open_doom()
    for _ in range(4):
        s.qmp.tap("ret",pause=1.5)
    time.sleep(10.0)
    check_port("doomgeneric")
    s.open_ccleste()
    s.qmp.tap("z",pause=3.0)
    check_port("ccleste")
    s.shot("offline-ports-closed")


def scenario_volume_popup(s):
    """The tray volume popup lists audiod's native streams and adjusts them.

    Run without a NIC by the volume-popup-offline smoke: the legacy UDP census
    needed a configured IPv4 address, so on a host without a supported NIC the
    popup listed nothing while Sound, on the native port, listed everything.
    """
    s.open_console()
    s.qmp.type_line("volume 80; volume unmute; start nativetest.sxe audio-source ToneA; start nativetest.sxe audio-source ToneB; exit")
    s.qmp.tap("ret")
    time.sleep(5.0)
    s.qmp.move_to(1208,787)
    s.qmp.click()
    s.qmp.move_to(1000,120)
    time.sleep(2.0)
    image = s.shot("volume-popup")
    anchor = find_caption(image,"Applications")
    if anchor is None:
        raise Failure("the volume popup does not show its Applications section")
    # "Applications" is drawn at popup (10,110).
    x,y = anchor[0]-10,anchor[1]-110
    box = (x,y,x+232,y+284)
    expect_text(image,box,"Output device",TEXT,FACE,"popup names the output device")
    expect_text(image,box,"Default output (AC'97)",(128,128,128),FACE,"popup shows the selected connector")
    expect_text(image,box,"80%",TEXT,FACE,"master slider shows the kernel volume")
    expect_text(image,(x,y+130,x+232,y+170),"ToneA",TEXT,FACE,"first application row")
    expect_text(image,(x,y+170,x+232,y+210),"ToneB",TEXT,FACE,"second application row")
    expect_text(image,(x,y+250,x+232,y+284),"Sound settings...",(0,0,255),FACE,"popup links to Sound")
    # The wheel over ToneB's row lowers that stream only.
    s.qmp.move_to(x+100,y+176)
    s.qmp.wheel(-2)
    time.sleep(1.5)
    image = s.shot("volume-popup-wheel")
    expect_text(image,(x+180,y+186,x+232,y+206),"90%",TEXT,FACE,"wheel lowers the application under the pointer")
    expect_text(image,(x+180,y+146,x+232,y+166),"100%",TEXT,FACE,"the other application keeps its level")
    # The speaker glyph beside the device mutes the master.
    s.qmp.move_to(x+19,y+49)
    s.qmp.click()
    s.qmp.move_to(x+150,y+20)
    time.sleep(1.0)
    image = s.shot("volume-popup-muted")
    expect_pixel(image,x+22,y+46,(192,0,0),"muted speaker is crossed out")
    s.qmp.move_to(x+19,y+49)
    s.qmp.click()
    s.qmp.move_to(x+150,y+20)
    time.sleep(1.0)
    s.shot("volume-popup-unmuted")
    # The link opens Sound and closes the popup.
    s.qmp.move_to(x+190,y+264)
    s.qmp.click()
    s.qmp.move_to(1000,120)
    for _ in range(15):
        time.sleep(1.0)
        image = s.shot("volume-popup-sound")
        if find_caption(image,"Device volume") is not None:
            break
    else:
        raise Failure("the popup link did not open Sound")


def scenario_sound(s):
    """Open Sound from the volume popup and exercise live SUN mixer controls.

    Two sustained SUN sources keep the list and meters observable. Sample-level
    independence is asserted separately by audiod's selftest and nativetest.
    """
    s.open_console()
    s.qmp.type_line("volume 100; volume unmute; start nativetest.sxe audio-source ToneA; start nativetest.sxe audio-source ToneB; exit")
    s.qmp.tap("ret")
    time.sleep(5.0)
    s.qmp.move_to(1208,787)
    s.qmp.click()
    time.sleep(2.0)
    popup = s.shot("sound-popup")
    expect_text(popup,(1044,740,1276,772),"Sound settings...",(0,0,255),FACE,
                "volume popup offers Sound settings")
    def from_popup():
        s.qmp.move_to(1190,752)
        s.qmp.click()
    x,y = open_sound_sheet(s,"sound-open",launch=from_popup)
    def settle(name):
        s.qmp.move_to(1000,120)
        time.sleep(1.0)
        return s.shot(name)
    # Volume page: the device, its volume and the speaker settings.
    image = settle("sound-volume")
    expect_text(image,(x+64,y+44,x+420,y+68),"AC'97",TEXT,FACE,"Volume page names the playback device")
    expect_text(image,(x+20,y+228,x+420,y+252),"Speaker settings",TEXT,FACE,"Volume page has the speaker settings group")
    expect_text(image,(x+88,y+299,x+420,y+324),"Automatically switch to headphones",(128,128,128),FACE,
                "unsupported jack detection leaves automatic switching disabled")
    expect_text(image,(x+20,y+366,x+420,y+396),"2 applications",TEXT,FACE,
                "control connection does not add a playing application")
    expect_pixel(image,x+84,y+163,SELECT,"device meter shows mixed PCM")
    expect_text(image,(x+160,y+414,x+416,y+444),"Apply",(128,128,128),FACE,
                "changes apply live, so Apply stays disabled")
    # Audio page: the output selector names the effective device and connector.
    sound_tab(s,x,y,"Audio")
    image = settle("sound-audio")
    expect_text(image,(x+20,y+38,x+420,y+60),"Sound playback",TEXT,FACE,"Audio page has a playback group")
    expect_text(image,(x+76,y+83,x+408,y+107),"AC'97 - Default output",TEXT,FIELD,
                "output selector names the effective device and connector")
    s.qmp.move_to(x+398,y+95)
    s.qmp.click()
    time.sleep(1.0)
    image = s.shot("sound-output-list")
    expect_text(image,(x+76,y+105,x+408,y+138),"AC'97 - Default output",SELECT_TEXT,SELECT,
                "output dropdown lists the selected connector")
    s.qmp.tap("ret")
    image = settle("sound-output-selected")
    expect_text(image,(x+76,y+83,x+408,y+107),"AC'97 - Default output",TEXT,FIELD,
                "acknowledged selection preserves the effective connector")
    # Applications page: select B, mute it, and check its meter clears while
    # the device meter still has A.
    sound_tab(s,x,y,"Applications")
    image = settle("sound-clients")
    if sound_app_row(image,x,y,"ToneA") is None:
        raise Failure("Sound does not list ToneA")
    row_b = sound_app_row(image,x,y,"ToneB")
    if row_b is None:
        raise Failure("Sound does not list ToneB")
    s.qmp.move_to(row_b[0]+8,row_b[1]+4)
    s.qmp.click()
    image = settle("sound-selected")
    expect_text(image,(x+32,y+243,x+408,y+266),"ToneB",TEXT,FACE,"selected stream follows list selection")
    s.qmp.move_to(x+39,y+330)
    s.qmp.click()
    image = settle("sound-muted-app")
    expect_pixel(image,x+40,y+303,FIELD,"muted application meter is silent")
    # Unmute preserves gain; keyboard Home/End exercises the native slider.
    s.qmp.move_to(x+39,y+330)
    s.qmp.click()
    s.qmp.move_to(x+190,y+281)
    s.qmp.click()
    s.qmp.tap("home")
    image = settle("sound-zero-gain")
    expect_text(image,(x+360,y+266,x+420,y+294),"0%",TEXT,FACE,"application slider applies zero gain")
    expect_pixel(image,x+40,y+303,FIELD,"zero gain clears selected meter")
    s.qmp.tap("end")
    image = settle("sound-restored-gain")
    expect_text(image,(x+360,y+266,x+420,y+294),"100%",TEXT,FACE,"application slider restores gain")
    expect_pixel(image,x+40,y+303,SELECT,"selected meter resumes")
    # Volume page: device mute clears and restores the device meter.
    sound_tab(s,x,y,"Volume")
    image = settle("sound-volume-again")
    expect_pixel(image,x+84,y+163,SELECT,"another application keeps playing")
    s.qmp.move_to(x+83,y+196)
    s.qmp.click()
    image = settle("sound-master-muted")
    expect_pixel(image,x+84,y+163,FIELD,"device mute clears the device meter")
    s.qmp.move_to(x+83,y+196)
    s.qmp.click()
    image = settle("sound-master-restored")
    expect_pixel(image,x+84,y+163,SELECT,"device unmute restores the device meter")
    # Hardware page lists the connector.
    sound_tab(s,x,y,"Hardware")
    image = settle("sound-hardware")
    expect_text(image,(x+20,y+63,x+420,y+213),"Playback",SELECT_TEXT,SELECT,"Hardware page lists the playback connector")
    expect_text(image,(x+32,y+269,x+408,y+292),"Connector: Default output (default)",TEXT,FACE,
                "Hardware page describes the selected connector")
    s.qmp.move_to(x+200,y+429)
    s.qmp.click()
    settle("sound-closed")


def scenario_sound_capture(s):
    """Record and replay the deterministic stereo microphone through Sound."""
    s.open_console()
    s.qmp.type_line("volume 100; volume unmute; exit")
    s.qmp.tap("ret")
    time.sleep(3.0)
    s.qmp.move_to(1208,787); s.qmp.click(); time.sleep(1.0)
    popup = s.shot("capture-popup")
    expect_text(popup,(1044,740,1276,772),"Sound settings...",(0,0,255),FACE,"popup opens native recording settings")
    def from_popup():
        s.qmp.move_to(1190,752); s.qmp.click()
    x,y = open_sound_sheet(s,"capture-open",launch=from_popup)
    def settle(name):
        s.qmp.move_to(1000,120)
        time.sleep(1.0)
        return s.shot(name)
    INPUT_METER = (x+84,y+249)
    sound_tab(s,x,y,"Audio")
    image = settle("capture-live")
    expect_text(image,(x+20,y+134,x+420,y+156),"Sound recording",TEXT,FACE,"Audio page has a recording group")
    expect_text(image,(x+76,y+179,x+408,y+203),"HD Audio analog codec",TEXT,FIELD,"input selector names the effective capture device")
    expect_pixel(image,*INPUT_METER,SELECT,"known microphone drives the live input meter")
    s.qmp.move_to(x+398,y+191); s.qmp.click(); time.sleep(1.0)
    image = s.shot("capture-input-list")
    expect_text(image,(x+76,y+201,x+408,y+234),"HD Audio analog codec",SELECT_TEXT,SELECT,
                "input dropdown offers the current capture connector")
    s.qmp.tap("ret")
    image = settle("capture-input-selected")
    expect_text(image,(x+20,y+366,x+420,y+396),"0 applications",TEXT,FACE,"idempotent input selection is acknowledged without a route error")
    expect_pixel(image,*INPUT_METER,SELECT,"selecting the current input preserves capture")
    # Input gain and mute must affect the input meter without a playback owner.
    s.qmp.move_to(x+216,y+225); s.qmp.click(); s.qmp.tap("home")
    image = settle("capture-zero")
    expect_text(image,(x+360,y+210,x+420,y+238),"0%",TEXT,FACE,"input gain reaches zero")
    expect_pixel(image,*INPUT_METER,FIELD,"zero input gain clears the meter")
    s.qmp.tap("end")
    image = settle("capture-gain-restored")
    expect_text(image,(x+360,y+210,x+420,y+238),"100%",TEXT,FACE,"input gain is restored")
    expect_pixel(image,*INPUT_METER,SELECT,"input meter resumes with gain")
    s.qmp.move_to(x+83,y+278); s.qmp.click()
    image = settle("capture-muted")
    expect_pixel(image,*INPUT_METER,FIELD,"input mute is effective")
    s.qmp.move_to(x+83,y+278); s.qmp.click()
    image = settle("capture-unmuted")
    expect_pixel(image,*INPUT_METER,SELECT,"input unmute preserves gain")
    # Voice page: the hardware test records three seconds.
    sound_tab(s,x,y,"Voice")
    image = settle("capture-voice")
    expect_pixel(image,x+80,y+149,SELECT,"Voice page mirrors the live input meter")
    expect_text(image,(x+76,y+171,x+196,y+197),"Record test",TEXT,FACE,"recording test is available")
    s.qmp.move_to(x+136,y+184); s.qmp.click(); s.qmp.move_to(1000,120)
    time.sleep(4.0)
    image = s.shot("capture-recorded")
    expect_text(image,(x+76,y+207,x+408,y+233),"Recorded 3.0 s.",TEXT,FACE,"test captured three seconds")
    expect_text(image,(x+204,y+171,x+334,y+197),"Play recording",TEXT,FACE,"captured test can be played")
    # Mute the live input first: replay must not depend on it.
    sound_tab(s,x,y,"Audio")
    s.qmp.move_to(x+83,y+278); s.qmp.click()
    sound_tab(s,x,y,"Voice")
    s.qmp.move_to(x+269,y+184); s.qmp.click()
    time.sleep(0.2)
    playing = s.shot("capture-playing")
    # Capture live states before glyph analysis, walking to the next tab in
    # relative steps: a calibrated PS/2 move_to takes seconds.
    sound_tab(s,x,y,"Applications",rel_from=(x+269,y+184))
    time.sleep(0.4)
    listed = s.shot("capture-playing-list")
    time.sleep(3.0)
    expect_text(playing,(x+76,y+207,x+408,y+233),"Playing the recorded test...",TEXT,FACE,"test playback starts")
    expect_pixel(playing,x+80,y+149,FIELD,"the muted live input stays silent during replay")
    origin = find_text(listed,(x+20,y+63,x+420,y+213),"Sound recording test",SELECT_TEXT,SELECT)[0]
    if origin is None or listed.getpixel((origin[0]-2,origin[1]+3)) != SELECT:
        raise Failure("recorded playback does not appear as a selected application")
    expect_text(listed,(x+20,y+366,x+420,y+396),"1 application",TEXT,FACE,"capture monitoring does not register a playback stream")
    expect_pixel(listed,x+40,y+303,SELECT,"recorded PCM reaches the playback meter while input is muted")
    sound_tab(s,x,y,"Voice")
    image = settle("capture-played")
    expect_text(image,(x+76,y+207,x+408,y+233),"Recorded 3.0 s.",TEXT,FACE,"recording remains replayable")
    sound_tab(s,x,y,"Audio")
    s.qmp.move_to(x+83,y+278); s.qmp.click()
    image = settle("capture-monitor-restored")
    expect_pixel(image,*INPUT_METER,SELECT,"live monitoring resumes after the test")
    s.qmp.move_to(x+200,y+429); s.qmp.click(); settle("capture-closed")


def scenario_cursor_settings(s):
    """Exercise actual native controls, licensing, live changes and reboot."""
    if s.serial is None:
        raise Failure("cursor settings regression requires the guest serial log")
    s.run("mouse.sxe --selftest", wait=1)
    deadline = time.time()+30
    while time.time() < deadline:
        with io.open(s.serial, encoding="utf-8", errors="replace") as log:
            text = log.read()
        if "CURSOR SETTINGS FAIL" in text:
            raise Failure("native cursor settings protocol selftest failed")
        if "CURSOR SETTINGS PASS" in text:
            break
        time.sleep(0.25)
    else:
        raise Failure("native cursor settings protocol selftest timed out")

    def open_panel():
        s.run("mouse.sxe", wait=5)
        image = s.shot("mouse-properties")
        expect_text(image,(0,0,1280,770),"Mouse Properties",SELECT_TEXT,(0,0,128),
                    "the native window publishes its title",which="title",solid_only=True)
        origin = find_text(image,(0,0,1280,770),"Pointers:",TEXT,FACE)[0]
        if origin is None:
            raise Failure("Mouse Properties did not paint its pointer list")
        return origin[0]-22, origin[1]-143, image

    def click(x,y):
        s.qmp.move_to(x,y); s.qmp.click(); s.qmp.move_to(1000,120); time.sleep(0.5)

    x,y,image = open_panel()
    box = (x,y,x+540,y+510)
    expect_text(image,box,"SavanXP Classic",TEXT,FIELD,"classic is the default scheme")
    expect_text(image,box,"Diagonal Resize 2",TEXT,FIELD,"both diagonal roles are listed")
    # Scheme changes preview without changing the applied pack.
    click(x+385,y+88)
    s.qmp.tap("home"); s.qmp.tap("down"); s.qmp.tap("ret")
    image = s.shot("retrosmart-preview")
    expect_text(image,box,"License: GPL-3.0",TEXT,FACE,"Retrosmart GPL notice is visible")
    click(x+460,y+405)
    image = s.shot("retrosmart-license")
    expect_text(image,(0,0,1280,770),"GNU General Public License v3.",TEXT,FACE,"license dialog includes GPLv3")
    s.qmp.tap("esc")
    click(x+475,y+489)
    image = s.shot("retrosmart-applied")
    expect_text(image,box,"Pointer scheme applied and saved.",TEXT,FACE,"Apply saves the chosen pack")
    # Pending Hackneyed preview is discarded by Cancel.
    click(x+385,y+88)
    s.qmp.tap("home"); s.qmp.tap("down"); s.qmp.tap("down"); s.qmp.tap("ret")
    click(x+390,y+489)
    x,y,image = open_panel()
    expect_text(image,(x,y,x+540,y+510),"Retrosmart (Win-ish Classic)",TEXT,FIELD,"Cancel retained the applied pack")
    click(x+390,y+489)

    with io.open(s.serial, encoding="utf-8", errors="replace") as log:
        boots = log.read().count("handoff: starting /bin/init")
    s.qmp.cmd("set-action", reboot="reset")
    s.qmp.cmd("system_reset")
    deadline = time.time()+180
    while time.time() < deadline:
        with io.open(s.serial, encoding="utf-8", errors="replace") as log:
            if log.read().count("handoff: starting /bin/init") > boots:
                break
        time.sleep(0.5)
    else:
        raise Failure("the cursor persistence test did not reboot")
    time.sleep(35)
    x,y,image = open_panel()
    expect_text(image,(x,y,x+540,y+510),"Retrosmart (Win-ish Classic)",TEXT,FIELD,"the chosen pack survived reboot")
    click(x+355,y+405)
    click(x+475,y+489)
    image = s.shot("classic-restored")
    expect_text(image,(x,y,x+540,y+510),"SavanXP Classic",TEXT,FIELD,"Use Default restores the classic scheme")
    click(x+300,y+489)


SCENARIOS = {
    "cursor_settings": scenario_cursor_settings,
    "sound": scenario_sound,
    "volume_popup": scenario_volume_popup,
    "logview": scenario_logview,
    "sound_capture": scenario_sound_capture,
    "doom": scenario_doom,
    "audio_ports_offline": scenario_audio_ports_offline,
    "desktop": scenario_desktop,
    "mediaplayer_unavailable": scenario_mediaplayer_unavailable,
    "alttab": scenario_alttab,
    "clipboard": scenario_clipboard,
    "calc": scenario_calc,
    "mines": scenario_mines,
    "paint": scenario_paint,
    "files": scenario_files,
    "console": scenario_console,
    "appwiz": scenario_appwiz,
    "system": scenario_system,
    "display": scenario_display,
    "effects": scenario_effects,
    "resolution": scenario_resolution,
    "taskbar": scenario_taskbar,
    "startmenu": scenario_startmenu,
    "wheel": scenario_wheel,
    "webview": scenario_webview,
    "notepadwheel": scenario_notepadwheel,
    "kbdlayout": scenario_kbdlayout,
    "contextmenu": scenario_contextmenu,
    "desktopicons": scenario_desktopicons,
    "filesmenu": scenario_filesmenu,
    "filestoolbar": scenario_filestoolbar,
    "taskmgrmenu": scenario_taskmgrmenu,
    "diskmgmt": scenario_diskmgmt,
    "bench": scenario_bench,
    "saturate": scenario_saturate,
    "spin": scenario_spin,
    "nightwood": scenario_nightwood,
    "gears": scenario_gears,
    "ccleste": scenario_ccleste,
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
    parser.add_argument("--socket", type=Path, required=True, help="QMP Unix socket")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--scenario", required=True, choices=sorted(SCENARIOS))
    parser.add_argument("--boot-wait", type=float, default=45.0)
    parser.add_argument("--abs-pointer", action="store_true",
                        help="el guest tiene un puntero absoluto (virtio-tablet), no PS/2")
    opts = parser.parse_args()

    qmp = Qmp(opts.socket, abs_pointer=opts.abs_pointer)
    marker = "handoff: starting /bin/init"
    if not wait_for_marker(opts.serial, marker):
        print("no aparecio '%s' en %s" % (marker, opts.serial), file=sys.stderr)
        return 1
    # El handoff es el arranque de init; la sesion tarda mas en estar pintada.
    time.sleep(opts.boot_wait)

    try:
        SCENARIOS[opts.scenario](Session(qmp, opts.out, opts.serial))
    except Failure as failure:
        print("FALLO: %s" % failure)
        print("Las capturas quedaron en %s para ver que paso." % opts.out)
        return 1
    print("%s: OK" % opts.scenario)
    return 0


if __name__ == "__main__":
    sys.exit(main())


def run_scenario(name, socket_path, out_dir, boot_wait=45.0, abs_pointer=False, serial=None):
    """Ejecuta un escenario visual contra una instancia YA encendida.

    Es el punto de entrada que usa `build.sh smoke` para los escenarios que
    necesitan mirar la pantalla. `main()` hace lo mismo pero ademas levanta
    QEMU, espera el handoff en el log y resuelve las rutas desde argv; aca la
    instancia ya esta corriendo y el que espera el arranque es el runner.

    Devuelve la lista de capturas que el escenario produjo, para que el runner
    pueda reportar cuantas hubo igual que hace con taskbar.
    """
    if name not in SCENARIOS:
        raise Failure("escenario desconocido: %s" % name)
    os.makedirs(out_dir, exist_ok=True)
    qmp = Qmp(Path(socket_path), abs_pointer=abs_pointer)
    # El handoff es el arranque de init; despues la sesion tarda en estar pintada.
    if boot_wait:
        time.sleep(boot_wait)
    session = Session(qmp, out_dir, serial)
    SCENARIOS[name](session)
    return sorted(str(p) for p in Path(out_dir).glob("*.png"))
