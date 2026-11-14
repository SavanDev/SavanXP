#!/usr/bin/env python3
"""Make the images SSL must refuse, from a good Linux fixture.

    damage.py GOOD OUT_DIR

Writes OUT_DIR/bad-header (the ELF magic broken), OUT_DIR/truncated (only the
64-byte ELF header, so the program headers it points at are missing) and
OUT_DIR/other-machine (e_machine set to EM_386). linux-hello expects the
kernel to refuse each one with ENOEXEC and leave no process behind.
"""

import os
import sys


def main():
    good = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
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
