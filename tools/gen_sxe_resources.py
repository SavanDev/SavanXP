#!/usr/bin/env python3
"""Genera los blobs de recursos SXE (.sxmeta y .sxicon) para CADA PROGRAMA que
el build va a linkear. Ver docs/userland/SXE_FORMAT.md (fases 2 y "estampado por
default").

`build.sh` lo invoca una sola vez por build, antes de linkear userland; después
el paso de llvm-objcopy estampa los blobs en cada binario como secciones
NO-alloc.

ESTAMPADO POR DEFAULT: todo programa que se pasa por --program recibe un
.sxmeta, tenga o no un .sxres al lado del fuente. Sin manifiesto, el programa
igual sale con NAME (su propio nombre), VERSION (la del sistema), SUBSYSTEM y
BUILD_ID (el commit). El .sxres es el enriquecimiento (icono, descripcion,
accent, mimes) que el generador conserva cuando existe.

NADA DE NUMEROS DUPLICADOS: los magics, tags, versiones, tamanos y topes se
leen de include/sxe/sxe_format.h, y los flags de lanzamiento de
savanxp/syscall.h. Un formato copiado a mano entre el lector y el generador se
desincroniza, y la falla es silenciosa (blobs que el runtime descarta sin decir
por que).

A diferencia del parser de runtime, que ignora lo que no entiende para poder
leer binarios mas nuevos, este es ESTRICTO con lo que SI viene en un .sxres:
una clave desconocida es un error de build. Un typo en un manifiesto tiene que
romper, no dejar la app sin icono en silencio.

Uso:  python tools/gen_sxe_resources.py --project-root DIR --manifest-dir DIR
                                        --output-dir DIR
"""

import argparse
import os
import re
import struct
import sys

from PIL import Image

# --- Lectura de los headers canonicos ---------------------------------------

_DEFINE_PATTERN = re.compile(r"^\s*#define\s+([A-Z][A-Z0-9_]*)\s+(.+?)\s*$")
_COMMENT_PATTERN = re.compile(r"/\*.*?\*/", re.DOTALL)


def parse_defines(path, prefix):
    """Extrae los #define de valor literal (entero, char o string) del header."""
    if not os.path.isfile(path):
        raise SystemExit(f"No se encuentra el header canonico '{path}'.")

    values = {}
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    text = _COMMENT_PATTERN.sub("", text)

    for line in text.splitlines():
        match = _DEFINE_PATTERN.match(line)
        if not match:
            continue
        name, raw = match.group(1), match.group(2).strip()
        if not name.startswith(prefix):
            continue
        # Las expresiones compuestas se saltean a proposito: este generador
        # solo depende de literales, y una expresion mal interpretada seria
        # peor que no tenerla.
        if re.fullmatch(r"0[xX][0-9a-fA-F]+u?", raw):
            values[name] = int(raw.rstrip("uU"), 16)
        elif re.fullmatch(r"[0-9]+u?", raw):
            values[name] = int(raw.rstrip("uU"), 10)
        elif re.fullmatch(r"'(.)'", raw):
            values[name] = ord(raw[1])
        elif re.fullmatch(r'".*"', raw):
            values[name] = raw[1:-1]
    return values


_REQUIRED_FORMAT_KEYS = (
    "SXE_META_MAGIC0", "SXE_META_MAGIC1", "SXE_META_MAGIC2", "SXE_META_MAGIC3",
    "SXE_META_VERSION", "SXE_META_HEADER_BYTES", "SXE_META_MAX_BYTES",
    "SXE_RECORD_HEADER_BYTES", "SXE_RECORD_ALIGNMENT", "SXE_RECORD_NONE", "SXE_RECORD_REQUIRED",
    "SXE_ICON_MAGIC0", "SXE_ICON_MAGIC1", "SXE_ICON_MAGIC2", "SXE_ICON_MAGIC3",
    "SXE_ICON_VERSION", "SXE_ICON_HEADER_BYTES", "SXE_ICON_ENTRY_BYTES",
    "SXE_ICON_MAX_BYTES", "SXE_ICON_MAX_IMAGES", "SXE_ICON_FORMAT_BGRA8888",
    "SXE_ICON_BYTES_PER_PIXEL", "SXE_ICON_SIZE_SMALL", "SXE_ICON_SIZE_LARGE",
    "SXE_VERSION_COMPONENTS", "SXE_TAG_PRIVATE_FIRST",
    "SXE_SECTION_META", "SXE_SECTION_ICON",
)

