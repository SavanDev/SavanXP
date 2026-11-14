#!/usr/bin/env python3
"""Turn the raw outputs of profile.sh trace into the committed references.

    summarize.py OUT EXPECTED

For every run in OUT it writes to EXPECTED:
  NAME.stdout, NAME.stderr, NAME.status   unchanged
  NAME.strace                             normalized, see below
  NAME.x11                                the X request log, X clients only
and prints two Markdown tables: the system call matrix and the X request
matrix, which docs/userland/LINUX_XEYES_PROFILE.md carries.

Normalization keeps what a SavanXP run must reproduce (call order, arguments
that are not addresses, results, errors) and drops what differs from run to
run: process ids, addresses, times and the calls of the launcher before the
traced program's own execve. A run of identical lines is folded into one line
with its count.
"""

import collections
import os
import re
import shutil
import sys

PHASES = [
    ("L1", ["hello", "startup", "errors"]),
    ("L2", ["args", "tls", "heap", "clock", "files-write", "files-check", "files-remove"]),
    ("L3", ["bb-echo", "bb-cat", "bb-ls", "bb-pwd", "bb-wc", "bb-mkdir", "bb-cat-missing"]),
    ("L4", ["jq", "jq-missing-lib", "jq-missing-interp"]),
    ("L5", ["xclient"]),
    ("L6", ["xeyes"]),
]
X_CLIENTS = ["xclient", "xeyes"]

ADDR = re.compile(r"0x[0-9a-f]{5,}")
TIME = re.compile(r"(tv_sec|tv_nsec)=\d+")
PIDLINE = re.compile(r"^(\d+) +")
CALL = re.compile(r"^([a-z_0-9]+)\(")
RESUMED = re.compile(r"^<\.\.\. ([a-z_0-9]+) resumed>")


HOST_PEER = re.compile(r'sun_path="([^"]*/xsock/(X\d+))"\}, \[(\d+) => (\d+)\]')


def guest_peer_length(m):
    guest = "/tmp/.X11-unix/" + m.group(2)
    length = int(m.group(4)) - len(m.group(1)) + len(guest)
    return f'sun_path="{m.group(1)}"}}, [{m.group(3)} => {length}]'


def normalize_strace(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    start = 0
    for i, line in enumerate(lines):
        body = PIDLINE.sub("", line)
        if body.startswith("execve("):
            start = i
    pids = {}
    out = []
    for line in lines[start:]:
        m = PIDLINE.match(line)
        if m:
            pid = m.group(1)
            pids.setdefault(pid, f"P{len(pids) + 1}")
            line = line[m.end():]
        line = ADDR.sub("ADDR", line)
        line = TIME.sub(r"\1=N", line)
        line = re.sub(r'getrandom\("[^"]*"', 'getrandom("RANDOM"', line)
        # The proxy binds the display socket from the host side, so the peer
        # name carries the host's path; the guest named /tmp/.X11-unix/X1.
        # The address length getpeername returns grows with that path too:
        # it is recomputed for the guest's name, or the reference would
        # depend on where the checkout lives.
        line = HOST_PEER.sub(guest_peer_length, line)
        line = re.sub(r'sun_path="[^"]*/xsock/(X\d+)"', r'sun_path="/tmp/.X11-unix/\1"', line)
        for pid, name in pids.items():
            line = re.sub(rf"\b{pid}\b", name, line)
        line = re.sub(r" +=", " =", line)
        out.append(line)
    folded = []
    for line in out:
        if folded and folded[-1][0] == line:
            folded[-1][1] += 1
        else:
            folded.append([line, 1])
    text = "".join(f"{l}  # x{n}\n" if n > 1 else f"{l}\n" for l, n in folded)
    return out, text


def calls_of(lines):
    names = []
    for line in lines:
        m = CALL.match(line) or RESUMED.match(line)
        if m and not line.startswith("+++") and not line.startswith("---"):
            if RESUMED.match(line):
                continue
            names.append(m.group(1))
    return names


def x_requests(path):
    """Request names before and after the first MapWindow of the client."""
    startup, in_use = collections.Counter(), collections.Counter()
    mapped = False
    for line in open(path):
        parts = line.split()
        if len(parts) < 4 or parts[1] != "C" or not parts[2].startswith("seq="):
            continue
        name = parts[3]
        (in_use if mapped else startup)[name] += 1
        if name == "MapWindow":
            mapped = True
    return startup, in_use


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    matrix = collections.defaultdict(dict)  # call -> phase -> count
    xstart = {}
    for phase, runs in PHASES:
        for run in runs:
            for ext in ("stdout", "stderr", "status"):
                shutil.copyfile(os.path.join(src, f"{run}.{ext}"), os.path.join(dst, f"{run}.{ext}"))
            lines, text = normalize_strace(os.path.join(src, f"{run}.strace"))
            with open(os.path.join(dst, f"{run}.strace"), "w") as f:
                f.write(text)
            # X clients: start-up ends with the last call that opens or looks
            # for something; what follows is the event loop and the exit.
            names = calls_of(lines)
            setup = {"open", "access", "connect", "socket", "uname"}
            last = max((i for i, n in enumerate(names) if n in setup), default=-1)
            for i, n in enumerate(names):
                key = phase
                if run in X_CLIENTS:
                    key = f"{phase} start" if i <= last else f"{phase} loop"
                matrix[n][key] = matrix[n].get(key, 0) + 1
            if run in X_CLIENTS:
                shutil.copyfile(os.path.join(src, f"{run}.x11"), os.path.join(dst, f"{run}.x11"))
                xstart[run] = x_requests(os.path.join(src, f"{run}.x11"))

    cols = ["L1", "L2", "L3", "L4", "L5 start", "L5 loop", "L6 start", "L6 loop"]
    order = {c: i for i, c in enumerate(cols)}

    def first(call):
        return min(order[k] for k in matrix[call])

    print("| Call | " + " | ".join(cols) + " |")
    print("| --- |" + " ---: |" * len(cols))
    for call in sorted(matrix, key=lambda c: (first(c), c)):
        row = [str(matrix[call].get(c, "")) for c in cols]
        print(f"| `{call}` | " + " | ".join(row) + " |")
    print()
    names = sorted(set().union(*[set(s) | set(u) for s, u in xstart.values()]))
    print("| Request | xclient start | xclient after map | xeyes start | xeyes after map |")
    print("| --- | ---: | ---: | ---: | ---: |")
    for n in names:
        row = []
        for c in X_CLIENTS:
            s, u = xstart[c]
            row += [str(s.get(n, "")), str(u.get(n, ""))]
        print(f"| `{n}` | " + " | ".join(row) + " |")


if __name__ == "__main__":
    main()
