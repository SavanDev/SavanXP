#!/usr/bin/env python3
"""Recording X11 proxy for the L0 inventory of the Linux subsystem.

Listens on a Unix socket as an X display, forwards every connection to a real
server, and writes one line per request, reply, error and event. Every
QueryExtension reply is rewritten to "absent", which is how x11d will answer,
so the client is traced on the code path the profile will run.

    x11rec.py --listen /tmp/.X11-unix/X1 --upstream /tmp/.X11-unix/X0 \
              --log trace.txt [--toplevel wid.txt]

The log has no times and no addresses, so two runs of the same client can be
compared line by line. Resource ids are printed relative to the client's
resource base (as r+0x...).
"""

import argparse
import os
import selectors
import socket
import struct
import sys

REQUESTS = {
    1: "CreateWindow", 2: "ChangeWindowAttributes", 3: "GetWindowAttributes",
    4: "DestroyWindow", 5: "DestroySubwindows", 6: "ChangeSaveSet",
    7: "ReparentWindow", 8: "MapWindow", 9: "MapSubwindows", 10: "UnmapWindow",
    11: "UnmapSubwindows", 12: "ConfigureWindow", 13: "CirculateWindow",
    14: "GetGeometry", 15: "QueryTree", 16: "InternAtom", 17: "GetAtomName",
    18: "ChangeProperty", 19: "DeleteProperty", 20: "GetProperty",
    21: "ListProperties", 22: "SetSelectionOwner", 23: "GetSelectionOwner",
    24: "ConvertSelection", 25: "SendEvent", 26: "GrabPointer",
    27: "UngrabPointer", 28: "GrabButton", 29: "UngrabButton",
    30: "ChangeActivePointerGrab", 31: "GrabKeyboard", 32: "UngrabKeyboard",
    33: "GrabKey", 34: "UngrabKey", 35: "AllowEvents", 36: "GrabServer",
    37: "UngrabServer", 38: "QueryPointer", 39: "GetMotionEvents",
    40: "TranslateCoordinates", 41: "WarpPointer", 42: "SetInputFocus",
    43: "GetInputFocus", 44: "QueryKeymap", 45: "OpenFont", 46: "CloseFont",
    47: "QueryFont", 48: "QueryTextExtents", 49: "ListFonts",
    50: "ListFontsWithInfo", 51: "SetFontPath", 52: "GetFontPath",
    53: "CreatePixmap", 54: "FreePixmap", 55: "CreateGC", 56: "ChangeGC",
    57: "CopyGC", 58: "SetDashes", 59: "SetClipRectangles", 60: "FreeGC",
    61: "ClearArea", 62: "CopyArea", 63: "CopyPlane", 64: "PolyPoint",
    65: "PolyLine", 66: "PolySegment", 67: "PolyRectangle", 68: "PolyArc",
    69: "FillPoly", 70: "PolyFillRectangle", 71: "PolyFillArc", 72: "PutImage",
    73: "GetImage", 74: "PolyText8", 75: "PolyText16", 76: "ImageText8",
    77: "ImageText16", 78: "CreateColormap", 79: "FreeColormap",
    80: "CopyColormapAndFree", 81: "InstallColormap", 82: "UninstallColormap",
    83: "ListInstalledColormaps", 84: "AllocColor", 85: "AllocNamedColor",
    86: "AllocColorCells", 87: "AllocColorPlanes", 88: "FreeColors",
    89: "StoreColors", 90: "StoreNamedColor", 91: "QueryColors",
    92: "LookupColor", 93: "CreateCursor", 94: "CreateGlyphCursor",
    95: "FreeCursor", 96: "RecolorCursor", 97: "QueryBestSize",
    98: "QueryExtension", 99: "ListExtensions", 100: "ChangeKeyboardMapping",
    101: "GetKeyboardMapping", 102: "ChangeKeyboardControl",
    103: "GetKeyboardControl", 104: "Bell", 105: "ChangePointerControl",
    106: "GetPointerControl", 107: "SetScreenSaver", 108: "GetScreenSaver",
    109: "ChangeHosts", 110: "ListHosts", 111: "SetAccessControl",
    112: "SetCloseDownMode", 113: "KillClient", 114: "RotateProperties",
    115: "ForceScreenSaver", 116: "SetPointerMapping", 117: "GetPointerMapping",
    118: "SetModifierMapping", 119: "GetModifierMapping", 127: "NoOperation",
}

EVENTS = {
    2: "KeyPress", 3: "KeyRelease", 4: "ButtonPress", 5: "ButtonRelease",
    6: "MotionNotify", 7: "EnterNotify", 8: "LeaveNotify", 9: "FocusIn",
    10: "FocusOut", 11: "KeymapNotify", 12: "Expose", 13: "GraphicsExpose",
    14: "NoExpose", 15: "VisibilityNotify", 16: "CreateNotify",
    17: "DestroyNotify", 18: "UnmapNotify", 19: "MapNotify",
    20: "MapRequest", 21: "ReparentNotify", 22: "ConfigureNotify",
    23: "ConfigureRequest", 24: "GravityNotify", 25: "ResizeRequest",
    26: "CirculateNotify", 27: "CirculateRequest", 28: "PropertyNotify",
    29: "SelectionClear", 30: "SelectionRequest", 31: "SelectionNotify",
    32: "ColormapNotify", 33: "ClientMessage", 34: "MappingNotify",
    35: "GenericEvent",
}