# Clave del .sxres -> tag del formato. Los valores de los tags salen del
# header; esto es solo el nombre amigable con el que se escriben.
_TEXT_KEYS = {
    "name": "SXE_TAG_NAME",
    "description": "SXE_TAG_DESCRIPTION",
    "version_string": "SXE_TAG_VERSION_STRING",
    "vendor": "SXE_TAG_VENDOR",
    "copyright": "SXE_TAG_COPYRIGHT",
    "build_id": "SXE_TAG_BUILD_ID",
    "interpreter": "SXE_TAG_INTERPRETER",
    # Grupo del launcher. Declararlo es lo que hace que el programa aparezca en
    # la lista de programas; sin la clave el binario sigue siendo lanzable y
    # simplemente no se muestra (ver SXE_TAG_CATEGORY en sxe_format.h).
    "category": "SXE_TAG_CATEGORY",
    # Directorio de datos persistentes. Lo lee el desinstalador para poder
    # ofrecer llevarselos; es una declaracion, no un permiso (ver el comentario
    # de SXE_TAG_DATA_DIR en sxe_format.h).
    "data_dir": "SXE_TAG_DATA_DIR",
}

_LIST_KEYS = {
    "mime_open": "SXE_TAG_MIME_OPEN",
    "ext_open": "SXE_TAG_EXT_OPEN",
}

# Claves con tratamiento propio (no son texto directo).
_SPECIAL_KEYS = (
    "version", "accent", "launch_flags", "window_flags", "subsystem", "icon", "icon_file",
)

_SUBSYSTEM_POSIX = 0


def load_format(project_root):
    fmt = parse_defines(os.path.join(project_root, "include", "sxe", "sxe_format.h"), "SXE_")

    missing = [key for key in _REQUIRED_FORMAT_KEYS if key not in fmt]
    for key in list(_TEXT_KEYS.values()) + list(_LIST_KEYS.values()) + [
        "SXE_TAG_VERSION", "SXE_TAG_ACCENT", "SXE_TAG_LAUNCH_FLAGS", "SXE_TAG_SUBSYSTEM",
        "SXE_TAG_WINDOW_FLAGS"
    ]:
        if key not in fmt:
            missing.append(key)
    if missing:
        raise SystemExit(
            "sxe_format.h no define como literal: " + ", ".join(sorted(set(missing))) +
            ". El generador no puede emitir blobs sin esos valores."
        )
    return fmt


def load_launch_flags(project_root):
    """SAVANXP_DESKTOP_LAUNCH_FLAG_FULLSCREEN -> {'fullscreen': 1}."""
    path = os.path.join(project_root, "include", "abi", "savanxp_abi.h")
    defines = parse_defines(path, "SAVANXP_DESKTOP_LAUNCH_FLAG_")
    flags = {}
    for name, value in defines.items():
        flags[name[len("SAVANXP_DESKTOP_LAUNCH_FLAG_"):].lower()] = value
    if "none" not in flags or "fullscreen" not in flags:
        raise SystemExit(f"'{path}' no define los flags de lanzamiento esperados.")
    return flags


def load_window_flags(project_root):
    """SAVANXP_WM_WINDOW_STYLE_FIXED_SIZE -> {'fixed_size': 1}."""
    path = os.path.join(project_root, "include", "abi", "savanxp_abi.h")
    defines = parse_defines(path, "SAVANXP_WM_WINDOW_STYLE_")
    flags = {}
    for name, value in defines.items():
        flags[name[len("SAVANXP_WM_WINDOW_STYLE_"):].lower()] = value
    if "none" not in flags or "fixed_size" not in flags:
        raise SystemExit(f"'{path}' no define los flags de estilo de ventana esperados.")
    return flags


