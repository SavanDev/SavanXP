# OpenSSL port

The official SavanXP port of OpenSSL 3.5.3 and the Mozilla CA store: the
TLS library HTTPS stands on (N5 of
[WEBVIEWER_ROADMAP.md](../../docs/roadmaps/open/WEBVIEWER_ROADMAP.md)). 3.5.3
is the version the pinned Ladybird (commit `37ec6a08`) overrides to in its
`vcpkg.json`, so the browser port later finds the library it expects.

It builds two shared libraries and installs three files:

```text
/lib/libcrypto.so.0.4
/lib/libssl.so.0.4          DT_NEEDED libcrypto.so.0.4
/system/ssl/cert.pem        the Mozilla roots, OpenSSL's default CA file
```

plus their notices under `/system/licenses`. **It builds no program.** The
tree consumes the libraries as imported CMake targets (`libcrypto`, `libssl`)
when the port has been built, the way it consumes `libffmpeg`.

## Upstream, pins and license

[`UPSTREAM`](UPSTREAM) pins the release archive (size and SHA-256, the same in
the GitHub release's `.sha256` and openssl.org's) and the CA store: curl's
2026-11-05 extract of Mozilla's `certdata.txt`, size, SHA-256 and the number
of roots (118). `fetch.sh` refuses anything else; `SAVANXP_VERIFY_SIGNATURE=1`
also verifies the release signature with GPG. Nothing is vendored.

OpenSSL is Apache-2.0 ([`LICENSE.txt`](LICENSE.txt)); the certificate data
is MPL-2.0 ([`licenses/MPL-2.0.txt`](licenses/MPL-2.0.txt)). Both are in the
[provenance registry](../../docs/third-party/THIRD_PARTY_PROVENANCE.md).

## Build

```bash
./ports/openssl/build.sh            # fetch, configure, make, link, install
./ports/openssl/build.sh --no-install
./ports/openssl/build.sh --skip-port
./build.sh build                    # then the tree: ssltest and sunssltest
```

```text
fetch.sh -> configure.sh -> make.sh -> link.sh -> install.sh
```

The work tree is `build/ports/openssl/work`. Everything compiles through the
SDK's `sxcc` with its static SSE profile and `-fPIC`; the target is
`overlay/50-savanxp.conf`, given to `Configure` with `--config`.

## One library for both subsystems

The library has to serve SUN programs (`sxnhttp`, Web Viewer) and POSIX ones
(future ports) alike. Like every library here it asks the program for the C
library, and a SUN program's libc functions cannot make POSIX calls (they
are `ENOSYS` in a native process). So the library is built to ask only for
what both runtimes give and what works in both:

- memory, strings, `ctype`, `qsort`, `sscanf`, `strerror`, `atexit`;
- `time`, `gettimeofday`, `gmtime`, `nanosleep`, `getpid`, `getenv` and the
  uid calls, and `getentropy` -- which the runtime puts on the native calls
  in a SUN program ([NATIVE_HEAP.md](../../docs/userland/NATIVE_HEAP.md)).

`link.sh` checks the libraries' undefined symbols against that list and
fails the build on anything else: a file, a socket or a thread would be a
POSIX call hidden inside a SUN program.

### Configuration

`configure.sh` says why each option is there. In short: no assembly, no
threads, no sockets (the caller moves the bytes through memory BIOs), no
run-time loading, **no file system** (`no-stdio`, `OPENSSL_NO_POSIX_IO` and
the one patch), no syslog, and only TLS 1.2 and 1.3 over TCP -- no QUIC,
DTLS, SSL 3, compression, legacy or national ciphers, OCSP, CT, CMP, CMS.

Entropy: `--with-rand-seed=getrandom`, with which OpenSSL takes the
program's `getentropy`, the kernel generator in both subsystems
([RANDOM.md](../../docs/kernel/RANDOM.md)). Unseeded, it refuses: `RAND_bytes`
and even `SSL_CTX_new` fail; nothing falls back to something weaker.

### The patch

[`patches/0001-no-file-system.patch`](patches/0001-no-file-system.patch): with
the file system configured out, three places still reached it. Directory
listing uses OpenSSL's "not implemented" variant (as on UEFI), and
`RAND_load_file`, `RAND_write_file` and the `file:` store return
"unsupported". Certificates reach the library in memory: the program reads
`/system/ssl/cert.pem` through its own subsystem and hands it over
(`PEM_read_bio_X509` on a memory BIO).

## The trust store

`/system/ssl/cert.pem` is the pinned extract, unchanged. **Test CAs never go
in it**: a scenario that needs one brings it, and only into a test image.
Updating it is a change to `UPSTREAM` (date, URL, size, SHA-256, count) and
to the registry entry.

## Tests

`ssltest` (POSIX, `/bin/ssltest`) and `sunssltest` (SUN,
`/system/sunssltest.sxe`) are one source, `system/runtime/ssltest.c`. Without
any network, each checks: the version is 3.5.3; SHA-256's known answer;
`RAND_bytes` works and does not repeat; the store holds the 118 roots and
nearly all are valid by `time()`; a root verifies against the store, is
refused as self-signed without it and as expired at 2099-12-31; a TLS client
writes a ClientHello with SNI and ALPN `http/1.1`.

```bash
./build.sh smoke openssl-sun     # SUN SSL TEST PASS
./build.sh smoke openssl-posix   # SSL TEST PASS
```

Both build and install the port first. Run with no entropy source (the
machine `random-unseeded` uses), `openssl-sun` fails at `RAND_bytes` and
`SSL_CTX_new`, as it must.

## Not here

TLS against a server -- certificate chains, names, dates and their errors
-- is `sxnhttp`'s (N5d), with its fixtures. Threads, assembly, revocation
(OCSP, CRL fetching) and a CA store that updates itself are later work.
