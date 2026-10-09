#!/usr/bin/env python3

import socket
import sys
import time


BODY_LEN = 4096
HEADER = (
    b"HTTP/1.0 200 OK\r\n"
    b"Content-Length: 4096\r\n"
    b"Connection: close\r\n"
    b"\r\n"
)
BODY = bytes(ord("A") + (i % 26) for i in range(BODY_LEN))
RESPONSE = HEADER + BODY


def serve(port: int) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("0.0.0.0", port))
        listener.listen(4)
        print(f"TCP test server listening on 0.0.0.0:{port}", file=sys.stderr)

        while True:
            conn, addr = listener.accept()
            with conn:
                print(f"connection from {addr[0]}:{addr[1]}", file=sys.stderr)
                request = bytearray()
                while b"\r\n\r\n" not in request and len(request) < 4096:
                    chunk = conn.recv(512)
                    if not chunk:
                        break
                    request.extend(chunk)

                if not request.startswith(b"GET /xv6 HTTP/1.0\r\n"):
                    print("unexpected request", file=sys.stderr)
                    continue

                # Uneven chunks exercise packet boundaries and short reads.
                offset = 0
                for size in (73, 509, 1021, 149, 1400):
                    if offset >= len(RESPONSE):
                        break
                    end = min(offset + size, len(RESPONSE))
                    conn.sendall(RESPONSE[offset:end])
                    offset = end
                    time.sleep(0.01)
                if offset < len(RESPONSE):
                    conn.sendall(RESPONSE[offset:])
                conn.shutdown(socket.SHUT_WR)
                print(f"sent {len(RESPONSE)} bytes", file=sys.stderr)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} PORT")
    serve(int(sys.argv[1]))


if __name__ == "__main__":
    main()