def load_system_version(project_root):
    """Version del sistema, para los programas que se envian CON el sistema.

    Existe para que 'version=system' en un manifiesto no quede stale al
    proximo bump: hardcodear 0.3.3 en cada .sxres seria la misma clase de
    duplicacion que este generador evita con los tags y los flags.
    """
    path = os.path.join(project_root, "include", "shared", "version.h")
    defines = parse_defines(path, "SAVANXP_VERSION_")
    parts = []
    for suffix in ("MAJOR", "MINOR", "PATCH"):
        key = "SAVANXP_VERSION_" + suffix
        if key not in defines:
            raise SystemExit(f"'{path}' no define {key} como literal entero.")
        parts.append(str(defines[key]))
    return ".".join(parts)


def load_native_osabi(project_root):
    path = os.path.join(project_root, "include", "abi", "savanxp_native_abi.h")
    defines = parse_defines(path, "SXN_ELF_OSABI_NATIVE")
    if "SXN_ELF_OSABI_NATIVE" not in defines:
        raise SystemExit(f"'{path}' no define SXN_ELF_OSABI_NATIVE.")
    return defines["SXN_ELF_OSABI_NATIVE"]


# --- Manifiesto .sxres -------------------------------------------------------


def parse_manifest(path):
    """key=value, con '#' y ';' como comentario. Estricto: una clave repetida
    o desconocida es un error."""
    entries = {}
    with open(path, "r", encoding="utf-8") as handle:
        for number, line in enumerate(handle, start=1):
            line = line.strip()
            if not line or line[0] in "#;":
                continue
            if "=" not in line:
                raise SystemExit(f"{path}:{number}: se esperaba 'clave=valor'.")
            key, value = line.split("=", 1)
            key, value = key.strip().lower(), value.strip()
            if key in entries:
                raise SystemExit(f"{path}:{number}: clave duplicada '{key}'.")
            known = set(_TEXT_KEYS) | set(_LIST_KEYS) | set(_SPECIAL_KEYS)
            if key not in known:
                raise SystemExit(
                    f"{path}:{number}: clave desconocida '{key}'. Validas: " +
                    ", ".join(sorted(known))
                )
            entries[key] = value
    return entries


def parse_version(text, components):
    parts = text.split(".")
    if len(parts) > components:
        raise SystemExit(f"version '{text}': maximo {components} componentes.")
    numbers = []
    for part in parts:
        if not part.isdigit():
            raise SystemExit(f"version '{text}': '{part}' no es un numero.")
        value = int(part)
        if value > 0xFFFF:
            raise SystemExit(f"version '{text}': '{part}' no entra en uint16.")
        numbers.append(value)
    while len(numbers) < components:
        numbers.append(0)
    return numbers


def parse_accent(text):
    raw = text.strip().lstrip("#")
    if raw.lower().startswith("0x"):
        raw = raw[2:]
    if not re.fullmatch(r"[0-9a-fA-F]{6}", raw):
        raise SystemExit(f"accent '{text}': se espera RRGGBB en hexadecimal.")
    return int(raw, 16)


def parse_flag_list(key, text, flags):
    value = 0
    for token in text.split(","):
        token = token.strip().lower()
        if not token:
            continue
        if token not in flags:
            raise SystemExit(
                f"{key} '{token}' desconocido. Validos: " + ", ".join(sorted(flags))
            )
        value |= flags[token]
    return value


def split_list(text):
    return [item.strip() for item in text.split(",") if item.strip()]


# --- Estampado por default ----------------------------------------------------


def apply_automatic_defaults(manifest, program_name, build_id):
    """Identidad minima que todo binario recibe SIN pedirla.
    Una clave presente pero vacia en el .sxres ("name=") cuenta como
    no declarada: se completa igual, no se respeta el vacio.

    Deliberadamente NO se inventan aca: icono, accent, launch_flags,
    descripcion, mimes. Esos son enriquecimiento -- nadie los puede derivar
    del build, y fabricarlos seria peor que no tenerlos."""
    result = dict(manifest)
    if not result.get("name"):
        result["name"] = program_name
    if not result.get("version"):
        result["version"] = "system"
    if not result.get("subsystem"):
        result["subsystem"] = "posix"
    if not result.get("build_id"):
        result["build_id"] = build_id
    return result


