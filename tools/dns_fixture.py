#!/usr/bin/env python3
"""A deterministic DNS server for the guest's resolver tests.

Serves one fixed zone over UDP and TCP on the same port number, so that a
truncated UDP reply can be asked again over TCP as RFC 1035 says. Each name
under `savanxp` exercises one path of include/sxn/sxndns.h:

  test.savanxp       A <address>, TTL 60
  alias.savanxp      CNAME test.savanxp and its A, in one reply
  chain.savanxp      CNAME alias.savanxp alone: the client asks again
  loop1/loop2        CNAMEs of each other, one per reply
  short.savanxp      A 10.0.2.9 with a TTL of one second
  big.savanxp        truncated over UDP; over TCP A 10.0.2.10, sent in pieces
  spoof.savanxp      a forged A 10.0.2.66 from another port, then the real
                     A 10.0.2.11 from the server's own port
  garbage.savanxp    a reply whose answer overruns the datagram, and nothing else
  servfail.savanxp   SERVFAIL
  empty.savanxp      NOERROR with no answers
  slow.savanxp       A <address>, 1.5 seconds late
  silent.savanxp     no reply at all
  anything else      NXDOMAIN

Queries are counted per name (`count`), which lets a test tell a cached
answer from one that went to the network. Only the standard library; every
request is parsed bounds-checked, like the client under test must do.
"""

from __future__ import annotations

import socket
import struct
import sys
import threading
import time

TYPE_A = 1
TYPE_CNAME = 5


def decode_name(data: bytes, offset: int) -> tuple[str, int]:
    """A question name, uncompressed (queries never compress)."""
    labels = []
    end = offset
    while True:
        if end >= len(data):
            raise ValueError("name overruns the message")
        length = data[end]
        if length == 0:
            end += 1
            break
        if length & 0xC0 or length > 63:
            raise ValueError("bad label")
        end += 1
        if end + length > len(data):
            raise ValueError("label overruns the message")
        labels.append(data[end:end + length].decode("ascii", "replace"))
        end += length
        if len(labels) > 64:
            raise ValueError("too many labels")
    return ".".join(labels).lower(), end


def encode_name(name: str) -> bytes:
    out = b""
    for label in name.split("."):
        out += bytes([len(label)]) + label.encode("ascii")
    return out + b"\x00"


def record(owner: bytes, rtype: int, ttl: int, data: bytes) -> bytes:
    return owner + struct.pack(">HHIH", rtype, 1, ttl, len(data)) + data


class Zone:
    def __init__(self, address: str) -> None:
        self.address = address
        self.counts: dict[str, int] = {}
        self.lock = threading.Lock()

    def count(self, name: str) -> int:
        with self.lock:
            return self.counts.get(name.lower(), 0)

    def _seen(self, name: str) -> None:
        with self.lock:
            self.counts[name] = self.counts.get(name, 0) + 1

    def answer(self, query: bytes, over_tcp: bool) -> list[tuple[float, bytes, bool]]:
        """The replies to send: (delay in seconds, bytes, from another port)."""
        if len(query) < 12:
            return []
        txid = query[0:2]
        if struct.unpack(">H", query[4:6])[0] != 1:
            return [(0.0, txid + struct.pack(">HHHHH", 0x8181, 0, 0, 0, 0), False)]
        name, question_end = decode_name(query, 12)
        if question_end + 4 > len(query):
            return []
        qtype = struct.unpack(">H", query[question_end:question_end + 2])[0]
        question = query[12:question_end + 4]
        self._seen(name)

        def reply(rcode: int = 0, answers: list[bytes] | None = None, truncated: bool = False) -> bytes:
            answers = answers or []
            flags = 0x8180 | (0x0200 if truncated else 0) | (rcode & 0x0F)
            header = txid + struct.pack(">HHHHH", flags, 1, len(answers), 0, 0)
            return header + question + b"".join(answers)

        here = struct.pack(">H", 0xC00C)

        def a(owner: bytes, ttl: int, address: str) -> bytes:
            return record(owner, TYPE_A, ttl, socket.inet_aton(address))

        def cname(owner: bytes, ttl: int, target: str) -> bytes:
            return record(owner, TYPE_CNAME, ttl, encode_name(target))

        if qtype != TYPE_A:
            return [(0.0, reply(), False)]
        if name == "test.savanxp":
            return [(0.0, reply(0, [a(here, 60, self.address)]), False)]
        if name == "alias.savanxp":
            return [(0.0, reply(0, [cname(here, 60, "test.savanxp"),
                                    a(encode_name("test.savanxp"), 60, self.address)]), False)]
        if name == "chain.savanxp":
            return [(0.0, reply(0, [cname(here, 60, "alias.savanxp")]), False)]
        if name == "loop1.savanxp":
            return [(0.0, reply(0, [cname(here, 60, "loop2.savanxp")]), False)]
        if name == "loop2.savanxp":
            return [(0.0, reply(0, [cname(here, 60, "loop1.savanxp")]), False)]
        if name == "short.savanxp":
            return [(0.0, reply(0, [a(here, 1, "10.0.2.9")]), False)]
        if name == "big.savanxp":
            if not over_tcp:
                return [(0.0, reply(truncated=True), False)]
            return [(0.0, reply(0, [a(here, 60, "10.0.2.10")]), False)]
        if name == "spoof.savanxp":
            return [(0.0, reply(0, [a(here, 60, "10.0.2.66")]), True),
                    (0.2, reply(0, [a(here, 60, "10.0.2.11")]), False)]
        if name == "garbage.savanxp":
            bad = a(here, 60, "10.0.2.12")
            bad = bad[:-6] + struct.pack(">H", 200) + bad[-4:]
            return [(0.0, reply(0, [bad]), False)]
        if name == "servfail.savanxp":
            return [(0.0, reply(2), False)]
        if name == "empty.savanxp":
            return [(0.0, reply(), False)]
        if name == "slow.savanxp":
            return [(1.5, reply(0, [a(here, 60, self.address)]), False)]
        if name == "silent.savanxp":
            return []
        return [(0.0, reply(3), False)]


