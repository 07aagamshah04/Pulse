# Pulse

A raw-socket HTTP server built from scratch in C++ on Linux/POSIX, as a systems-programming learning project. See `CLAUDE.md — Pulse Month 1.md` for the project's goals and ground rules, and `Month1/` for that month's week-by-week concept docs and test logs.

## Build

```bash
mkdir -p build && cd build
cmake ..
make
```

- `cmake ..` reads `CMakeLists.txt` (the `..` = "one directory up," since you're inside `build/`) and **generates** the actual platform build files — a `Makefile`, on Linux. It does not compile anything itself; it's a translator from CMake's platform-independent instructions to your platform's real build tool.
- `make` reads that generated `Makefile` and does the real work: runs `g++` on each `.cpp` to produce object files, then links them into the `pulse` executable.
- Building inside a separate `build/` folder (instead of the project root) is called an "out-of-source build" — everything generated lands in one folder you can wipe entirely with `rm -rf build` without touching any actual source code. That's why `.gitignore` excludes it.

## Run

```bash
./build/pulse 8080
```

## API

| Method | Path | Response |
|---|---|---|
| GET | `/` | `200 OK` — `Hello from Pulse!` |
| GET | `/hello` | `200 OK` — `Hello from Pulse!` |
| GET | `/health` | `200 OK` — `OK` |
| POST | `/echo` | `200 OK` — request body, echoed back |
| any other path | | `404 Not Found` |
| registered path, wrong method | | `405 Method Not Allowed` |

```bash
curl -i http://localhost:8080/hello
curl -i http://localhost:8080/health
curl -i -X POST http://localhost:8080/echo -d "hello"
curl -i http://localhost:8080/does-not-exist     # 404
curl -i -X POST http://localhost:8080/hello      # 405 — GET-only route
```

Or talk to it raw, by hand:

```bash
nc localhost 8080
```
```http
GET /hello HTTP/1.1
Host: localhost

```

Inspect it while it's running:

```bash
ss -ltnp
ls -l /proc/<PID>/fd
```

## Architecture

```
Browser / curl
      |
      | TCP connection
      v
 socket() -> bind() -> listen()        (once, at startup — TcpServer's constructor)
      |
      v
+---- accept() -----------------------------+   <-- blocks here until a client connects
|          |                                 |
|          v                                 |
|   read() (looped, accumulating bytes       |
|   until a complete request is buffered —   |
|   see Week 3)                              |
|          |                                 |
|          v                                 |
|   HttpParser::parse()                      |
|          |                                 |
|          v                                 |
|   Router::route()                          |
|          |                                 |
|          v                                 |
|   a handler (a lambda in main.cpp)         |
|          |                                 |
|          v                                 |
|   HttpResponse::to_string()                |
|          |                                 |
|          v                                 |
|   write()                                  |
|          |                                 |
|          v                                 |
|   close()  (Socket's destructor, RAII) ----+---> back to accept(), for the next client
|
v
Browser / curl
```

One client is handled fully — accept, read, parse, route, respond, close — before the loop goes back to `accept()` for the next one. No threads, no event loop; that's a deliberate Month 1 boundary (see below), not an oversight.

## Syscalls used

| Syscall | Why |
|---|---|
| `socket()` | Allocates the endpoint; nothing is addressed or listening yet. |
| `setsockopt(SO_REUSEADDR)` | Lets the server rebind its port immediately after a restart, instead of waiting out the kernel's `TIME_WAIT` hold on the previous process's now-dead socket. |
| `bind()` | Claims a specific local `(address, port)` for the socket. |
| `listen()` | Switches the socket from "outgoing only" to passive, and sets the backlog queue size for handshake-completed-but-not-yet-`accept()`ed connections. |
| `accept()` | Blocks until a client's TCP handshake completes, then returns a *new* fd for that one connection — the listening socket itself is untouched and keeps listening. |
| `read()` | Pulls whatever bytes have arrived so far; not guaranteed to return a complete request in one call (Week 3's core lesson). |
| `write()` | Sends response bytes back; also not guaranteed to send everything in one call for larger responses. |
| `close()` | Releases the fd and tears down the connection — handled automatically by `Socket`'s destructor (RAII), on every exit path. |

## Deliberate simplifications

These are decisions, not gaps that were missed:

- **Single-threaded, one client at a time.** Concurrency (threads, `epoll`, async I/O) is explicitly out of scope for Month 1 — see the project spec's own "WHAT NOT TO WORRY ABOUT YET" list.
- **`Connection: close` on every response, no keep-alive, no pipelining.** Each connection serves exactly one request, matching behavior the server has had since Week 1.
- **No `Server:` header sent.** Never implemented — purely optional per the HTTP spec.
- **A hard 1 MiB cap on an accumulating request (`kMaxRequestSize` in `tcp_server.cpp`).** Bounds memory for a client that never sends a complete request.
- **No TLS, no HTTP/2, no database, nothing distributed.** All explicitly later-month territory per the project spec.

## Benchmark — Month 1 baseline

Per the project spec: this is a **baseline**, not a performance claim. The goal is a trustworthy "before" number for comparing future architectural changes against — not proof the server is fast. Full raw data, every field explained, and the actual experiment log live in [`Month1/Week4/test_explain.html`](Month1/Week4/test_explain.html).

```
Machine:      Windows 11 PC, running Pulse inside WSL2 (Ubuntu)
CPU:          12 logical processors visible to WSL2 (nproc) — not necessarily the full host core count
RAM:          3.5 GiB allocated to the WSL2 VM (not the host's total physical RAM)
OS:           Ubuntu 26.04 LTS, under WSL2 on Windows 11 — not bare-metal Linux

Concurrency:  1                        10
Requests:     1000                     1000 (confirmed again at 50,000)
Throughput:   2023 req/s               ~5380 req/s
Latency:      0.494 ms/request (mean)  1.86 ms/request (mean)
CPU usage:    0.0%                     up to ~12.6% of one core, under sustained load
Memory (RSS): 3840 KB                  3840 KB — identical, including at 50,000 requests
Failed:       0                        0
```

**What these numbers actually mean:**

- **Throughput went up with concurrency, latency went up too — both true at once.** The server's own request-handling work never parallelizes (still exactly one client at a time), but at `-c 10` the kernel can complete TCP handshakes for the next several clients *while* the current one is being served — connection-setup overlap, not request-processing overlap. Any individual request, meanwhile, waits behind whichever ones arrived first — hence the higher per-request latency.
- **CPU usage stays low even under sustained load, and not remotely close to saturating even one of the 12 available cores.** This server is not CPU-bound; its throughput ceiling comes from the sequential `accept()` loop, not a lack of processing power. That's precisely the kind of bottleneck later-month concurrency work (threads, `epoll`) would exist to address.
- **Memory is flat — same `3840 KB` at idle, at 1,000 requests, and at 50,000 requests.** Every per-request allocation (`raw_request`, the parsed `HttpRequest`, the response) is a local variable scoped to one call of `handle_client()`, freed the instant it returns — there is no global cache, connection pool, or shared mutable state for a request to leak into. This number is the baseline to compare against once future months add exactly that kind of state.