# --- Construccion de blobs ---------------------------------------------------


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def build_meta_blob(fmt, manifest, launch_flags, window_flags, native_osabi, system_version, label):
    alignment = fmt["SXE_RECORD_ALIGNMENT"]
    records = []

    def add(tag, payload):
        records.append((tag, payload))

    for key, tag_name in sorted(_TEXT_KEYS.items()):
        if key in manifest and manifest[key]:
            add(fmt[tag_name], manifest[key].encode("utf-8"))

    if "version" in manifest and manifest["version"]:
        text = manifest["version"].strip()
        if text.lower() == "system":
            text = system_version
        numbers = parse_version(text, fmt["SXE_VERSION_COMPONENTS"])
        add(fmt["SXE_TAG_VERSION"], struct.pack("<" + "H" * len(numbers), *numbers))
        # version_string por defecto es lo que se escribio en version: asi una
        # sola linea del manifiesto cubre el valor comparable y el que se
        # muestra, sin obligar a repetirlo.
        if not manifest.get("version_string"):
            add(fmt["SXE_TAG_VERSION_STRING"], text.encode("utf-8"))

    if "accent" in manifest and manifest["accent"]:
        add(fmt["SXE_TAG_ACCENT"], struct.pack("<I", parse_accent(manifest["accent"])))

    if "launch_flags" in manifest and manifest["launch_flags"]:
        value = parse_flag_list("launch_flags", manifest["launch_flags"], launch_flags)
        add(fmt["SXE_TAG_LAUNCH_FLAGS"], struct.pack("<I", value))

    # Estilo de ventana (tamano fijo). Lo lee el WM del binario al crear la
    # ventana, no el que pide el launch: ver SXE_TAG_WINDOW_FLAGS.
    if "window_flags" in manifest and manifest["window_flags"]:
        value = parse_flag_list("window_flags", manifest["window_flags"], window_flags)
        add(fmt["SXE_TAG_WINDOW_FLAGS"], struct.pack("<I", value))

    if "subsystem" in manifest and manifest["subsystem"]:
        name = manifest["subsystem"].strip().lower()
        if name == "posix":
            value = _SUBSYSTEM_POSIX
        elif name == "native":
            value = native_osabi
        else:
            raise SystemExit(f"subsystem '{name}' desconocido (posix|native).")
        add(fmt["SXE_TAG_SUBSYSTEM"], struct.pack("<B", value))

    for key, tag_name in sorted(_LIST_KEYS.items()):
        if key in manifest and manifest[key]:
            items = split_list(manifest[key])
            add(fmt[tag_name], b"\0".join(item.encode("utf-8") for item in items))

    # Orden determinista por tag: dos builds del mismo manifiesto tienen que
    # producir bytes identicos.
    records.sort(key=lambda item: item[0])

    body = bytearray()
    for tag, payload in records:
        body += struct.pack("<HHI", tag, fmt["SXE_RECORD_NONE"], len(payload))
        body += payload
        body += b"\0" * (align_up(len(payload), alignment) - len(payload))

    blob_bytes = fmt["SXE_META_HEADER_BYTES"] + len(body)
    if blob_bytes > fmt["SXE_META_MAX_BYTES"]:
        raise SystemExit(
            f"{label}: .sxmeta ocupa {blob_bytes} bytes y el tope es {fmt['SXE_META_MAX_BYTES']}."
        )

    header = struct.pack(
        "<4sHHII",
        bytes((fmt["SXE_META_MAGIC0"], fmt["SXE_META_MAGIC1"], fmt["SXE_META_MAGIC2"], fmt["SXE_META_MAGIC3"])),
        fmt["SXE_META_VERSION"],
        fmt["SXE_META_HEADER_BYTES"],
        blob_bytes,
        len(records),
    )
    return bytes(header) + bytes(body)


