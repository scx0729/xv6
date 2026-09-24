#!/usr/bin/env python3

import socket
import sys
import time


port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", port))

while True:
    data, client = sock.recvfrom(4096)
    if data == b"START":
        break

# Send READY more than once so an isolated UDP loss cannot stall the test.
for _ in range(3):
    sock.sendto(b"READY", client)

sock.setblocking(False)
deadline = time.monotonic() + 4.0
progress = False

while time.monotonic() < deadline:
    for _ in range(16):
        try:
            sock.sendto(b"FLOOD", client)
        except BlockingIOError:
            break

    while True:
        try:
            data, source = sock.recvfrom(4096)
        except BlockingIOError:
            break
        if source == client and data == b"PROGRESS":
            progress = True

    time.sleep(0.001)

result = b"PASS" if progress else b"FAIL"
for _ in range(100):
    try:
        sock.sendto(result, client)
    except BlockingIOError:
        pass
    time.sleep(0.01)
