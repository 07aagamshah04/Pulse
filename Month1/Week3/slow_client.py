#!/usr/bin/env python3
"""
Month 1 / Week 3 — the slow-client experiment from Part 2 of the Week 3 doc.

Sends a GET /hello request in two pieces, with a 1-second gap between them,
to prove that a single read() (Week 1/2's approach) is not reliably enough
to capture a whole HTTP request, and that Week 3's accumulate-until-complete
loop in tcp_server.cpp fixes it.

Usage:
    python3 slow_client.py [port]
"""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

s = socket.create_connection(("localhost", port))
s.send(b"GET /hel")
print("Sent first piece: 'GET /hel' -- sleeping 1s before sending the rest...")
time.sleep(1)
s.send(b"lo HTTP/1.1\r\nHost: localhost\r\n\r\n")
print("Sent second piece. Waiting for response...")

response = s.recv(4096)
print("--- response ---")
print(response.decode(errors="replace"))
s.close()