def read_icon_png(path):
    """PNG -> pixeles 0xAARRGGBB. Misma conversion que
    gen_desktop_icon_assets.py, para que los blobs y el set horneado sean el
    mismo formato byte por byte."""
    if not os.path.isfile(path):
        raise SystemExit(f"No se encuentra el asset de icono '{path}'.")
    image = Image.open(path).convert("RGBA")
    width, height = image.size
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            r, g, b, a = image.getpixel((x, y))
            pixels += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
    return width, height, bytes(pixels)


def build_icon_blob(fmt, images, label):
    if len(images) > fmt["SXE_ICON_MAX_IMAGES"]:
        raise SystemExit(f"{label}: {len(images)} imagenes, el tope es {fmt['SXE_ICON_MAX_IMAGES']}.")

    images = sorted(images, key=lambda item: item[0])
    entries_offset = fmt["SXE_ICON_HEADER_BYTES"]
    data_offset = entries_offset + (len(images) * fmt["SXE_ICON_ENTRY_BYTES"])

    entries = bytearray()
    payload = bytearray()
    cursor = data_offset
    for width, height, pixels in images:
        expected = width * height * fmt["SXE_ICON_BYTES_PER_PIXEL"]
        if len(pixels) != expected:
            raise SystemExit(f"{label}: icono {width}x{height} con {len(pixels)} bytes, se esperaban {expected}.")
        entries += struct.pack("<HHIII", width, height, fmt["SXE_ICON_FORMAT_BGRA8888"], cursor, len(pixels))
        payload += pixels
        cursor += len(pixels)

    blob_bytes = data_offset + len(payload)
    if blob_bytes > fmt["SXE_ICON_MAX_BYTES"]:
        raise SystemExit(f"{label}: .sxicon ocupa {blob_bytes} bytes y el tope es {fmt['SXE_ICON_MAX_BYTES']}.")

    header = struct.pack(
        "<4sHHII",
        bytes((fmt["SXE_ICON_MAGIC0"], fmt["SXE_ICON_MAGIC1"], fmt["SXE_ICON_MAGIC2"], fmt["SXE_ICON_MAGIC3"])),
        fmt["SXE_ICON_VERSION"],
        fmt["SXE_ICON_HEADER_BYTES"],
        blob_bytes,
        len(images),
    )
    return bytes(header) + bytes(entries) + bytes(payload)


def collect_icons_from_file(fmt, manifest_path, relative_paths, label):
    """The program's own icon: PNGs resolved RELATIVE to the .sxres.

    This is what lets the art skip assets/desktop/icons/. That set is the
    system catalog -- versioned and baked into the image --, so forcing an
    icon through it would defeat the point of the format: in SXE the icon
    travels INSIDE the executable, and a third-party port must be able to
    bring its own without leaving anything in the system tree.

    `relative_paths` is one PNG or a comma-separated list. A PNG whose size is
    exactly one of the two the runtime requires is used as is for that size;
    a size with no exact PNG is derived from the first one in the list.
    """
    sources = []
    for relative_path in (part.strip() for part in relative_paths.split(",")):
        if not relative_path:
            continue
        path = os.path.normpath(os.path.join(os.path.dirname(manifest_path), relative_path))
        if not os.path.isfile(path):
            raise SystemExit(f"{label}: icon '{path}' not found.")
        sources.append(Image.open(path).convert("RGBA"))
    if not sources:
        raise SystemExit(f"{label}: icon_file names no PNG.")

    images = []
    for size in (fmt["SXE_ICON_SIZE_SMALL"], fmt["SXE_ICON_SIZE_LARGE"]):
        exact_match = [source for source in sources if source.size == (size, size)]
        source = exact_match[0] if exact_match else sources[0]
        if source.size == (size, size):
            scaled = source
        else:
            # An integer multiple in EITHER direction -- a source larger than
            # the target (shrinking) or smaller (enlarging, the usual case for a
            # pixel-art icon drawn at 16x16 that needs a 32x32) -- uses NEAREST
            # so the pixel art stays exact. source%size covers shrinking;
            # size%source covers enlarging. With no clean multiple either way,
            # NEAREST would drop rows or leave uneven blocks, so a smooth
            # resample is the better choice there.
            exact = (
                (source.width % size == 0 and source.height % size == 0) or
                (source.width != 0 and source.height != 0 and
                    size % source.width == 0 and size % source.height == 0)
            )
            scaled = source.resize((size, size), Image.NEAREST if exact else Image.LANCZOS)
        pixels = bytearray()
        for y in range(size):
            for x in range(size):
                r, g, b, a = scaled.getpixel((x, y))
                pixels += struct.pack("<I", (a << 24) | (r << 16) | (g << 8) | b)
        images.append((size, size, bytes(pixels)))
    return images


