#!/usr/bin/env python3
"""Writes the files ldfailtest hands to the shared loader, one per failure step.

Each file is wrong in exactly one way, so ldso_load stops at the step that
checks it (docs/kernel/SHARED_LIBRARIES.md, "The loader's failure paths"):

    empty.so       0 bytes: the file opens, a section over it does not   -> -3
    short.so       10 bytes: less than an ELF header                     -> -4
    notelf.so      text, longer than a header, no ELF magic              -> -5
    exec.so        a valid ELF header of an executable, not a library    -> -5
    badsegment.so  a library whose PT_LOAD reaches past the file's end   -> -6
    nodynamic.so   a library with a segment and no PT_DYNAMIC            -> -7
    needsgone.so   a library needing libldfail_gone.so.0.4, not in /lib  -> -8

They are built here, byte by byte, rather than linked: a linker refuses to
produce most of them, and the ones it would produce carry far more than the
one defect under test.

Usage:
    tools/make_loader_fixtures.py OUTPUT_DIR
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

ET_EXEC = 2
ET_DYN = 3
EM_X86_64 = 62
PT_LOAD = 1
PT_DYNAMIC = 2
PF_R = 4
DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_SYMTAB = 6
DT_STRSZ = 10

EHDR_SIZE = 64
PHDR_SIZE = 56
SYM_SIZE = 24
DYN_SIZE = 16


def elf_header(kind: int, phnum: int) -> bytes:
    ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    return ident + struct.pack(
        "<HHIQQQIHHHHHH",
        kind, EM_X86_64, 1,
        0,                       # e_entry
        EHDR_SIZE if phnum else 0,  # e_phoff
        0,                       # e_shoff
        0,                       # e_flags
        EHDR_SIZE, PHDR_SIZE, phnum,
        0, 0, 0,                 # no section headers
    )


def program_header(kind: int, offset: int, vaddr: int, filesz: int, memsz: int) -> bytes:
    return struct.pack("<IIQQQQQQ", kind, PF_R, offset, vaddr, vaddr, filesz, memsz, 0x1000)


def library(needed: str | None, dynamic: bool, segment_overrun: int = 0) -> bytes:
    """A shared object loaded from one read-only PT_LOAD that covers the whole
    file, with a .dynstr, a .dynsym holding only the null symbol and, when
    `dynamic`, a PT_DYNAMIC naming them (and `needed`, when given)."""
    phnum = 2 if dynamic else 1
    strings = b"\x00" + (needed.encode() + b"\x00" if needed else b"")
    strtab = EHDR_SIZE + phnum * PHDR_SIZE
    symtab = (strtab + len(strings) + 7) & ~7
    dyntab = symtab + SYM_SIZE
    entries = []
    if needed:
        entries.append((DT_NEEDED, 1))
    entries += [(DT_STRTAB, strtab), (DT_SYMTAB, symtab), (DT_STRSZ, len(strings)), (DT_NULL, 0)]
    dyn = b"".join(struct.pack("<qQ", tag, value) for tag, value in entries)
    size = dyntab + (len(dyn) if dynamic else 0)

    headers = program_header(PT_LOAD, 0, 0, size + segment_overrun, size + segment_overrun)
    if dynamic:
        headers += program_header(PT_DYNAMIC, dyntab, dyntab, len(dyn), len(dyn))
    image = bytearray(elf_header(ET_DYN, phnum) + headers)
    image += strings
    image += bytes(symtab - len(image))
    image += bytes(SYM_SIZE)
    if dynamic:
        image += dyn
    assert len(image) == size
    return bytes(image)


def fixtures() -> dict[str, bytes]:
    return {
        "empty.so": b"",
        "short.so": b"\x7fELF\x02\x01\x01\x00\x00\x00",
        "notelf.so": b"This is a text file standing where a library should be.\n" * 2,
        "exec.so": elf_header(ET_EXEC, 0) + bytes(64),
        "badsegment.so": library(None, dynamic=True, segment_overrun=0x10000),
        "nodynamic.so": library(None, dynamic=False),
        "needsgone.so": library("libldfail_gone.so.0.4", dynamic=True),
    }


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, data in fixtures().items():
        (out / name).write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
