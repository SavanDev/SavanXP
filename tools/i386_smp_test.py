#!/usr/bin/env python3
"""Verify native AP startup, private TSS/stacks and targeted IPIs in QEMU."""
from __future__ import annotations
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time

from legacy_boot_test import wait_serial
from qmp_client import QmpClient
from run_qemu import build_qemu_args, find_qemu


def run(iso: Path, disk: Path, artifacts: Path, smp: int, virtio: bool, accel: str, timeout: float) -> int:
    if smp < 2:
        raise ValueError("smp-boot requires --smp 2..32 to exercise an AP")
    artifacts.mkdir(parents=True, exist_ok=True)
    serial = artifacts / "serial.log"
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="sx-i386-smp.") as temporary:
        qmp = Path(temporary) / "qmp"
        command = build_qemu_args(iso=iso, disk_image=disk, debug_log=artifacts / "debug.log",
            accel=accel, smp=smp, headless=True, virtio=virtio, snapshot=True,
            serial_path=serial, qmp_path=qmp)
        command[0] = find_qemu() or "qemu-system-i386"
        with (artifacts / "qemu.log").open("w") as log:
            guest = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                wait_serial(serial, "user: /bin/progman loaded", timeout=min(timeout, 90))
                text = serial.read_text(errors="replace")
                aps = smp - 1
                required = (f"smp: reported={smp} online={smp} scheduling=1 (xAPIC)",
                    f"smp: private TSS {aps}/{aps}, targeted IPI {2 * aps}/{2 * aps} PASS")
                for token in required:
                    if token not in text:
                        raise RuntimeError(f"missing SMP evidence: {token}")
                records = re.findall(r"smp: AP index=(\d+) lapic=(\d+) tr=0x([0-9a-f]+) pings=(\d+)", text)
                if len(records) != aps or {int(r[0]) for r in records} != set(range(1, smp)):
                    raise RuntimeError("missing or duplicate dense CPU indices")
                if len({r[1] for r in records}) != aps:
                    raise RuntimeError("APs did not report distinct LAPIC IDs")
                for index, _, selector, pings in records:
                    if int(selector, 16) != 0x28 + 8 * int(index) or int(pings) != 2:
                        raise RuntimeError("AP private TSS or repeated targeted IPI failed")
                if virtio and "virtio-gpu: msi-x vector 49 armed" not in text:
                    raise RuntimeError("virtio GPU did not arm its native local-APIC vector")
                time.sleep(.2)
                with QmpClient(qmp) as client:
                    cpus = client.execute("query-cpus-fast")["return"]
                    registers = client.execute("human-monitor-command", {"command-line": "info registers -a"})["return"]
                (artifacts / "cpus.json").write_text(json.dumps(cpus, indent=2) + "\n")
                (artifacts / "registers.txt").write_text(registers)
                if len(cpus) != smp:
                    raise RuntimeError("QEMU did not create the requested CPUs")
                contexts = re.findall(r"CPU#\d+\s+(.*?)(?=CPU#|\Z)", registers, re.S)
                selectors, tss_bases, stack_pages = set(), set(), set()
                if len(contexts) != smp:
                    raise RuntimeError("QMP did not expose each hardware CPU context")
                for context in contexts:
                    tr = re.search(r"TR\s*=([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)", context, re.I)
                    if not tr or int(tr[3], 16) != 103:
                        raise RuntimeError("hardware TSS limit is not 104 bytes")
                    selector, base = int(tr[1], 16), int(tr[2], 16)
                    selectors.add(selector); tss_bases.add(base)
                    if selector == 0x28:
                        continue
                    cr0 = int(re.search(r"CR0=([0-9a-f]+)", context, re.I)[1], 16)
                    cr4 = int(re.search(r"CR4=([0-9a-f]+)", context, re.I)[1], 16)
                    esp = int(re.search(r"ESP=([0-9a-f]+)", context, re.I)[1], 16)
                    if cr0 & 0x6000000c or (cr0 & 0x80010001) != 0x80010001 or (cr4 & 0x610) != 0x610:
                        raise RuntimeError("AP paging, write protection, caches or FPU controls differ from the BSP contract")
                    if "CPL=0" not in context or "HLT=1" not in context or not 0x40000000 <= esp < 0x80000000:
                        raise RuntimeError("an AP did not remain parked on its reserved supervisor stack")
                    stack_pages.add(esp & ~4095)
                if selectors != {0x28 + 8 * n for n in range(smp)} or len(tss_bases) != smp or len(stack_pages) != aps:
                    raise RuntimeError("hardware contexts reused a TSS or AP stack")
                if "exception vector" in text or "BOOT FAIL" in text:
                    raise RuntimeError("SMP boot contained a process/kernel fault")
                print(f"i386 SMP bring-up: PASS ({smp} CPUs, {aps} private AP TSS/stacks, {2 * aps} targeted IPIs; one scheduling CPU)", flush=True)
                return 0
            finally:
                guest.terminate()
                try: guest.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    guest.kill(); guest.wait()
