# HTTP: the SUN loader (`sxnhttp`)

`include/sxn/sxnhttp.h` loads `http://` URLs for SUN programs. It is the N4
stage of [WEBVIEWER_ROADMAP.md](../roadmaps/open/WEBVIEWER_ROADMAP.md): URL
handling and HTTP/1.1 taken out of Web Viewer's renderer into a library any
native program links. This document pins what it does, what it refuses, and
why. Names resolve through [`sxndns`](NETWORKING.md#name-resolution-sxndns);
the transport is SUN's TCP sockets ([NETWORKING.md](NETWORKING.md)).

## Two halves

- **The pure half**, `include/sxn/sxnhttp_message.h` and
  `system/runtime/sxnhttp_message.c`: URL parsing, formatting and resolution,
  and the response parser. No system calls, so the host test
  `sxnhttp-message-test` compiles it as it is.
- **The loader**, `include/sxn/sxnhttp.h` and `system/runtime/sxnhttp.c`:
  sockets, budgets, cancellation and redirects around the parser.

Both are in the SUN runtime (`system/runtime/units.txt`, group `core`) and
the SDK; a program that does not call them does not carry them.

## The contract

```c
struct sxnhttp_client client;          /* caller-owned, ~17 KiB, one thread */
sxnhttp_client_init(&client, &resolver);
struct sxnhttp_request request = {.url = "http://host:8080/page", .cancel = event};
struct sxnhttp_response response = {.body = buffer, .body_capacity = sizeof buffer};
long result = sxnhttp_get(&client, &request, &response);
```

On success (`0`) the response carries the **final URL** after redirects, the
**status**, the **content type**, the **body** and how many redirects were
followed. A 4xx or 5xx is a success: an error page is a page. On failure the
result is one `enum sxnhttp_error`, and `sxnhttp_error_text` turns it into a
sentence; a DNS failure also keeps the resolver's own result in `dns_error`.
`progress`, when set, is called on the loading thread at each phase
(resolving, connecting, securing, waiting, receiving with the bytes so far and
the `Content-Length` when there is one, redirected).

## What goes out

`GET <path?query> HTTP/1.1`, then `Host` (with the port when it is not 80),
`User-Agent` when the caller names one, `Accept: */*`,
`Accept-Encoding: identity` and `Connection: close`. One request per
connection: no keep-alive, no pipelining, so a body that runs to the close
is always well defined.

## What comes back, and what is refused

The parser is fed the bytes in whatever pieces TCP delivered them and decides
the same way however they were cut; the host test checks every response
whole, split at every byte and fed one byte at a time.

| Response | Result |
|---|---|
| `Content-Length`, repeated or listed with equal values | the body |
| `Transfer-Encoding: chunked`, with extensions and trailers | the decoded body |
| neither, over HTTP/1.0 or 1.1 | the body up to the close |
| 1xx before the real response (not 101), up to four | skipped |
| 204, 304, `Content-Length: 0` | no body |
| both a length and chunked; lengths that disagree; a negative, empty or 1 TiB or longer length | `BAD_FRAMING` |
| a coding other than chunked, chunked twice, chunked over HTTP/1.0 | `BAD_FRAMING` |
| a chunk size that is not hex, over eight digits, or not followed by CRLF | `BAD_RESPONSE` / `BAD_FRAMING` |
| a status line that is not `HTTP/1.0` or `HTTP/1.1` with three digits; 101; header folding; a header without a name or colon; control characters | `BAD_RESPONSE` |
| a `Content-Encoding` other than identity | `UNSUPPORTED_ENCODING` |
| the head past 16 KiB, trailers past 4 KiB | `HEADERS_TOO_LARGE` |
| the connection ending, or reset, inside a framed body or the head | `TRUNCATED` |

Nothing is guessed: a response that could be read two ways fails, because a
guess is how request smuggling and silently short pages happen. Bare LF line
ends are accepted, as old servers write them.

**When the caller provides a fixed buffer, its size caps the body.** A body
that does not fit stops the read there, keeps what fits, and the response
says `SXNHTTP_RESPONSE_CUT`. That is a deliberate limit, told apart from a
truncation, which is the server's failure.

## Downloads: the head and the sink

Two optional hooks in the request make the same loader carry a file of any
length (D1 of [WEBVIEWER_ROADMAP.md](../roadmaps/open/WEBVIEWER_ROADMAP.md)):

```c
request.head = on_head;  /* int on_head(void *user, const struct sxnhttp_response *head) */
request.sink = on_bytes; /* long on_bytes(void *user, const unsigned char *bytes, size_t length) */
```

- **`head`** runs once, with the final response's head (after every
  redirect, never a redirect's own), before any of its body: status, final
  URL, type, `content_length` with `SXNHTTP_RESPONSE_LENGTH`, and the
  `Content-Disposition`. Returning 1 stops the load there: `sxnhttp_get`
  returns 0 with `SXNHTTP_RESPONSE_HEAD_ONLY` and no body. That is how a
  browser asks "a page or a file?" before reading either.
- **`sink`** takes the body in the pieces it is decoded in, instead of the
  buffer, which may then be absent. A negative return stops the load with
  `SXNHTTP_ERROR_SINK`. The framing rules do not change: a body cut short is
  `TRUNCATED` after the sink has seen a prefix, so a caller writing a file
  writes it aside and keeps it only on success.
- **Budgets.** The idle budget applies between reads. By default, a sink has
  the total budget through the head and can keep reading the body afterward.
  Set `sink_keep_total` to apply the total budget through the end of the body,
  as the buffered mode does. Web Viewer sets it for pages.

A redirect's body is never read; the next hop starts as soon as its head is.

`Content-Disposition` (RFC 6266) is read, never failed on: anything but
`inline` is an attachment, an unknown type included, and the file name is
`filename*` (RFC 8187, UTF-8 only) or else `filename`, a token or a quoted
string. A malformed parameter ends the reading; what came before it stands.
The name is handed over decoded but not cleaned: `sxnhttp_file_name` turns
it, or else the URL's last path segment, or else `download`, into something
safe to save under -- no directory, no control characters, nothing a FAT
volume refuses (`< > : " | ? *` become `_`), no leading dot, no trailing dot
or space, and a long name shortened in its stem, keeping its extension and
whole UTF-8 sequences.

## Redirects

301, 302, 303, 307 and 308 with a `Location` are followed, up to five; the
sixth is `TOO_MANY_REDIRECTS`. The location is resolved against the current
URL by RFC 3986 section 5.2 (relative paths, `..`, `//host`, `?query`), and
must resolve to another `http://` or `https://` URL: a redirect to `file://`,
to any other scheme, from https to http, or without a `Location` is
`BAD_REDIRECT`. A redirect to `https://` in a program without TLS is
`UNSUPPORTED_SCHEME`.

## URLs

`sxnhttp_parse_url` takes absolute `http://` and `https://` only, lowercases
the host, applies the default port and drops the fragment. It refuses user
information (`user@host`), IPv6 literals, spaces and control characters, a
port of 0 or past 65535, and anything longer than 512 characters.

## Budgets and cancellation

All monotonic, all ending early when the cancel event is set (checked first
in every wait):

| Step | Budget |
|---|---|
| DNS | `sxndns`'s own: six seconds for the route, two attempts of two seconds |
| connect, and writing the request | 5 seconds |
| the TLS handshake, https:// | 10 seconds |
| silence between two reads | 10 seconds (`idle_ms`) |
| the whole navigation, redirects included | 30 seconds (`total_ms`) |

Every step is also cut at the total deadline.

## HTTPS

`https://` is TLS 1.2 or 1.3 through the OpenSSL port
([ports/openssl/README.md](../../ports/openssl/README.md)), validated, with
no way to accept a certificate that failed. A program gets it by linking
`system/runtime/sxnhttp_tls.c` and DEPENDS `libssl libcrypto`; `sxnhttp.c`
carries a weak `sxnhttp_tls_backend()` that has none, so every other program
answers `https://` with `UNSUPPORTED_SCHEME` and carries no OpenSSL.

The split (`system/runtime/sxnhttp_tls.h`): the TLS session never touches the
socket. `sxnhttp` moves the bytes with its own deadlines, waits and cancel
event; the session turns them into plaintext and back through memory BIOs.
OpenSSL is built without threads, so `sxnhttp_tls.c` holds one process lock
around every call into it -- none blocks, the I/O is outside.

What is checked, and the error for each:

| Check | When | Error |
|---|---|---|
| the kernel generator is seeded ([RANDOM.md](../kernel/RANDOM.md)) | before connecting | `TLS_ENTROPY` |
| the date can be believed ([TIME.md](../kernel/TIME.md#can-the-date-be-believed)) | before connecting | `TLS_CLOCK` |
| a trust store: `/system/ssl/cert.pem`, read once per process, or `trust_pem` | opening the session | `TLS_STORE` |
| the chain to a trusted root: self-signed, unknown issuer, missing intermediate, bad signature | handshake | `TLS_UNTRUSTED` |
| the name (SNI sent, no partial wildcards) or the IPv4 address (no SNI, RFC 6066) | handshake | `TLS_NAME` |
| the dates, against that clock | handshake | `TLS_DATE` |
| TLS 1.2 or later, ALPN offering only `http/1.1` and refusing another choice, alerts, records | handshake and after | `TLS_HANDSHAKE` |

The chain is checked before names and dates, as OpenSSL's verifier does: an
expired certificate from an unknown CA is `TLS_UNTRUSTED`. `tls_detail` keeps
the verifier's own words; `tls_protocol` and `tls_cipher` say what was
negotiated. A connection closed without a `close_notify` ends the stream, and
the response's framing decides: a body whose length or chunks are complete
is complete, one that is not is `TRUNCATED`; a body delimited by the close is
taken as browsers take it.

`trust_pem` replaces the system store for one request, with roots the caller
holds in memory. It exists for tests, whose CA must never be installed in an
image, and is not a way around a failure: those roots are checked exactly as
the system's are.

## Tests

- `sxnhttp-message-test` (host): RFC 3986's resolution examples, URL
  refusals, every body framing at every split point and byte by byte, each
  framing and malformation error, truncation at every cut of a good response,
  the caps, and random bytes after a valid head; the sink in pieces, its
  errors and its stop; `Content-Length` up to the 1 TiB limit;
  `Content-Disposition` well and badly formed; and `sxnhttp_file_name`.
- `native-network` (guest): `nativetest` against `tools/http_fixture.py`, which
  the echo server starts and reports through its `HTTP` command. The fixture
  sends every response in small pieces with pauses, splitting status line,
  headers, chunk-size lines and CRLFs. One path per case: each framing, the
  request it receives, the cap, each refusal, relative and chained redirects,
  names through the DNS fixture, a refused port, the total and idle budgets,
  and cancellation from another thread mid-body. Then the downloads: a
  3 MiB blob with a length, to the close, chunked in uneven chunks and as an
  attachment, each through the sink and byte-exact against the SHA-256 the
  fixture computed (`/blob.sha256`); cut short (`TRUNCATED`), refused by the
  sink, stopped at the head; the head through redirects; and a slow body
  that outlives `total_ms` as a download and not as a page.
- `https-smoke` (guest): `httpstest` against `tools/tls_fixture.py`, whose PKI
  the host's `openssl` makes at start (3.4 or newer). It fetches the test root
  over HTTP and trusts it in memory; a valid certificate loads by name and by
  address, with TLS 1.3 and `http/1.1`; wrong name, expired, not yet valid,
  self-signed, missing intermediate, an untrusted CA and a TLS 1.1 server
  each fail with their error; the system store does not trust the test root;
  a date set to 2005 is `TLS_CLOCK`, and the date is restored; and the blob,
  with a length, chunked and as an attachment, byte-exact through the sink,
  and `TRUNCATED` when cut short. Last, three threads with a client each,
  as Web Viewer's picture threads, fetch the valid page at once and all get
  it whole, through that one lock.
- `https-unseeded` (guest): with no entropy source, `TLS_ENTROPY`.

## Not here

Revocation (OCSP, CRLs), session resumption, keep-alive, compression, caching, cookies, authentication, POST,
HTTP/2 and later, IPv6. **Pending, POSIX parity:** the POSIX subsystem has no
HTTP library; ports bring their own. It is to gain one over the same pure
module when a POSIX program needs it
([AGENTS.md](../../AGENTS.md#feature-parity-between-subsystems)).
