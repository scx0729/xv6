#!/usr/bin/env python3

import socket
import sys
import time


def exchange(port, request, expected):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(0.2)
    deadline = time.monotonic() + 12
    try:
        while time.monotonic() < deadline:
            sock.sendto(request, ("127.0.0.1", port))
            try:
                reply, _ = sock.recvfrom(2048)
            except socket.timeout:
                continue
            if reply == expected:
                return
            raise RuntimeError("unexpected UDP reply: %r" % (reply,))
    finally:
        sock.close()
    raise RuntimeError("timed out waiting for %r" % (expected,))


def main():
    port = int(sys.argv[1])
    exchange(port, b"udp-one", b"reply-one")
    exchange(port, b"udp-two", b"reply-two")
    print("UDP SERVER CLIENT PASSED")


if __name__ == "__main__":
    main()
