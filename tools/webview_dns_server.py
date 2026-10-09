#!/usr/bin/env python3
"""Fake DNS fixture for the Web Viewer visual scenario.

Answers A queries for test.savanxp with 10.0.2.2 (the slirp gateway, which is
how the guest reaches this host) and NXDOMAIN for anything else. Binds an
ephemeral port on loopback and prints READY <port>: the guest is pointed at it
with `webview --dns 10.0.2.2:<port>`, since slirp's own 10.0.2.3 cannot be
redirected to a fixture.

Only the standard library. Requests are parsed bounds-checked, like the
client under test must do; logging goes to stderr so the READY line stays
alone on stdout.
"""

import argparse
import socket
import struct
import sys


def decode_name(data, offset):
    """QNAME without compression (queries never compress). Returns (name, end)."""
    labels = []
    end = offset
    while True:
        if end >= len(data):
            raise ValueError("name overruns datagram")
        length = data[end]
        if length == 0:
            end += 1
            break
        if length & 0xC0 or length > 63:
            raise ValueError("bad label")
        end += 1
        if end + length > len(data):
            raise ValueError("label overruns datagram")
        labels.append(data[end:end + length].decode("ascii", "replace"))
        end += length
        if len(labels) > 16:
            raise ValueError("too many labels")
    return ".".join(labels).rstrip(".").lower(), end


def build_reply(query, rcode, address):
    txid = query[0:2]
    qdcount = struct.unpack(">H", query[4:6])[0]
    flags = 0x8180 | (rcode & 0x0F)
    if qdcount != 1:
        return txid + struct.pack(">HHHHH", flags, 0, 0, 0, 0)
    try:
        _, question_end = decode_name(query, 12)
    except ValueError:
        return None
    if question_end + 4 > len(query):
        return None
    question = query[12:question_end + 4]
    if rcode != 0 or address is None:
        return txid + struct.pack(">HHHHH", flags, 1, 0, 0, 0) + question
    answer = struct.pack(">H", 0xC00C)
    answer += struct.pack(">HHIH", 1, 1, 60, 4)
    answer += socket.inet_aton(address)
    return txid + struct.pack(">HHHHH", flags, 1, 1, 0, 0) + question + answer


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--name", default="test.savanxp")
    parser.add_argument("--address", default="10.0.2.2")
    args = parser.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, 0))
    print("READY %d" % sock.getsockname()[1], flush=True)
    while True:
        try:
            data, peer = sock.recvfrom(512)
        except OSError:
            break
        try:
            if len(data) < 12:
                continue
            qdcount = struct.unpack(">H", data[4:6])[0]
            if qdcount != 1:
                reply = build_reply(data, 1, None)
            else:
                name, _ = decode_name(data, 12)
                qtype_offset = None
                try:
                    _, qend = decode_name(data, 12)
                    qtype_offset = qend
                except ValueError:
                    qtype_offset = None
                qtype = struct.unpack(">H", data[qtype_offset:qtype_offset + 2])[0] \
                    if qtype_offset is not None and qtype_offset + 4 <= len(data) else 0
                if name == args.name and qtype == 1:
                    reply = build_reply(data, 0, args.address)
                else:
                    reply = build_reply(data, 3, None)
            if reply is not None:
                sock.sendto(reply, peer)
        except (ValueError, struct.error, OSError) as exc:
            print("dns fixture: dropping a bad query (%s)" % exc, file=sys.stderr)


if __name__ == "__main__":
    main()
