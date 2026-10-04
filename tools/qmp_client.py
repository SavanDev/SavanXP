#!/usr/bin/env python3
"""Small QMP client used by the Linux smoke runner."""

from __future__ import annotations

import json
import socket
import time
from pathlib import Path
from typing import Any


class QmpError(RuntimeError):
    """The QMP connection or a QMP command failed."""


class QmpClient:
    def __init__(self, path: Path, timeout: float = 2.0) -> None:
        self.path = path
        self.timeout = timeout
        self.socket: socket.socket | None = None
        self.reader: Any = None
        self.next_id = 1

    def __enter__(self) -> "QmpClient":
        self.connect()
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> None:
        self.close()

    def connect(self) -> None:
        last_error: OSError | None = None
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            try:
                self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.socket.settimeout(self.timeout)
                self.socket.connect(str(self.path))
                self.reader = self.socket.makefile("rwb", buffering=0)
                greeting = self._read_json()
                if "QMP" not in greeting:
                    raise QmpError(f"unexpected QMP greeting: {greeting!r}")
                self.execute("qmp_capabilities")
                return
            except (FileNotFoundError, ConnectionRefusedError, OSError) as exc:
                last_error = exc
                self.close()
                time.sleep(0.05)
        raise QmpError(f"could not connect to QMP socket {self.path}: {last_error}")

    def close(self) -> None:
        if self.reader is not None:
            self.reader.close()
            self.reader = None
        if self.socket is not None:
            self.socket.close()
            self.socket = None

    def _read_json(self) -> dict[str, Any]:
        if self.reader is None:
            raise QmpError("QMP client is not connected")
        line = self.reader.readline()
        if not line:
            raise QmpError("QMP connection closed")
        try:
            value = json.loads(line)
        except json.JSONDecodeError as exc:
            raise QmpError(f"invalid QMP JSON: {line!r}") from exc
        if not isinstance(value, dict):
            raise QmpError(f"invalid QMP message: {value!r}")
        return value

    def execute(self, command: str, arguments: dict[str, Any] | None = None) -> dict[str, Any]:
        if self.reader is None or self.socket is None:
            raise QmpError("QMP client is not connected")
        command_id = self.next_id
        self.next_id += 1
        message: dict[str, Any] = {"execute": command, "id": command_id}
        if arguments:
            message["arguments"] = arguments
        self.reader.write(json.dumps(message, separators=(",", ":")).encode() + b"\n")
        while True:
            response = self._read_json()
            if response.get("id") != command_id:
                continue
            if "error" in response:
                raise QmpError(f"QMP {command} failed: {response['error']}")
            return response

    def key(self, qcode: str, down: bool) -> None:
        self.execute(
            "input-send-event",
            {
                "events": [
                    {
                        "type": "key",
                        "data": {
                            "down": down,
                            "key": {"type": "qcode", "data": qcode},
                        },
                    }
                ]
            },
        )

    def tap(self, qcode: str, hold: float = 0.05, pause: float = 0.12) -> None:
        self.key(qcode, True)
        time.sleep(hold)
        self.key(qcode, False)
        if pause:
            time.sleep(pause)

    def chord(self, modifier: str, qcode: str) -> None:
        self.key(modifier, True)
        time.sleep(0.15)
        self.key(qcode, True)
        time.sleep(0.05)
        self.key(qcode, False)
        time.sleep(0.15)
        self.key(modifier, False)
        time.sleep(0.40)

    def relative(self, dx: int, dy: int) -> None:
        self.execute(
            "input-send-event",
            {
                "events": [
                    {"type": "rel", "data": {"axis": "x", "value": dx}},
                    {"type": "rel", "data": {"axis": "y", "value": dy}},
                ]
            },
        )

    def absolute(self, x: int, y: int) -> None:
        """Place the pointer at an absolute device position.

        The values are in the device's own coordinate space, not screen pixels:
        virtio-tablet publishes 0..32767 for both axes. Only a device with an
        absolute handler takes these, so this is virtio-only.
        """
        self.execute(
            "input-send-event",
            {
                "events": [
                    {"type": "abs", "data": {"axis": "x", "value": x}},
                    {"type": "abs", "data": {"axis": "y", "value": y}},
                ]
            },
        )

    def button(self, name: str, down: bool) -> None:
        self.execute(
            "input-send-event",
            {
                "events": [
                    {"type": "btn", "data": {"down": down, "button": name}}
                ]
            },
        )

    def screenshot(self, path: Path) -> None:
        self.execute("screendump", {"filename": str(path)})


def send_kbd_smoke_actions(client: QmpClient) -> None:
    """Send the key sequence expected by kbdtest --selftest."""
    client.tap("a")
    client.chord("shift", "a")
    client.chord("ctrl", "c")
    client.tap("right")
    client.tap("ret")


# The range virtio-tablet publishes: 0 at one corner, 32767 at the other, and
# 16383/16384 right in the middle. The guest expects the middle of the range to
# land on the middle of its screen and 32767 on the opposite corner.
_POINTER_RANGE_LAST = 32767
_POINTER_RANGE_MIDDLE = 16384


def send_pointer_smoke_actions(client: QmpClient) -> None:
    """Send the pointer sequence expected by mousetest --selftest.

    Absolute moves and one click, in the order the guest asserts them: centre,
    button, opposite corner. The second absolute move is the one that matters
    most -- a consumer that accumulated positions instead of assigning them
    would land off-screen rather than in the corner.

    No wheel tick on purpose: this QMP only accepts the 'x' and 'y' rel axes, so
    a REL_WHEEL cannot be asked for over it (hmp `mouse_move 0 0 1` can, and
    that monitor belongs to the runner, not to a driver). The wheel travels the
    same notify, so it is not what this smoke is measuring.
    """
    client.absolute(_POINTER_RANGE_MIDDLE, _POINTER_RANGE_MIDDLE)
    time.sleep(0.30)
    client.button("left", True)
    time.sleep(0.20)
    client.button("left", False)
    time.sleep(0.20)
    client.absolute(_POINTER_RANGE_LAST, _POINTER_RANGE_LAST)
    time.sleep(0.30)