def collect_icons(fmt, project_root, icon_name, label):
    """El manifiesto referencia el icono por nombre de asset, igual que
    progman.ini lo hace hoy contra el set horneado. Los dos tamanos que el
    sistema usa son obligatorios: un .sxicon con uno solo obligaria a escalar
    en runtime."""
    root = os.path.join(project_root, "assets", "desktop", "icons")
    images = []
    for size in (fmt["SXE_ICON_SIZE_SMALL"], fmt["SXE_ICON_SIZE_LARGE"]):
        path = os.path.join(root, f"{size}x{size}", f"{icon_name}.png")
        width, height, pixels = read_icon_png(path)
        if width != size or height != size:
            raise SystemExit(f"{label}: '{path}' mide {width}x{height}, se esperaba {size}x{size}.")
        images.append((width, height, pixels))
    return images


def c_string(text):
    """A C string literal of `text` in UTF-8, escaping what C needs."""
    out = []
    for byte in text.encode("utf-8"):
        if byte in (0x22, 0x5C):
            out.append("\\" + chr(byte))
        elif 32 <= byte < 127:
            out.append(chr(byte))
        else:
            out.append("\\%03o" % byte)
    return '"' + "".join(out) + '"'


def build_sun_header(fmt, manifest, images, window_flags, label):
    """The resources of a SUN program as C, for <program>.sunres.h.

    A SUN program cannot read its own .sxe -- SUN has no files -- so what the
    POSIX runtime reads from the binary at startup is compiled into the
    program instead: the title, accent, window style and 16x16 icon its
    window publishes (struct sxnwm_presentation_source in sxn/sxnwm.h). The
    .sxe sections are still stamped, for the launcher that scans binaries.
    """
    flags = []
    accent = 0
    if manifest.get("accent"):
        accent = parse_accent(manifest["accent"])
        flags.append("SAVANXP_WM_PRESENTATION_HAS_ACCENT")
    style = 0
    if manifest.get("window_flags"):
        style = parse_flag_list("window_flags", manifest["window_flags"], window_flags)
    small = fmt["SXE_ICON_SIZE_SMALL"]
    icon = next((pixels for width, height, pixels in images if width == small and height == small), None)

    lines = [
        "/* Generated by tools/gen_sxe_resources.py from " + label + ". Do not edit. */",
        "#pragma once",
        "",
        '#include "sxn/sxnwm.h"',
        "",
    ]
    if icon is not None:
        values = struct.unpack("<%dI" % (small * small), icon)
        lines.append("static const uint32_t sun_resources_icon[%d] = {" % (small * small))
        for row in range(small):
            chunk = values[row * small:(row + 1) * small]
            lines.append("    " + ", ".join("0x%08xu" % value for value in chunk) + ",")
        lines.append("};")
        lines.append("")
    lines.append("static const struct sxnwm_presentation_source sun_resources = {")
    lines.append("    " + c_string(manifest.get("name", "")) + ",")
    lines.append("    " + (" | ".join(flags) if flags else "0u") + ",")
    lines.append("    0x%06xu," % accent)
    lines.append("    %du," % style)
    lines.append("    %du," % (small if icon is not None else 0))
    lines.append("    " + ("sun_resources_icon" if icon is not None else "0") + ",")
    lines.append("};")
    return ("\n".join(lines) + "\n").encode("utf-8")


# --- Main --------------------------------------------------------------------


