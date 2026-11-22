#!/usr/bin/env python3
"""A TLS server for the guest's https:// tests (sxn/sxnhttp.h, docs/net/HTTP.md).

At start it makes a throwaway PKI with the host's `openssl` command (found on
PATH, or $SAVANXP_HOST_OPENSSL; 3.4 or newer, for -not_before/-not_after): a
test root, an intermediate under it, and one server certificate per case.
The root is the only thing the guest trusts, and it never reaches an image:
the HTTP fixture serves it at /tls-root.pem and the test hands it to
sxnhttp in memory (sxnhttp_request.trust_pem).

One port; the certificate is chosen by the SNI name, every name resolving to
the host through tools/dns_fixture.py (*.tls.savanxp):

  good.tls.savanxp        leaf + intermediate: valid
  wrongname.tls.savanxp   good's certificate, for good.tls.savanxp
  expired.tls.savanxp     valid 2020-01-01 to 2021-01-01
  notyet.tls.savanxp      valid from 2090-01-01
  selfsigned.tls.savanxp  a self-signed leaf
  nochain.tls.savanxp     leaf without the intermediate it needs
  rogue.tls.savanxp       leaf from a CA the guest does not trust
  (an address)            a leaf for the address 10.0.2.2

and a second port, `old_port`, where a valid old.tls.savanxp is served by a
server that speaks TLS 1.1 at most (the protocol cannot change after SNI).

Every successful handshake answers one HTTP/1.1 response whose body says the
name, the protocol and the ALPN the server agreed:
"secure <name> <TLSv1.3> <http/1.1>".
"""

from __future__ import annotations

import os
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import warnings
from pathlib import Path

# Valid from two days ago: QEMU starts the guest's RTC at the host's local
# time, which a UTC-only guest reads as UTC (docs/kernel/TIME.md), so a
# certificate valid from "now" would not be valid yet behind UTC.
VALID = ["-not_before", time.strftime("%Y%m%d%H%M%SZ", time.gmtime(time.time() - 2 * 86400)),
         "-not_after", time.strftime("%Y%m%d%H%M%SZ", time.gmtime(time.time() + 30 * 86400))]

NAMES = ("good", "wrongname", "expired", "notyet", "selfsigned", "nochain", "rogue")


class TlsFixtureError(RuntimeError):
    pass


def _openssl() -> str:
    tool = os.environ.get("SAVANXP_HOST_OPENSSL") or shutil.which("openssl")
    if not tool:
        raise TlsFixtureError("no openssl command on PATH (set SAVANXP_HOST_OPENSSL)")
    return tool


class Pki:
    """The certificates, in a temporary directory that lives with the fixture."""

    def __init__(self) -> None:
        self.tool = _openssl()
        self.dir = Path(tempfile.mkdtemp(prefix="savanxp-tls-"))
        self.serial = 0
        self._ca("root", "SavanXP Test Root")
        self._ca("rogue-root", "Untrusted Test Root")
        self._ca("inter", "SavanXP Test Intermediate", issuer="root")
        self._leaf("good", "DNS:good.tls.savanxp", "inter")
        self._leaf("expired", "DNS:expired.tls.savanxp", "inter", "20200101000000Z", "20210101000000Z")
        self._leaf("notyet", "DNS:notyet.tls.savanxp", "inter", "20900101000000Z", "20910101000000Z")
        self._leaf("nochain", "DNS:nochain.tls.savanxp", "inter")
        self._leaf("rogue", "DNS:rogue.tls.savanxp", "rogue-root")
        self._leaf("old", "DNS:old.tls.savanxp", "inter")
        self._leaf("ip", "IP:10.0.2.2", "inter")
        self._self_signed("selfsigned", "DNS:selfsigned.tls.savanxp")
        self.root_pem = (self.dir / "root.pem").read_bytes()

    def _run(self, *args: str) -> None:
        result = subprocess.run([self.tool, *args], cwd=self.dir, capture_output=True, text=True)
        if result.returncode != 0:
            raise TlsFixtureError(f"openssl {args[0]} failed: {result.stderr.strip()}")

    def _next_serial(self) -> str:
        self.serial += 1
        return str(self.serial)

    def _key(self, name: str) -> None:
        self._run("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", f"{name}.key")

    def _ca(self, name: str, common: str, issuer: str | None = None) -> None:
        self._key(name)
        extensions = self.dir / f"{name}.ext"
        extensions.write_text("basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n"
                              "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n", encoding="ascii")
        if issuer is None:
            self._run("req", "-x509", "-key", f"{name}.key", "-subj", f"/CN={common}", *VALID,
                      "-addext", "basicConstraints=critical,CA:TRUE",
                      "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-out", f"{name}.pem")
            return
        self._run("req", "-new", "-key", f"{name}.key", "-subj", f"/CN={common}", "-out", f"{name}.csr")
        self._run("x509", "-req", "-in", f"{name}.csr", "-CA", f"{issuer}.pem", "-CAkey", f"{issuer}.key",
                  "-set_serial", self._next_serial(), *VALID,
                  "-extfile", extensions.name, "-out", f"{name}.pem")

    def _leaf(self, name: str, san: str, issuer: str, not_before: str = "", not_after: str = "") -> None:
        self._key(name)
        extensions = self.dir / f"{name}.ext"
        extensions.write_text(f"basicConstraints=critical,CA:FALSE\nsubjectAltName={san}\n"
                              "keyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n"
                              "subjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid:always\n",
                              encoding="ascii")
        self._run("req", "-new", "-key", f"{name}.key", "-subj", f"/CN={name}", "-out", f"{name}.csr")
        dates = ["-not_before", not_before, "-not_after", not_after] if not_before else VALID
        self._run("x509", "-req", "-in", f"{name}.csr", "-CA", f"{issuer}.pem", "-CAkey", f"{issuer}.key",
                  "-set_serial", self._next_serial(), *dates,
                  "-extfile", extensions.name, "-out", f"{name}.pem")

    def _self_signed(self, name: str, san: str) -> None:
        self._key(name)
        self._run("req", "-x509", "-key", f"{name}.key", "-subj", f"/CN={name}", *VALID,
                  "-addext", f"subjectAltName={san}", "-addext", "basicConstraints=critical,CA:FALSE",
                  "-out", f"{name}.pem")

    def chain(self, name: str, with_intermediate: bool = True) -> Path:
        """The leaf, then the intermediate when asked, as one file."""
        path = self.dir / f"{name}.chain.pem"
        parts = [(self.dir / f"{name}.pem").read_bytes()]
        if with_intermediate:
            parts.append((self.dir / "inter.pem").read_bytes())
        path.write_bytes(b"".join(parts))
        return path

    def close(self) -> None:
        shutil.rmtree(self.dir, ignore_errors=True)


