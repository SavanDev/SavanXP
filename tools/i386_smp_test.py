#!/usr/bin/env python3
"""Verify native SMP startup, hardware contexts and concurrent user scheduling."""
from __future__ import annotations
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time

from legacy_boot_test import wait_serial, type_shell
from i386_console_iso import make_console_iso
from qmp_client import QmpClient
from run_qemu import build_qemu_args, find_qemu


def run(iso: Path, disk: Path, artifacts: Path, smp: int, virtio: bool, accel: str, timeout: float, scheduling: bool = False, idle: bool = False, kernel_preempt: bool = False, wakeup: bool = False, hardening: bool = False) -> int:
    console = scheduling or idle or kernel_preempt or wakeup or hardening
    if kernel_preempt and smp > 8:
        raise ValueError("kernel-preempt supports 1..8 CPUs for its bounded regression workload")
    if console and virtio:
        raise ValueError("smp uses the PS/2 console; validate virtio with sxgui-smoke/virtio-smoke")
    if smp < 2 and not (idle or kernel_preempt or wakeup or hardening):
        raise ValueError("smp-boot requires --smp 2..32 to exercise an AP")
    artifacts.mkdir(parents=True, exist_ok=True)
    serial = artifacts / "serial.log"
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="sx-i386-smp.") as temporary:
        qmp = Path(temporary) / "qmp"
        boot_iso = make_console_iso(iso.resolve(), Path(temporary)) if console else iso
        command = build_qemu_args(iso=boot_iso, disk_image=disk, debug_log=artifacts / "debug.log",
            accel=accel, smp=smp, headless=True, virtio=virtio, snapshot=True,
            serial_path=serial, qmp_path=qmp,
            # qemu32 has neither SMEP nor RDRAND; max exposes both to i386.
            cpu="max" if hardening else "qemu32")
        command[0] = find_qemu() or "qemu-system-i386"
        with (artifacts / "qemu.log").open("w") as log:
            guest = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                wait_serial(serial, r"savanxp:/\$ " if console else "user: /bin/progman loaded", timeout=min(timeout, 90))
                if hardening:
                    type_shell(qmp, ("guardtest32",))
                    wait_serial(serial, r"GUARD32 PASS.*?savanxp:/\$ ", timeout=timeout)
                    text = serial.read_text(errors="replace")
                    for token in ("cpu: SMEP enabled", "cpu: stack guard from RDRAND"):
                        if token not in text:
                            raise RuntimeError(f"missing hardening evidence: {token}")
                if wakeup:
                    type_shell(qmp, ("wakeup32",))
                    result = wait_serial(serial, r"WAKEUP (PASS|FAIL).*?savanxp:/\$ ", timeout=timeout)
                    if result[1] != "PASS": raise RuntimeError("syscall wakeup handoff regression failed")
                    text = serial.read_text(errors="replace")
                    if len(re.findall(r"WAKEUP [\w-]+ PASS rounds=16", text)) != 14:
                        raise RuntimeError("missing event/pipe/poll handoff coverage")
                    if smp == 1 and "order=checked" not in text:
                        raise RuntimeError("single-CPU execution order was not checked")
                if kernel_preempt:
                    type_shell(qmp, ("kpreempt32",))
                    result = wait_serial(serial, r"KPREEMPT (PASS|FAIL).*?savanxp:/\$ ", timeout=timeout)
                    if result[1] != "PASS": raise RuntimeError("kernel-continuation regression failed")
                    text = serial.read_text(errors="replace")
                    workers = set(re.findall(r"KPREEMPT worker pid=(\d+)", text))
                    victims = set(re.findall(r"KPREEMPT victim pid=(\d+)", text))
                    summaries = re.findall(r"process: kernel continuations pid=(\d+) suspended=(\d+) resumed=(\d+) cpu=(\d+)", text)
                    tested = [r for r in summaries if r[0] in workers]
                    if not tested or any(int(r[1]) <= 0 or r[1] != r[2] for r in tested):
                        raise RuntimeError("no balanced real ring-0 suspensions/resumptions")
                    suspended = dict(re.findall(r"process: kernel suspended pid=(\d+) cpu=(\d+)", text))
                    resumed = dict(re.findall(r"process: kernel resumed pid=(\d+) cpu=(\d+) stack=PASS", text))
                    if any(suspended.get(r[0]) != resumed.get(r[0]) or r[0] not in resumed for r in tested):
                        raise RuntimeError("a kernel continuation migrated or failed its stack checks")
                    deferred = re.findall(r"process: kernel kill deferred pid=(\d+) cpu=(\d+) suspended=(\d+)", text)
                    acknowledged = set(re.findall(r"process: kernel kill resumed pid=(\d+) cpu=\d+", text))
                    if len(victims) != 3 or not victims <= {r[0] for r in deferred} or not victims <= acknowledged:
                        raise RuntimeError("deferred kernel kills did not resume safely")
                    if smp == 1 and not victims <= {r[0] for r in deferred if r[2] == "1"}:
                        raise RuntimeError("single-CPU kill did not target suspended kernel contexts")
                if scheduling or idle:
                    type_shell(qmp, ("idletest",))
                    result = wait_serial(serial, r"IDLE (PASS|FAIL).*?savanxp:/\$ ", timeout=timeout)
                    if result[1] != "PASS": raise RuntimeError("idle-process accounting regression failed")
                    type_shell(qmp, ("taskmgr --selftest",))
                    result = wait_serial(serial, r"TASKMGR SMOKE (PASS|FAIL).*?savanxp:/\$ ", timeout=timeout)
                    if result[1] != "PASS": raise RuntimeError("reference Task Manager selftest failed")
                    text = serial.read_text(errors="replace")
                    entered = re.findall(r"idle: ring3 cpu=(\d+) pid=(\d+)", text)
                    if len(entered) != smp or {int(r[0]) for r in entered} != set(range(smp)) or len({r[1] for r in entered}) != smp:
                        raise RuntimeError("not every scheduling CPU executed its own ring-3 idle")
                if scheduling:
                    type_shell(qmp, ("smptest",))
                    wait_serial(serial, r"smptest: ok.*?savanxp:/\$ ", timeout=timeout)
                    text = serial.read_text(errors="replace")
                    if ("spinners ran in parallel" not in text or "smp: remote termination requested" not in text
                            or "smp: reschedule IPI received" not in text):
                        raise RuntimeError("no concurrent execution or remote teardown was observed")
                    ids = {int(n) for n in re.findall(r"smptest: (?:worker|parent) lapic=(\d+)", text)}
                    if not ids or not any(n != 0 for n in ids):
                        raise RuntimeError("no user process executed on an AP")
                    # Existing register/x87/SSE and fault/reclamation gates under SMP.
                    type_shell(qmp, ("multitest32",))
                    wait_serial(serial, r"MULTITASK PASS.*?savanxp:/\$ ", timeout=timeout)
                text = serial.read_text(errors="replace")
                aps = smp - 1
                # As in v0.4.0, -cpu max runs x2APIC; qemu32 has no x2APIC.
                apic_mode = "x2APIC" if hardening else "xAPIC"
                required = ("timer: local APIC target_hz=1000",
                    f"smp: reported={smp} online={smp} scheduling=1 ({apic_mode})",
                    f"smp: private TSS {aps}/{aps}, targeted IPI {2 * aps}/{2 * aps} PASS",
                    f"smp: scheduling={smp} online={smp} (local APIC timers)",
                    *(("smp: lazy kernel TLB PASS",) if smp > 1 else ()))
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
                selectors, tss_bases = set(), set()
                if len(contexts) != smp:
                    raise RuntimeError("QMP did not expose each hardware CPU context")
                for context in contexts:
                    tr = re.search(r"TR\s*=([0-9a-f]+) ([0-9a-f]+) ([0-9a-f]+)", context, re.I)
                    if not tr or int(tr[3], 16) != 103:
                        raise RuntimeError("hardware TSS limit is not 104 bytes")
                    selector, base = int(tr[1], 16), int(tr[2], 16)
                    selectors.add(selector); tss_bases.add(base)
                    if hardening and not int(re.search(r"CR4=([0-9a-f]+)", context, re.I)[1], 16) & (1 << 20):
                        raise RuntimeError("a CPU runs without SMEP while the BSP established it")
                    if selector == 0x28:
                        continue
                    cr0 = int(re.search(r"CR0=([0-9a-f]+)", context, re.I)[1], 16)
                    cr4 = int(re.search(r"CR4=([0-9a-f]+)", context, re.I)[1], 16)
                    if cr0 & 0x6000000c or (cr0 & 0x80010001) != 0x80010001 or (cr4 & 0x610) != 0x610:
                        raise RuntimeError("AP paging, write protection, caches or FPU controls differ from the BSP contract")
                if selectors != {0x28 + 8 * n for n in range(smp)} or len(tss_bases) != smp:
                    raise RuntimeError("hardware contexts reused a TSS")
                if "kernel: fatal exception" in text or "BOOT FAIL" in text or (not scheduling and "exception vector" in text):
                    raise RuntimeError("SMP boot contained a process/kernel fault")
                label = "hardening" if hardening else "wakeup handoff" if wakeup else "kernel preemption" if kernel_preempt else "idle accounting" if idle else "scheduling" if scheduling else "startup"
                print(f"i386 SMP {label}: PASS ({smp} CPUs, {aps} private AP TSS, {2 * aps} startup IPIs; {smp} scheduling CPUs)", flush=True)
                return 0
            finally:
                guest.terminate()
                try: guest.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    guest.kill(); guest.wait()