ERRORS = {
    1: "Request", 2: "Value", 3: "Window", 4: "Pixmap", 5: "Atom",
    6: "Cursor", 7: "Font", 8: "Match", 9: "Drawable", 10: "Access",
    11: "Alloc", 12: "Colormap", 13: "GContext", 14: "IDChoice", 15: "Name",
    16: "Length", 17: "Implementation",
}

# The predefined atoms of the core protocol, by value.
ATOMS = [None, "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL",
    "COLORMAP", "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2",
    "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7",
    "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE",
    "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP",
    "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING",
    "VISUALID", "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE",
    "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS",
    "WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE",
    "END_SPACE", "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y",
    "UNDERLINE_POSITION", "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT",
    "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH", "WEIGHT",
    "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME",
    "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR"]


def atom_name(value):
    if 0 < value < len(ATOMS):
        return ATOMS[value]
    return f"atom{value}"


class Conn:
    def __init__(self, num, client, server, log, toplevel_path):
        self.num = num
        self.client = client
        self.server = server
        self.log = log
        self.toplevel_path = toplevel_path
        self.cbuf = b""
        self.sbuf = b""
        self.client_setup_done = False
        self.server_setup_done = False
        self.end = "<"
        self.seq = 0
        self.ext_queries = {}  # sequence (16 bits) -> extension name
        self.root = None
        self.rbase = 0
        self.rmask = 0
        self.toplevel = None

    def emit(self, text):
        self.log.write(f"{self.num} {text}\n")
        self.log.flush()

    def rid(self, value):
        if self.rmask and (value & ~self.rmask) == self.rbase:
            return f"r+{value & self.rmask:#x}"
        if value == self.root:
            return "root"
        return f"{value:#x}"

    def u16(self, data, off):
        return struct.unpack_from(self.end + "H", data, off)[0]

    def u32(self, data, off):
        return struct.unpack_from(self.end + "I", data, off)[0]

    # Client to server.
    def from_client(self, data):
        self.cbuf += data
        out = b""
        if not self.client_setup_done:
            if len(self.cbuf) < 12:
                return out
            self.end = "<" if self.cbuf[0:1] == b"l" else ">"
            n = self.u16(self.cbuf, 6)
            d = self.u16(self.cbuf, 8)
            total = 12 + n + (-n % 4) + d + (-d % 4)
            if len(self.cbuf) < total:
                return out
            major = self.u16(self.cbuf, 2)
            order = "LSB" if self.end == "<" else "MSB"
            self.emit(f"C setup {order} protocol={major} auth-name-len={n} auth-data-len={d}")
            out += self.cbuf[:total]
            self.cbuf = self.cbuf[total:]
            self.client_setup_done = True
        while len(self.cbuf) >= 4:
            length = self.u16(self.cbuf, 2) * 4
            if length == 0:
                self.emit("C big request: not expected with BIG-REQUESTS absent")
                if len(self.cbuf) < 8:
                    break
                length = self.u32(self.cbuf, 4) * 4
            if len(self.cbuf) < length:
                break
            req = self.cbuf[:length]
            self.cbuf = self.cbuf[length:]
            self.seq = (self.seq + 1) & 0xFFFF
            self.request(req)
            out += req
        return out

    def request(self, req):
        op = req[0]
        name = REQUESTS.get(op, f"Extension{op}")
        detail = ""
        try:
            if op == 1:
                wid, parent = self.u32(req, 4), self.u32(req, 8)
                x, y, w, h = struct.unpack_from(self.end + "hhHH", req, 12)
                mask = self.u32(req, 28)
                detail = f" wid={self.rid(wid)} parent={self.rid(parent)} {w}x{h}+{x}+{y} mask={mask:#x}"
                if parent == self.root and self.toplevel is None:
                    self.toplevel = wid
                    if self.toplevel_path:
                        with open(self.toplevel_path, "w") as f:
                            f.write(f"{wid:#x}\n")
            elif op == 16:
                n = self.u16(req, 4)
                detail = f" only-if-exists={req[1]} name={req[8:8 + n].decode('latin-1')}"
            elif op == 98:
                n = self.u16(req, 4)
                ext = req[8:8 + n].decode("latin-1")
                self.ext_queries[self.seq] = ext
                detail = f" name={ext}"
            elif op == 18:
                wid, prop, typ = self.u32(req, 4), self.u32(req, 8), self.u32(req, 12)
                detail = (f" window={self.rid(wid)} property={atom_name(prop)} type={atom_name(typ)}"
                          f" format={req[16]} mode={req[1]}")
            elif op == 20:
                prop, typ = self.u32(req, 8), self.u32(req, 12)
                detail = f" window={self.rid(self.u32(req, 4))} property={atom_name(prop)} type={atom_name(typ)}"
            elif op == 53:
                w, h = struct.unpack_from(self.end + "HH", req, 12)
                detail = f" pixmap={self.rid(self.u32(req, 4))} depth={req[1]} {w}x{h}"
            elif op == 72:
                w, h = struct.unpack_from(self.end + "HH", req, 12)
                detail = f" drawable={self.rid(self.u32(req, 4))} format={req[1]} depth={req[21]} {w}x{h}"
            elif op in (2, 3, 4, 8, 10, 12, 14, 15):
                detail = f" window={self.rid(self.u32(req, 4))}"
                if op == 12:
                    detail += f" mask={self.u16(req, 8):#x}"
            elif op in (55, 56):
                detail = f" gc={self.rid(self.u32(req, 4))}"
            elif op in (64, 65, 66, 67, 68, 69, 70, 71):
                detail = f" drawable={self.rid(self.u32(req, 4))} bytes={len(req) - 12}"
            elif op == 38:
                detail = f" window={self.rid(self.u32(req, 4))}"
            elif op == 45:
                n = self.u16(req, 8)
                detail = f" name={req[12:12 + n].decode('latin-1')}"
            elif op == 85:
                n = self.u16(req, 8)
                detail = f" name={req[12:12 + n].decode('latin-1')}"
        except struct.error:
            detail = " (truncated)"
        self.emit(f"C seq={self.seq} {name} len={len(req)}{detail}")

    # Server to client.
    def from_server(self, data):
        self.sbuf += data
        out = bytearray()
        if not self.server_setup_done:
            if len(self.sbuf) < 8:
                return bytes(out)
            total = 8 + self.u16(self.sbuf, 6) * 4
            if len(self.sbuf) < total:
                return bytes(out)
            setup = self.sbuf[:total]
            self.sbuf = self.sbuf[total:]
            self.server_setup_done = True
            if setup[0] == 1:
                self.rbase = self.u32(setup, 12)
                self.rmask = self.u32(setup, 16)
                vlen = self.u16(setup, 24)
                nfmt = setup[29]
                screen = 40 + vlen + (-vlen % 4) + 8 * nfmt
                self.root = self.u32(setup, screen)
                w, h = self.u16(setup, screen + 20), self.u16(setup, screen + 22)
                self.emit(f"S setup success screen={w}x{h}")
            else:
                self.emit(f"S setup refused status={setup[0]}")
            out += setup
        while len(self.sbuf) >= 32:
            kind = self.sbuf[0]
            if kind == 1:
                total = 32 + self.u32(self.sbuf, 4) * 4
            else:
                total = 32
            if len(self.sbuf) < total:
                break
            msg = bytearray(self.sbuf[:total])
            self.sbuf = self.sbuf[total:]
            seq = self.u16(msg, 2)
            if kind == 1:
                ext = self.ext_queries.pop(seq, None)
                if ext is not None:
                    real = "present" if msg[8] else "absent"
                    msg[8:12] = b"\0\0\0\0"
                    self.emit(f"S reply seq={seq} QueryExtension {ext} answered absent (server: {real})")
                else:
                    self.emit(f"S reply seq={seq} len={total}")
            elif kind == 0:
                code = msg[1]
                major = msg[10]
                self.emit(
                    f"S error seq={seq} {ERRORS.get(code, code)} "
                    f"request={REQUESTS.get(major, major)}"
                )
            else:
                ev = kind & 0x7F
                sent = " sent" if kind & 0x80 else ""
                self.emit(f"S event {EVENTS.get(ev, ev)}{sent}")
            out += msg
        return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", required=True)
    ap.add_argument("--upstream", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--toplevel")
    args = ap.parse_args()

    try:
        os.unlink(args.listen)
    except FileNotFoundError:
        pass
    lsock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    lsock.bind(args.listen)
    lsock.listen(8)
    log = open(args.log, "w")
    sel = selectors.DefaultSelector()
    sel.register(lsock, selectors.EVENT_READ, None)
    count = 0
    print("x11rec: ready", file=sys.stderr, flush=True)

    while True:
        for key, _ in sel.select():
            if key.data is None:
                client, _ = lsock.accept()
                server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                server.connect(args.upstream)
                count += 1
                conn = Conn(count, client, server, log, args.toplevel)
                conn.emit("open")
                sel.register(client, selectors.EVENT_READ, (conn, "c"))
                sel.register(server, selectors.EVENT_READ, (conn, "s"))
                continue
            conn, side = key.data
            src = conn.client if side == "c" else conn.server
            try:
                data = src.recv(65536)
            except OSError:
                data = b""
            if not data:
                conn.emit("close by " + ("client" if side == "c" else "server"))
                for s in (conn.client, conn.server):
                    try:
                        sel.unregister(s)
                    except (KeyError, ValueError):
                        pass
                    s.close()
                continue
            if side == "c":
                conn.server.sendall(conn.from_client(data))
            else:
                conn.client.sendall(conn.from_server(data))


if __name__ == "__main__":
    main()