class TlsFixture:
    def __init__(self, bind: str = "127.0.0.1") -> None:
        self.pki = Pki()
        self.contexts: dict[str, ssl.SSLContext] = {}
        for name in NAMES:
            cert = {"wrongname": "good", "selfsigned": "selfsigned"}.get(name, name)
            with_chain = name not in ("nochain", "selfsigned", "rogue")
            self.contexts[f"{name}.tls.savanxp"] = self._context(cert, with_chain)
        self.default = self._context("ip", True)
        self.default.sni_callback = self._pick
        self.names: dict[int, str] = {}
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind((bind, 0))
        self.server.listen(32)
        self.port = self.server.getsockname()[1]
        self.old = self._context("old", True, old=True)
        self.old_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.old_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.old_server.bind((bind, 0))
        self.old_server.listen(8)
        self.old_port = self.old_server.getsockname()[1]

    def _context(self, cert: str, with_chain: bool, old: bool = False) -> ssl.SSLContext:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.pki.chain(cert, with_chain), self.pki.dir / f"{cert}.key")
        context.set_alpn_protocols(["http/1.1"])
        if old:
            context.set_ciphers("DEFAULT:@SECLEVEL=0")
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", DeprecationWarning)
                context.minimum_version = ssl.TLSVersion.TLSv1
                context.maximum_version = ssl.TLSVersion.TLSv1_1
        return context

    def _pick(self, connection: ssl.SSLObject, name: str | None, _context: ssl.SSLContext) -> None:
        if name is not None:
            self.names[id(connection)] = name
            if name in self.contexts:
                connection.context = self.contexts[name]

    def start(self) -> "TlsFixture":
        threading.Thread(target=self._serve, args=(self.server, self.default), daemon=True).start()
        threading.Thread(target=self._serve, args=(self.old_server, self.old), daemon=True).start()
        return self

    def _serve(self, server: socket.socket, context: ssl.SSLContext) -> None:
        while True:
            try:
                connection, _ = server.accept()
            except OSError:
                return
            threading.Thread(target=self._handle, args=(connection, context), daemon=True).start()

    def _handle(self, raw: socket.socket, context: ssl.SSLContext) -> None:
        raw.settimeout(20.0)
        try:
            with context.wrap_socket(raw, server_side=True) as connection:
                request = b""
                while b"\r\n\r\n" not in request and len(request) < 8192:
                    chunk = connection.recv(1024)
                    if not chunk:
                        return
                    request += chunk
                name = self.names.pop(id(connection), "10.0.2.2")
                body = f"secure {name} {connection.version()} {connection.selected_alpn_protocol()}".encode()
                connection.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                                   + b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body)
        except (OSError, ssl.SSLError, ValueError):
            return
