# Pulse

A multi-threaded HTTP/1.1 server built from scratch in C++17 on Linux/POSIX sockets, as a systems-programming learning project. No networking frameworks: every socket call (`socket`, `bind`, `listen`, `accept`, `read`, `write`, `close`) is made directly.

| Month | Architecture | Status |
|---|---|---|
| 1 | Single-threaded, blocking: one client at a time | Done ([`Month1/`](Month1/)) |
| 2 | **Thread-per-connection**: one thread per client, thread-safe shared statistics | **Done** ([`Month2/`](Month2/)) |
| 3 | Non-blocking I/O | Planned |
| 4 | `epoll` event loop | Planned |
| 5 | Controlled worker pool | Planned |

Each month's folder has its spec (a `CLAUDE.md…` file) and, for every week, a concepts doc (`WeekN_Concepts.html`) and the test results (`notes.html`).

## Build

```bash
cmake -S . -B build
cmake --build build
```

- `cmake -S . -B build` reads `CMakeLists.txt` from this folder (`-S .`) and **generates** the real build files into `build/` (`-B build`). It doesn't compile anything itself.
- `cmake --build build` does the compiling and linking, producing `build/pulse`.
- Everything generated lives in `build/`, so `rm -rf build` wipes it without touching any source. That's why `.gitignore` excludes `build/` and `build-*/`.

**Statistics mode (Month 2 / Week 3).** The shared request counters can be built three ways, to compare them:

```bash
cmake -S . -B build-mutex  -DPULSE_STATS_MODE=mutex    # default: correct, consistent /stats snapshots
cmake -S . -B build-atomic -DPULSE_STATS_MODE=atomic   # correct, each counter is a std::atomic
cmake -S . -B build-unsafe -DPULSE_STATS_MODE=unsafe   # NO synchronization: has data races, on purpose, for the experiment
```

Pulse prints its mode at startup: `Pulse listening on port 8080... (stats mode: mutex)`.

## Run

```bash
./build/pulse 8080
```

## API

| Method | Path | Response |
|---|---|---|
| GET | `/` | `200 OK`: `Hello from Pulse!` |
| GET | `/hello` | `200 OK`: `Hello from Pulse!` |
| GET | `/health` | `200 OK`: `OK` |
| POST | `/echo` | `200 OK`: the request body, echoed back |
| GET | `/slow` | `200 OK` after a **5-second** sleep: a deliberately slow handler, to show it doesn't block other clients |
| GET | `/stats` | `200 OK`: server statistics (connections, active connections, requests, 2xx / 4xx / 5xx counts) |
| GET | `/fail` | `500 Internal Server Error`: a handler that always throws, to test error handling |
| any other path | | `404 Not Found` |
| registered path, wrong method | | `405 Method Not Allowed` |
| malformed request | | `400 Bad Request` |

```bash
curl -i http://localhost:8080/hello
curl -i -X POST http://localhost:8080/echo -d "hello"
curl    http://localhost:8080/stats
curl -i http://localhost:8080/fail                # 500
curl -i http://localhost:8080/does-not-exist      # 404
```

Inspect it while it's running:

```bash
ps -L -o tid,stat,wchan:22,comm -p $(pgrep -x pulse)    # one row per thread, and what each is waiting in
grep Threads /proc/$(pgrep -x pulse)/status              # 1 + number of open connections
ls /proc/$(pgrep -x pulse)/fd                            # 0,1,2 + listening socket (3) + one per client
```

## Architecture (Month 2)

```
                         Main thread  (TcpServer::run)
                              │
          socket() → bind() → listen()     once, at startup (TcpServer's constructor)
                              │
                ┌──────────► accept() ◄─────────────────────────────────┐
                │             │   blocks until a client connects        │
                │             ▼                                         │
                │   std::thread(serve_connection, std::move(socket))    │
                │             .detach()  ── hand the connection over ───┘  back to accept() at once
                │             │
                │             ▼  one NEW thread per connection
                │   ┌──────────────────────────────────────────────┐
                │   │ Worker thread  (serve_connection)            │
                │   │   ActiveConnectionGuard: stats +1 / -1       │
                │   │   try {                                       │
                │   │     read()  looped until the request is whole │
                │   │     HttpParser::parse()                       │
                │   │     Router::route()  → handler (main.cpp)     │
                │   │        (a handler that throws → 500)          │
                │   │     stats.on_response(status)                 │
                │   │     write()                                   │
                │   │   } catch (...) { log; this connection ends } │
                │   │   close()  ← Socket's destructor (RAII)       │
                │   └──────────────────────────────────────────────┘
                │             the thread ends
```

- **The main thread only accepts.** Every connection is moved into its own detached thread, and the main thread goes straight back to `accept()`. A slow client or slow handler blocks only its own thread.
- **Ownership is moved, never shared.** The client `Socket` is move-only. After `std::move` into the thread, the main thread's copy is empty, so exactly one thread owns and closes each connection.
- **One bad client can't kill the server.** Every worker catches all exceptions, and `SIGPIPE` is ignored, so a client that disconnects mid-request ends only its own connection.

### Shared state

Everything a worker touches, and why it's safe:

| Data | Shared? | Protection |
|---|---|---|
| client `Socket`, request buffer, response, parser state | No: each lives on its own worker's stack | none needed |
| `Router` (routes) | Yes, but **read-only** after startup | none needed |
| listening socket | Only the main thread uses it | none needed |
| `ServerStats` (the counters) | Yes, **written** by every worker | `std::mutex` (default) or `std::atomic`, chosen at build time |
| stdout / stderr (the log) | Yes, written by every worker | a log mutex: each line is built first, then written whole |

## Syscalls used

| Syscall | Why |
|---|---|
| `socket()` | Allocates the endpoint. Nothing is addressed or listening yet |
| `setsockopt(SO_REUSEADDR)` | Lets the server rebind its port straight after a restart, instead of waiting out `TIME_WAIT` |
| `bind()` | Claims a local `(address, port)` |
| `listen()` | Makes the socket passive, and sets the backlog: how many finished connections can wait to be `accept()`ed |
| `accept()` | Blocks until a client connects, then returns a **new** fd for that one connection |
| `read()` | Returns whatever bytes have arrived. Not guaranteed to be a whole request, so it's looped |
| `write()` | Sends the response. Looped, since it isn't guaranteed to send everything at once |
| `close()` | Releases the fd. Done by `Socket`'s destructor (RAII), on every exit path |
| `clone()` | Creates each worker thread (called for us by `std::thread` → `pthread_create`) |
| `signal(SIGPIPE, SIG_IGN)` | Makes writing to a vanished client return an error instead of killing the whole process |

## Deliberate simplifications

These are decisions, not gaps that were missed:

- **No limit on the number of threads.** Every connection gets a new thread, by design, so Week 4 could measure what happens without a limit. A bounded worker pool is Month 5.
- **Detached threads, no graceful shutdown.** `Ctrl+C` ends the process immediately, including requests in progress. Workers reach the server through a pointer, which is safe only because `run()` never returns.
- **`Connection: close` on every response.** No keep-alive or pipelining: one request per connection.
- **A 1 MiB cap on a request** (`kMaxRequestSize`), so one client can't grow memory without limit.
- **No TLS, no HTTP/2, no database, nothing distributed.** Later-month territory.

## Results

All numbers were measured on one laptop (Ryzen 5 5600H, 6 cores / 12 threads, Pulse running inside WSL2 with 3.5 GiB of RAM). They are **measurements for comparison, not performance claims**: on this machine the same code's throughput varied up to ~2.5× between days, so only numbers from the same session are compared with each other.

### What thread-per-connection fixed (Month 2, Week 2)

| Scenario | Month 1 | Month 2 |
|---|---|---|
| `/hello` while another client is connected but silent | **timed out after 30 s** | **18 ms** |
| `/hello` while a 5-second `/slow` is running | waits behind it | **30 ms** |
| 5 × `/slow` at the same time | ~25 s (one after another) | **~5 s** (side by side) |

### Thread safety (Month 2, Week 3)

- Without synchronization, the shared counters **lost updates** under load (20,000 requests × 3 rounds, 50 clients at once): the active-connection count drifted to 15 instead of 1. ThreadSanitizer reported **10 data races** on exactly the unprotected counter lines.
- With a **mutex** or **atomics**: exact counts, **0 races**.
- Mutex vs atomic made **no measurable throughput difference** inside the server (~8,000–8,500 req/s, same session), even though a microbenchmark showed atomic ~3× faster. Mutex was kept, because it gives consistent `/stats` snapshots.

### The trade-off: Month 1 vs Month 2, same session (Month 2, Week 4)

`ab -n 20000` to `/hello`, both servers run back to back, twice:

| Clients at once | 1 | 10 | 50 | 100 | 200 |
|---|---|---|---|---|---|
| Month 1 req/s | ~4,800 | ~11,600 | ~13,800 | ~13,900 | ~13,900 |
| Month 2 req/s | ~3,500 | ~8,000 | ~8,800 | ~8,400 | ~8,500 |
| Month 1 slowest request | 1 ms | 2–3 ms | 5 ms | 9–12 ms | **~1,000 ms** |
| Month 2 slowest request | 1–16 ms | 2–17 ms | 15–16 ms | 15–24 ms | **33 ms** |

- **For quick requests, Month 1 is faster.** Month 2 creates a new thread for every request (~0.13 ms), about as long as the whole `/hello` takes.
- **Month 2 never makes anyone wait seconds.** With 200 clients, Month 1's waiting line overflowed and some clients had to retry a second later.
- **Both hit the load tool's limit** (`ab`, single-threaded, at 98–99% CPU), so neither number is the server's true maximum. Pulse itself used only ~1.2 of 6 cores.

**Cost of each open connection in Month 2:** 1 thread + 1 file descriptor + **~49 KB** of real memory, growing in a straight line (2,000 idle connections ≈ 98 MB). Idle connections cost no CPU. On this machine the first hard limit is the open-file limit, at about 10,000 simultaneous connections.

**In one line:** Month 2 didn't make Pulse faster. It made Pulse **not get stuck**, at the price of a thread per connection. Months 3–5 aim to keep the first without paying the second.

Full details and raw data: [`Month2/Week2/notes.html`](Month2/Week2/notes.html), [`Month2/Week3/notes.html`](Month2/Week3/notes.html), [`Month2/Week4/notes.html`](Month2/Week4/notes.html). The Month 1 baseline: [`Month1/Week4/test_explain.html`](Month1/Week4/test_explain.html).
