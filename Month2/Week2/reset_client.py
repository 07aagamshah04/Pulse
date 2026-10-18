#!/usr/bin/env python3
"""
MONTH 2 / WEEK 2 — the "bad client" test for serve_connection() and SIGPIPE.

Sends a valid GET /slow, waits half a second (so Pulse's worker is now
asleep inside the /slow handler), then closes the connection with a TCP
RESET instead of a normal close. 5 seconds later, when /slow wakes up and
Pulse tries to write() its response, the client is gone:

  - write() fails (ECONNRESET / EPIPE) -> write_all() throws via throw_errno()
  - without Week 2's changes: SIGPIPE could kill the whole process, or the
    exception escaping the worker thread would std::terminate() it
  - with Week 2's changes: SIGPIPE is ignored (main.cpp) and the exception
    is caught in TcpServer::serve_connection(), which logs
    "connection error: ..." — and the server keeps running for everyone else

How the RESET is forced: SO_LINGER with a timeout of 0 tells the OS
"on close(), don't do the normal polite FIN handshake — throw the
connection away immediately and send RST".

Usage:
    python3 Month2/Week2/reset_client.py [port]
"""
import socket
import struct
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

s = socket.create_connection(("localhost", port))
s.send(b"GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n")
print("Sent GET /slow. Waiting 0.5 s so Pulse's worker is inside the slow handler...")
time.sleep(0.5)

# struct.pack("ii", 1, 0) = the C struct linger { l_onoff = 1, l_linger = 0 }
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()
print("Connection RESET. In ~5 s, Pulse's worker will try to write its response to a client that's gone.")
print("Watch Pulse's terminal for '[TID n] connection error: ...' — and check the server is still alive.")