class DnsFixture:
    """The zone on 127.0.0.1, UDP and TCP on one port number."""

    def __init__(self, address: str = "10.0.2.2", bind: str = "127.0.0.1") -> None:
        self.zone = Zone(address)
        self.bind = bind
        self.udp, self.tcp = self._open(bind)
        self.port = self.udp.getsockname()[1]
        self.forger = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.forger.bind((bind, 0))

    @staticmethod
    def _open(bind: str) -> tuple[socket.socket, socket.socket]:
        for _ in range(64):
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.bind((bind, 0))
            tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                tcp.bind((bind, udp.getsockname()[1]))
            except OSError:
                udp.close()
                tcp.close()
                continue
            tcp.listen(16)
            return udp, tcp
        raise OSError("no port free for both UDP and TCP")

    def start(self) -> "DnsFixture":
        threading.Thread(target=self._serve_udp, daemon=True).start()
        threading.Thread(target=self._serve_tcp, daemon=True).start()
        return self

    def _send_udp(self, data: bytes, peer: tuple[str, int], forged: bool) -> None:
        try:
            (self.forger if forged else self.udp).sendto(data, peer)
        except OSError:
            pass

    def _serve_udp(self) -> None:
        while True:
            try:
                data, peer = self.udp.recvfrom(512)
            except OSError:
                return
            try:
                replies = self.zone.answer(data, over_tcp=False)
            except (ValueError, struct.error) as exc:
                print("dns fixture: dropping a bad query (%s)" % exc, file=sys.stderr)
                continue
            for delay, reply, forged in replies:
                if delay <= 0:
                    self._send_udp(reply, peer, forged)
                else:
                    timer = threading.Timer(delay, self._send_udp, (reply, peer, forged))
                    timer.daemon = True
                    timer.start()

    def _serve_tcp(self) -> None:
        while True:
            try:
                connection, _ = self.tcp.accept()
            except OSError:
                return
            threading.Thread(target=self._serve_connection, args=(connection,), daemon=True).start()

    def _serve_connection(self, connection: socket.socket) -> None:
        with connection:
            connection.settimeout(10.0)
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                buffer = b""
                while len(buffer) < 2:
                    chunk = connection.recv(512)
                    if not chunk:
                        return
                    buffer += chunk
                size = struct.unpack(">H", buffer[:2])[0]
                while len(buffer) < size + 2:
                    chunk = connection.recv(512)
                    if not chunk:
                        return
                    buffer += chunk
                replies = self.zone.answer(buffer[2:size + 2], over_tcp=True)
                for delay, reply, _forged in replies:
                    time.sleep(delay)
                    framed = struct.pack(">H", len(reply)) + reply
                    # In pieces, the length prefix split too: the client
                    # must assemble the frame, not expect one read.
                    for piece in (framed[:1], framed[1:2], framed[2:9], framed[9:]):
                        connection.sendall(piece)
                        time.sleep(0.05)
            except (OSError, ValueError, struct.error):
                return
