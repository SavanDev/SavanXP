#!/usr/bin/env python3
"""Genera los blobs de recursos SXE (.sxmeta y .sxicon) para CADA PROGRAMA que
el build va a linkear. Ver docs/SXE_FORMAT.md (fases 2 y "estampado por
default").

build.ps1 lo invoca una sola vez por build, antes de linkear userland; despues
el paso de llvm-objcopy estampa los blobs en cada binario como secciones
NO-alloc.

ESTAMPADO POR DEFAULT: todo programa que se pasa por --program recibe un
.sxmeta, tenga o no un .sxres al lado del fuente. Sin manifiesto, el programa
igual sale con NAME (su propio nombre), VERSION (la del sistema), SUBSYSTEM y
BUILD_ID (el commit) -- es el mismo rol que cumple el bloque VERSIONINFO que
el linker de Windows agrega aunque el programador no haya escrito un .rc. El
.sxres nunca dejo de existir: es el ENRIQUECIMIENTO (icono, descripcion,
accent, mimes) que nadie puede inferir del build.

NADA DE NUMEROS DUPLICADOS: los magics, tags, versiones, tamanos y topes se
leen de include/sxe/sxe_format.h, y los flags de lanzamiento de
savanxp/syscall.h. Es el mismo criterio que Assert-SxfsFormatMatchesHeader:
un formato copiado a mano entre el lector y el generador se desincroniza, y la
falla es silenciosa (blobs que el runtime descarta sin decir por que).

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
}

_LIST_KEYS = {
    "mime_open": "SXE_TAG_MIME_OPEN",
    "ext_open": "SXE_TAG_EXT_OPEN",
}

# Claves con tratamiento propio (no son texto directo).
_SPECIAL_KEYS = ("version", "accent", "launch_flags", "subsystem", "icon", "icon_file")

_SUBSYSTEM_POSIX = 0


def load_format(project_root):
    fmt = parse_defines(os.path.join(project_root, "include", "sxe", "sxe_format.h"), "SXE_")

    missing = [key for key in _REQUIRED_FORMAT_KEYS if key not in fmt]
    for key in list(_TEXT_KEYS.values()) + list(_LIST_KEYS.values()) + [
        "SXE_TAG_VERSION", "SXE_TAG_ACCENT", "SXE_TAG_LAUNCH_FLAGS", "SXE_TAG_SUBSYSTEM"
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
    path = os.path.join(project_root, "subsystems", "posix", "sdk", "v1", "include", "savanxp", "syscall.h")
    defines = parse_defines(path, "SAVANXP_DESKTOP_LAUNCH_FLAG_")
    flags = {}
    for name, value in defines.items():
        flags[name[len("SAVANXP_DESKTOP_LAUNCH_FLAG_"):].lower()] = value
    if "none" not in flags or "fullscreen" not in flags:
        raise SystemExit(f"'{path}' no define los flags de lanzamiento esperados.")
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
    path = os.path.join(project_root, "subsystems", "native", "sdk", "include", "savanxp_native.h")
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


def parse_launch_flag_list(text, flags):
    value = 0
    for token in text.split(","):
        token = token.strip().lower()
        if not token:
            continue
        if token not in flags:
            raise SystemExit(
                f"launch_flags '{token}' desconocido. Validos: " + ", ".join(sorted(flags))
            )
        value |= flags[token]
    return value


def split_list(text):
    return [item.strip() for item in text.split(",") if item.strip()]


# --- Estampado por default ----------------------------------------------------


def apply_automatic_defaults(manifest, program_name, build_id):
    """Identidad minima que todo binario recibe SIN pedirla, igual que un EXE
    de Windows linkeado sin .rc igual sale con su bloque VERSIONINFO por
    default. Una clave presente pero vacia en el .sxres ("name=") cuenta como
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


def build_meta_blob(fmt, manifest, launch_flags, native_osabi, system_version, label):
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
        value = parse_launch_flag_list(manifest["launch_flags"], launch_flags)
        add(fmt["SXE_TAG_LAUNCH_FLAGS"], struct.pack("<I", value))

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


def collect_icons_from_file(fmt, manifest_path, relative_path, label):
    """Icono propio del programa: un PNG resuelto RELATIVO al .sxres.

    Es lo que hace falta para que el arte no tenga que pasar por
    assets/desktop/icons/. Ese set es el catalogo del sistema -- se versiona y se
    hornea en la imagen --, asi que obligar a pasar por ahi para estampar un
    icono contradice el sentido del formato: en SXE el icono viaja DENTRO del
    ejecutable, y un port de terceros tiene que poder traer el suyo sin dejar
    nada en el arbol del sistema.

    De un solo PNG se derivan los dos tamanos que el runtime exige.
    """
    path = os.path.normpath(os.path.join(os.path.dirname(manifest_path), relative_path))
    if not os.path.isfile(path):
        raise SystemExit(f"{label}: no se encuentra el icono '{path}'.")

    source = Image.open(path).convert("RGBA")
    images = []
    for size in (fmt["SXE_ICON_SIZE_SMALL"], fmt["SXE_ICON_SIZE_LARGE"]):
        if source.size == (size, size):
            scaled = source
        else:
            # Multiplo entero en CUALQUIER sentido -- el original mas grande
            # que el destino (baja de tamano) o mas chico (sube, el caso mas
            # comun para un icono de pixel art dibujado a 16x16 que necesita
            # el 32x32) -- usa NEAREST para conservar el pixel art tal cual.
            # source%size cubre bajar; size%source cubre subir. Si no hay
            # multiplo limpio en ninguna direccion, NEAREST se comeria filas o
            # dejaria bloques irregulares, asi que ahi conviene un remuestreo
            # suave.
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
    parser.add_argument("--build-id", default="unknown",
                        help="Commit o id de build a estampar en BUILD_ID cuando el .sxres no lo fija.")
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    project_root = args.project_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    fmt = load_format(project_root)
    launch_flags = load_launch_flags(project_root)
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

        meta = build_meta_blob(fmt, manifest, launch_flags, native_osabi, system_version, label)
        write_if_changed(os.path.join(args.output_dir, name + ".sxmeta"), meta)

        icon_path = os.path.join(args.output_dir, name + ".sxicon")
        if manifest.get("icon") and manifest.get("icon_file"):
            raise SystemExit(
                f"{label}: declara 'icon' y 'icon_file' a la vez. El icono sale del "
                "catalogo del sistema o de un PNG propio, no de los dos."
            )
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
        generated += 1

    # Un .sxres cuyo programa no se paso por --program no es necesariamente un
    # error (puede ser una app excluida por -NoTestApps), pero silenciarlo del
    # todo esconde el typo mas comun: renombrar el .c y olvidar el .sxres.
    orphans = sorted(name for name in sxres_by_name if name not in set(program_names))
    if orphans:
        print("sxe: aviso, .sxres sin programa en este build: " + ", ".join(orphans))

    print(f"sxe: {generated} programa(s) estampado(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
