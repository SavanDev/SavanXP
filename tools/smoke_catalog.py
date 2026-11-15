#!/usr/bin/env python3
"""List the smoke scenarios currently implemented by the i386 branch."""
import argparse

SCENARIOS = {
    "smoke": "memory/ELF32 libraries, reference POSIX/shell, CPU/FPU and desktop graphics",
    "boot": "kernel, persistent root and init/shell startup",
    "processes": "concurrent ELF32 workers, contexts and spawn/wait/exit",
    "shared": "memory objects, heap, fork, SDK integer ABI and ELF32 library calls",
    "shared-memory": "memory objects and ELF32 libraries with 32 MiB RAM",
    "posix": "reference shell, BusyBox, file descriptors, pipes, inheritance and cleanup",
    "memory": "32 MiB RAM, user fault isolation, page protection and ENOMEM rollback",
    "desktop": "desktop glyphs, mouse motion/buttons and cursor restoration",
    "legacy-cpu": "QMP assertion that qemu32 exposes no long mode or NX",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", nargs="?", choices=SCENARIOS)
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    if args.scenario:
        print(SCENARIOS[args.scenario])
    else:
        for name, description in SCENARIOS.items():
            print(f"{name}: {description}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
