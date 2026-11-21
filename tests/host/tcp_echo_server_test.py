#!/usr/bin/env python3
"""Protocol test for the Linux TCP smoke helper."""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path


def pattern(count: int) -> bytes:
    return bytes((index * 31 + 7) & 0xFF for index in range(count))


def read_line(client: socket.socket) -> str:
    line = bytearray()
    while not line.endswith(b"\n"):
        chunk = client.recv(1)
        if not chunk or len(line) > 16:
            raise AssertionError("Invalid line from the echo server")
        line.extend(chunk)
    return line.decode("ascii").strip()


def dns_query(name: str, txid: int) -> bytes:
    qname = b"".join(bytes([len(label)]) + label.encode("ascii") for label in name.split("."))
    return struct.pack(">HHHHHH", txid, 0x0100, 1, 0, 0, 0) + qname + b"\x00\x00\x01\x00\x01"


def check_dns_fixture(port: int) -> None:
    """The resolver's fixture: its port, UDP answers, truncation, TCP in pieces."""
    with socket.create_connection(("127.0.0.1", port), timeout=5.0) as control:
        control.settimeout(5.0)
        control.sendall(b"DNS\n")
        dns_port = int(read_line(control))
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(5.0)
            udp.sendto(dns_query("test.savanxp", 0x1234), ("127.0.0.1", dns_port))
            reply, source = udp.recvfrom(512)
            if source[1] != dns_port or reply[:2] != b"\x12\x34" or reply[-4:] != socket.inet_aton("10.0.2.2"):
                raise AssertionError("test.savanxp did not answer 10.0.2.2 from the server port")
            udp.sendto(dns_query("big.savanxp", 0x4321), ("127.0.0.1", dns_port))
            reply, _ = udp.recvfrom(512)
            if not reply[2] & 0x02 or struct.unpack(">H", reply[6:8])[0] != 0:
                raise AssertionError("big.savanxp was not truncated over UDP")
            udp.sendto(dns_query("nosuch.savanxp", 0x5555), ("127.0.0.1", dns_port))
            reply, _ = udp.recvfrom(512)
            if reply[3] & 0x0F != 3:
                raise AssertionError("an unknown name was not NXDOMAIN")
        with socket.create_connection(("127.0.0.1", dns_port), timeout=5.0) as tcp:
            query = dns_query("big.savanxp", 0x7777)
            tcp.sendall(struct.pack(">H", len(query)) + query)
            framed = b""
            pieces = 0
            while len(framed) < 2 or len(framed) < struct.unpack(">H", framed[:2])[0] + 2:
                chunk = tcp.recv(4096)
                if not chunk:
                    raise AssertionError("the TCP reply ended early")
                framed += chunk
                pieces += 1
            if framed[-4:] != socket.inet_aton("10.0.2.10") or pieces < 2:
                raise AssertionError("big.savanxp over TCP was not 10.0.2.10 in pieces")
        control.sendall(b"DNSCOUNT big.savanxp\n")
        if read_line(control) != "2":
            raise AssertionError("DNSCOUNT did not count both questions about big.savanxp")


def check_http_fixture(port: int) -> None:
    """The loader's fixture: its port, a chunked body in pieces, a redirect."""
    with socket.create_connection(("127.0.0.1", port), timeout=5.0) as control:
        control.settimeout(5.0)
        control.sendall(b"HTTP\n")
        http_port = int(read_line(control))
    def get(path: str) -> tuple[bytes, int]:
        with socket.create_connection(("127.0.0.1", http_port), timeout=5.0) as client:
            client.settimeout(5.0)
            client.sendall(b"GET " + path.encode("ascii") + b" HTTP/1.1\r\nHost: x\r\n\r\n")
            data = b""
            reads = 0
            while True:
                chunk = client.recv(4096)
                if not chunk:
                    return data, reads
                data += chunk
                reads += 1
    data, reads = get("/chunked")
    if b"Transfer-Encoding: chunked" not in data or b"chunked \r\n4\r\nbody" not in data or reads < 4:
        raise AssertionError("/chunked was not a chunked body sent in pieces")
    data, _ = get("/redirect-relative")
    if not data.startswith(b"HTTP/1.1 302") or b"Location: length?from=redirect" not in data:
        raise AssertionError("/redirect-relative did not redirect")
    data, _ = get("/echo")
    if b"GET /echo HTTP/1.1" not in data:
        raise AssertionError("/echo did not echo the request")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="savanxp-tcp-") as directory:
        root = Path(directory)
        port_file = root / "port"
        log_file = root / "server.log"
        with log_file.open("wb") as log:
            process = subprocess.Popen(
                [str(args.server), "--port", "0", "--port-file", str(port_file)],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 5.0
                while time.monotonic() < deadline and not port_file.is_file():
                    if process.poll() is not None:
                        raise RuntimeError("TCP echo server exited before publishing a port")
                    time.sleep(0.02)
                if not port_file.is_file():
                    raise RuntimeError("TCP echo server did not publish a port")
                port = int(port_file.read_text(encoding="ascii").strip())
                with socket.create_connection(("127.0.0.1", port), timeout=5.0) as client:
                    client.settimeout(5.0)
                    client.sendall(b"REFUSE\n")
                    response = bytearray()
                    while not response.endswith(b"\n"):
                        chunk = client.recv(1)
                        if not chunk or len(response) >= 6:
                            raise AssertionError("Invalid REFUSE port response")
                        response.extend(chunk)
                    refused_port = int(response)
                    try:
                        refused = socket.create_connection(("127.0.0.1", refused_port), timeout=1.0)
                    except ConnectionRefusedError:
                        pass
                    else:
                        refused.close()
                        raise AssertionError("Reserved non-listening port accepted a connection")
                    client.sendall(b"BULK 5000\n")
                    received = b""
                    while len(received) < 5000:
                        received += client.recv(5000 - len(received))
                    if received != pattern(5000):
                        raise AssertionError("BULK pattern mismatch")

                    payload = pattern(9000)
                    client.sendall(b"ECHO 9000\n" + payload)
                    echoed = b""
                    while len(echoed) < 9000:
                        echoed += client.recv(9000 - len(echoed))
                    if echoed != payload:
                        raise AssertionError("ECHO payload mismatch")

                    client.sendall(b"DELAY 10 300\n")
                    delayed = b""
                    while len(delayed) < 300:
                        delayed += client.recv(300 - len(delayed))
                    if delayed != pattern(300):
                        raise AssertionError("DELAY pattern mismatch")
                    client.sendall(b"RESET\n")
                    try:
                        client.recv(1)
                    except ConnectionResetError:
                        pass
                    else:
                        raise AssertionError("RESET did not reset the established connection")
                check_dns_fixture(port)
                check_http_fixture(port)
                # One idle client must not prevent another connection's work.
                with socket.create_connection(("127.0.0.1", port), timeout=5.0) as idle:
                    with socket.create_connection(("127.0.0.1", port), timeout=5.0) as writer:
                        writer.sendall(b"SAVE host 64\n" + pattern(64))
                        writer.shutdown(socket.SHUT_WR)
                        if writer.recv(1) != b"":
                            raise AssertionError("SAVE did not finish at orderly EOF")
                    idle.sendall(b"SAVED host\n")
                    received = bytearray()
                    while len(received) < 64:
                        chunk = idle.recv(64 - len(received))
                        if not chunk:
                            raise AssertionError("SAVED truncated")
                        received.extend(chunk)
                    if received != pattern(64):
                        raise AssertionError("SAVE lost data before EOF")
            finally:
                process.terminate()
                process.wait(timeout=5.0)
    print("TCP echo server: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
