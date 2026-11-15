#!/usr/bin/env python3
"""Make the images SSL must refuse, from a good Linux fixture.

    damage.py GOOD OUT_DIR [--dynamic]

Writes OUT_DIR/bad-header (the ELF magic broken), OUT_DIR/truncated (only the
64-byte ELF header, so the program headers it points at are missing) and
OUT_DIR/other-machine (e_machine set to EM_386). linux-hello expects the
kernel to refuse each one with ENOEXEC and leave no process behind.
With --dynamic, writes duplicate, nested and unterminated PT_INTERP variants
for linux-dynamic instead.
"""

import os
import sys


def main():
    good = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    if len(sys.argv) > 3 and sys.argv[3] == "--dynamic":
        phoff = int.from_bytes(good[32:40], "little")
        phnum = int.from_bytes(good[56:58], "little")
        headers = [phoff + i * 56 for i in range(phnum)]
        interp = next(i for i in headers if int.from_bytes(good[i:i + 4], "little") == 3)
        spare = next(i for i in headers if int.from_bytes(good[i:i + 4], "little") == 0x6474e551)
        offset = int.from_bytes(good[interp + 8:interp + 16], "little")
        size = int.from_bytes(good[interp + 32:interp + 40], "little")
        duplicate = bytearray(good)
        duplicate[spare:spare + 56] = good[interp:interp + 56]
        nested = bytearray(good)
        name = b"/fixtures/interp-nested\0"
        assert len(name) <= size
        nested[offset:offset + size] = name.ljust(size, b"\0")
        unterminated = bytearray(good)
        unterminated[offset:offset + size] = b"/" + b"x" * (size - 1)
        for name, data in (("interp-duplicate", duplicate), ("interp-nested", nested),
                           ("interp-unterminated", unterminated)):
            path = os.path.join(out, name)
            with open(path, "wb") as file:
                file.write(data)
            os.chmod(path, 0o755)
        return
    bad_header = bytearray(good)
    bad_header[0] = ord("X")
    other_machine = bytearray(good)
    other_machine[18:20] = (3).to_bytes(2, "little")
    for name, data in (("bad-header", bad_header), ("truncated", good[:64]),
                       ("other-machine", other_machine)):
        path = os.path.join(out, name)
        with open(path, "wb") as f:
            f.write(data)
        os.chmod(path, 0o755)


if __name__ == "__main__":
    main()
