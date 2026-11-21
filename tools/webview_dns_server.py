#!/usr/bin/env python3
"""DNS fixture for the Web Viewer visual scenario.

Serves the zone of tools/dns_fixture.py (test.savanxp is the host, 10.0.2.2
through slirp; unknown names are NXDOMAIN) over UDP and TCP on one ephemeral
loopback port, and prints READY <port>: the guest is pointed at it with
`webview --dns 10.0.2.2:<port>`, since slirp's own 10.0.2.3 cannot be
redirected to a fixture. Logging goes to stderr so the READY line stays alone
on stdout.
"""

import argparse
import threading

from dns_fixture import DnsFixture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--address", default="10.0.2.2")
    args = parser.parse_args()
    fixture = DnsFixture(address=args.address, bind=args.bind).start()
    print("READY %d" % fixture.port, flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