def write_if_changed(path, data):
    """No reescribir un blob identico: evita que el estampado y el link se
    reactiven en cada build por un mtime nuevo."""
    if os.path.isfile(path):
        with open(path, "rb") as handle:
            if handle.read() == data:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as handle:
        handle.write(data)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root")
    parser.add_argument("--manifest-dir", action="append", default=[],
                        help="Directorio donde buscar <programa>.sxres. Se puede repetir.")
    parser.add_argument("--program", action="append", default=[],
                        help="Nombre de un programa que el build va a linkear. "
                             "Se puede repetir; recibe .sxmeta aunque no tenga .sxres.")
    parser.add_argument("--native-program", action="append", default=[],
                        help="A program that runs on SUN: it also gets <name>.sunres.h, "
                             "its resources as C. Repeatable.")
    parser.add_argument("--build-id", default="unknown",
                        help="Commit o id de build a estampar en BUILD_ID cuando el .sxres no lo fija.")
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    project_root = args.project_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fmt = load_format(project_root)
    launch_flags = load_launch_flags(project_root)
    window_flags = load_window_flags(project_root)
    native_osabi = load_native_osabi(project_root)
    system_version = load_system_version(project_root)

    # Primer directorio que trae el .sxres de un programa gana: mismo criterio
    # de "primero en la lista, primero servido" que el resto del repo (ver
    # file_assoc_scan_programs). En la practica nunca hay dos con el mismo
    # nombre, pero el orden queda definido igual.
    sxres_by_name = {}
    for directory in args.manifest_dir:
        if not os.path.isdir(directory):
            continue
        for entry in sorted(os.listdir(directory)):
            if entry.endswith(".sxres"):
                sxres_by_name.setdefault(os.path.splitext(entry)[0], os.path.join(directory, entry))

    # dict.fromkeys en vez de set(): de-duplica preservando el orden de
    # llegada, que es el que decide "primero" si algun dia importara.
    program_names = list(dict.fromkeys(args.program))

    generated = 0
    for name in program_names:
        manifest_path = sxres_by_name.get(name)
        manifest = parse_manifest(manifest_path) if manifest_path else {}
        label = manifest_path or f"{name} (sin .sxres, estampado automatico)"
        manifest = apply_automatic_defaults(manifest, name, args.build_id)

        meta = build_meta_blob(
            fmt, manifest, launch_flags, window_flags, native_osabi, system_version, label)
        write_if_changed(os.path.join(args.output_dir, name + ".sxmeta"), meta)

        icon_path = os.path.join(args.output_dir, name + ".sxicon")
        if manifest.get("icon") and manifest.get("icon_file"):
            raise SystemExit(
                f"{label}: declara 'icon' y 'icon_file' a la vez. El icono sale del "
                "catalogo del sistema o de un PNG propio, no de los dos."
            )
        images = []
        if manifest.get("icon_file"):
            images = collect_icons_from_file(fmt, manifest_path, manifest["icon_file"], label)
            write_if_changed(icon_path, build_icon_blob(fmt, images, label))
        elif manifest.get("icon"):
            images = collect_icons(fmt, project_root, manifest["icon"], label)
            write_if_changed(icon_path, build_icon_blob(fmt, images, label))
        elif os.path.isfile(icon_path):
            # El manifiesto dejo de declarar icono (o nunca tuvo uno): sacar el
            # blob viejo para que el estampado no siga metiendo una seccion
            # fantasma de un build anterior.
            os.remove(icon_path)
        if name in args.native_program:
            write_if_changed(
                os.path.join(args.output_dir, name + ".sunres.h"),
                build_sun_header(fmt, manifest, images, window_flags, os.path.relpath(label, project_root)
                                 if manifest_path else label))
        generated += 1

    # Un .sxres cuyo programa no se paso por --program no es necesariamente un
    # error (puede ser una app excluida por --no-test-apps), pero silenciarlo del
    # todo esconde el typo mas comun: renombrar el .c y olvidar el .sxres.
    orphans = sorted(name for name in sxres_by_name if name not in set(program_names))
    if orphans:
        print("sxe: aviso, .sxres sin programa en este build: " + ", ".join(orphans))

    print(f"sxe: {generated} programa(s) estampado(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
