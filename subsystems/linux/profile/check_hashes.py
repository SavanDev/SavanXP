#!/usr/bin/env python3
"""Check that built Linux fixtures are the files the references came from.

    check_hashes.py fixtures.sha256 BUILT_DIR NAME...

Each NAME in BUILT_DIR must have the SHA-256 that fixtures.sha256 records.
A mismatch means the toolchain or a source changed since profile.sh took the
reference outputs: run it again and commit what it produces.
"""

import hashlib
import os
import sys


def main():
    table, built, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    want = {}
    for line in open(table):
        digest, name = line.split()
        want[name] = digest
    bad = 0
    for name in names:
        have = hashlib.sha256(open(os.path.join(built, name), "rb").read()).hexdigest()
        if want.get(name) != have:
            print(f"{name}: built {have}, references taken from {want.get(name)}")
            bad += 1
    if bad:
        print("re-run subsystems/linux/profile/profile.sh and commit its outputs")
        return 1
    print(f"{len(names)} fixtures match their references")
    return 0


if __name__ == "__main__":
    sys.exit(main())
