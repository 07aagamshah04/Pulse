---
title: "Pulse — Month 2, Week 3: Test Log"
subtitle: "Thread-safe statistics and logging: what changed, how to test it, what happened"
---

<style>
.tldr { background:#fff8e1; border:2px solid #f0c040; border-radius:8px; padding:1.2em 1.5em; margin:1.5em 0; font-size:1.05em; line-height:1.6; }
.stat-row { display:flex; gap:1em; flex-wrap:wrap; margin:1.5em 0; }
.stat-card { flex:1; min-width:180px; background:#f5f5f5; border:1px solid #ddd; border-radius:8px; padding:1em 1.2em; text-align:center; }
.stat-card .label { font-size:0.82em; color:#555; text-transform:uppercase; letter-spacing:0.03em; }
.stat-card .values { font-size:1.6em; font-weight:bold; margin:0.3em 0; }
.stat-card .up { color:#c0392b; }
.stat-card .flat { color:#2e7d32; }
.stat-card .note { font-size:0.8em; color:#777; }
.verdict-right { color:#2e7d32; font-weight:bold; }
.verdict-wrong { color:#c0392b; font-weight:bold; }
.pending { background:#f3f3f3; border:1px dashed #aaa; border-radius:6px; padding:0.6em 1em; color:#555; }
details { background:#fafafa; border:1px solid #ddd; border-radius:6px; padding:0.3em 1.2em 1em; margin:1.5em 0; }
details[open] { padding-bottom:1.2em; }
summary { cursor:pointer; font-weight:600; padding:0.8em 0; }
</style>

# How this file works

Same as Weeks 1 and 2: for each test, **write predictions in `notes.txt` first** (this week especially: Test 3 is about predicting a number that might be *wrong*), run the commands, paste the full raw output into the chat, and the results get written up here.

| # | Test | Status |
|---|---|---|
| 1 | New routes: `/stats`, `/fail` (500), and the counters for known traffic | **Done** |
| 2 | `active_connections` during and after 5 × `/slow` | **Done** |
| 3 | **The unsafe counter under load:** does it lose updates? | **Done**: yes |
| 4 | The same load on the mutex and atomic builds | **Done** |
| 5 | ThreadSanitizer on all three builds | **Done** (run by Claude) |
| 6 | Scrambled log lines: gone? | **Done** |
| 7 | Mutex vs atomic vs unsafe: throughput | **Done**: no difference |

---

# What changed in the code

Every change is commented with `MONTH 2 / WEEK 3`. Read those comments alongside this table.

| File | Change | Why (Week 3 doc) |
|---|---|---|
| **`include/server_stats.hpp`**, **`src/server_stats.cpp`** (new) | `ServerStats`: six counters (total/active connections, total requests, 2xx/4xx/5xx), in **three versions**: `unsafe` (plain `uint64_t`), `mutex` (plain + `std::mutex` + `lock_guard`), `atomic` (`std::atomic<uint64_t>`). The preprocessor keeps exactly one. Plus `ActiveConnectionGuard`: RAII for active connections | Parts 2–8 |
| `CMakeLists.txt` | New option **`PULSE_STATS_MODE`** = `unsafe` / `mutex` / `atomic` (default `mutex`), and the two new `.cpp` files | So the three versions can be built side by side and compared without editing code |
| **`include/logger.hpp`**, **`src/logger.cpp`** (new) | `log_line()` / `log_error()`: write one finished line under a log mutex | Part 9: scrambled log lines |
| `src/tcp_server.cpp`, `serve_connection()` | `ActiveConnectionGuard active(stats_);` as the first line | Connections +1 at start, active −1 on **every** way a worker ends |
| `src/tcp_server.cpp`, `handle_client()` | `stats_.on_response(status)` just before writing. A throwing handler now gets **500** instead of a dropped connection. Every log line built in an `ostringstream`, then `log_line()` | Parts 8, 9 |
| `include/http_response.hpp` | `status_code()` getter, `internal_server_error()` factory | To count 2xx/4xx/5xx, and to send a 500 |
| `include/tcp_server.hpp` | Constructor takes `ServerStats&`, stored as reference member `stats_`. **Shared-state inventory** written in the class comment | The spec's "documented shared state" deliverable |
| `src/main.cpp` | `ServerStats stats;` created in `main()` before the server. New routes **`GET /stats`** (captures `[&stats]`) and **`GET /fail`** (always throws) | Part 8 |

## Where the statistics are used: the complete map

`ServerStats` is created in **one** place, changed in **three**, and read in **one**. In the order things happen:

```
main.cpp:252          ServerStats stats;                         ← the ONE stats object, created once
main.cpp:253          TcpServer server(..., build_router(stats), stats);
                         │                         │
                         │                         └─► main.cpp:135  /stats lambda captures [&stats]   (READS)
                         └─► tcp_server.cpp:69  stats_(stats)  → stored as reference member stats_ (tcp_server.hpp:145)

── for EVERY connection, on its worker thread ──────────────────────────────────────────────
tcp_server.cpp:296    ActiveConnectionGuard active(stats_);       ← worker starts
                        └─ server_stats.hpp:159  constructor → on_connection_opened()
                                                   total_connections +1, active_connections +1

tcp_server.cpp:468    stats_.on_response(response.status_code()); ← response decided, about to write
                                                   total_requests +1, and ONE of 2xx / 4xx / 5xx +1

(serve_connection ends: normal return OR after catching an exception)
                        └─ server_stats.hpp:160  destructor → on_connection_closed()
                                                   active_connections −1

── only when someone requests GET /stats ───────────────────────────────────────────────────
main.cpp:136          StatsSnapshot s = stats.snapshot();         ← copies all six numbers, prints them
```

| Counter | Increased (+1) | Decreased (−1) | Read |
|---|---|---|---|
| `total_connections` | guard constructor (`server_stats.hpp:159`), called from `tcp_server.cpp:296` | never | `/stats` (`main.cpp:136`) |
| `active_connections` | the same guard constructor | guard **destructor** (`server_stats.hpp:160`), which runs automatically when `serve_connection()` ends | `/stats` |
| `total_requests` | `on_response()` at `tcp_server.cpp:468` | never | `/stats` |
| `responses_2xx` / `4xx` / `5xx` | `on_response()` (same line). The status code picks which one | never | `/stats` |

What the map shows:

- **Only worker threads change the stats.** The main thread creates the object and never touches it again. The `/stats` handler also runs on a worker, the one serving that request.
- **There's no visible `on_connection_closed()` call in `tcp_server.cpp`.** The decrease is in the guard's destructor, so it happens on every way `serve_connection()` can end (normal return, early `return` inside `handle_client`, exception) and can't be forgotten.
- **Some connections never reach line 468**, so they count as a *connection* but not a *request*: a client that disconnects before sending a complete request (`n == 0` early return), and a `read()` error (`n < 0` early return). A failed *write* is still counted, because line 468 runs before the write.
- **Every one of these calls goes through `ServerStats`**, so the build's mode (`unsafe` / `mutex` / `atomic`, in `server_stats.cpp`) decides how safe *all* of them are at once.

(Line numbers are from the Week 3 version of the code.)

**Decisions made, and written down in `server_stats.hpp`:**

- **"total_requests" = requests Pulse sent a response to**, including 400s for malformed input. A client that disconnects before sending a complete request is a *connection* but not a *request*. That way **`total_requests == responses_2xx + responses_4xx + responses_5xx`** always holds once traffic stops.
- **`/stats` sees itself as active but not yet as a request.** While it runs, its own connection is active, so `active_connections` is ≥ 1. It's counted as a request only *after* it responds. So an idle server's `/stats` shows `active_connections: 1`.
- **Default mode is `mutex`**: `on_response` updates `total_requests` and the bucket under one lock, and `/stats` copies all six under one lock, so a snapshot always adds up.

---

# Setup: build the three versions

From the Pulse folder in WSL. Each version goes in its own build folder, so you can switch between them without rebuilding:

```bash
cd /mnt/c/Users/aagam/OneDrive/Desktop/Pulse

cmake -S . -B build-unsafe -DPULSE_STATS_MODE=unsafe && cmake --build build-unsafe
cmake -S . -B build-mutex  -DPULSE_STATS_MODE=mutex  && cmake --build build-mutex
cmake -S . -B build-atomic -DPULSE_STATS_MODE=atomic && cmake --build build-atomic
```

| Piece | Meaning |
|---|---|
| `-S . -B build-unsafe` | **S**ource is this folder, **b**uild into `build-unsafe/` (created if missing) |
| `-DPULSE_STATS_MODE=unsafe` | Set the CMake option. You'll see `-- Pulse stats mode: unsafe` in CMake's output |
| `cmake --build build-unsafe` | Compile. Same as `cd build-unsafe && make` |

Each one should end with `[100%] Built target pulse` and **no warnings**. Every Pulse prints its mode at startup: `Pulse listening on port 8080... (stats mode: unsafe)`. Always check that line, so you know which build you're testing.

(The old `build/` folder still works too: `cd build && cmake .. && make` gives the default, `mutex`.)

**`.gitignore`:** these folders are build output. `.gitignore` had only `build/` (which matches only the folder named exactly `build`), so `build-*/` was added to cover them.

---

# Test 1 — New routes, and the counters for known traffic

**Question:** do `/stats` and `/fail` work, and do the counters match exactly what was sent?

```bash
# T1
./build-mutex/pulse 8080

# T2, one line at a time, top to bottom
curl -s localhost:8080/stats; echo
curl -s -o /dev/null -w "/hello -> %{http_code}\n" localhost:8080/hello
curl -s -o /dev/null -w "/nope  -> %{http_code}\n" localhost:8080/nope
curl -s -w "\n/fail  -> %{http_code}\n" localhost:8080/fail
printf 'GARBAGE\r\n\r\n' | nc -q1 localhost 8080 | head -1
curl -s localhost:8080/stats; echo
```

Keep Pulse running in T1 for Test 2.

**Paste back:** T1 and T2.

## Result: every counter matches the traffic exactly

<div class="tldr">
<b>What I did:</b> on the mutex build, read <code>/stats</code>, sent four known requests (<code>/hello</code>, <code>/nope</code>, <code>/fail</code>, garbage), then read <code>/stats</code> again.
<br><br>
<b>What happened:</b> <code>/fail</code> answered <b>500</b> and the server kept going. The second <code>/stats</code> showed <b>5 requests = 2 × 2xx + 2 × 4xx + 1 × 5xx</b>, which is exactly the traffic sent, once you account for one rule: <b><code>/stats</code> counts itself as a connection straight away, but as a request only after it has answered.</b>
</div>

### Predicted vs actual

No Test 1 predictions were written in `notes.txt`. Here is what the design (`server_stats.hpp`) says should happen, against what did:

| | Expected from the design | Actual | Verdict |
|---|---|---|---|
| `/fail` | `500`, server keeps running | **`500`**, and the next request (garbage) was served normally | <span class="verdict-right">As expected</span> |
| first `/stats` | connections 1, active 1, requests 0 | **1 / 1 / 0** | <span class="verdict-right">As expected</span> |
| second `/stats`, requests | 5 | **5** | <span class="verdict-right">As expected</span> |
| second `/stats`, 2xx / 4xx / 5xx | 2 / 2 / 1 | **2 / 2 / 1** | <span class="verdict-right">As expected</span> |
| second `/stats`, active | 1 (only itself) | **1** | <span class="verdict-right">As expected</span> |
| `total_requests == 2xx + 4xx + 5xx` | must hold | 5 = 2 + 2 + 1 | <span class="verdict-right">Holds</span> |

### Working out every number

Six connections were made, in this order:

| # | Request | Status | Bucket | Counted as a request by the second `/stats`? |
|---|---|---|---|---|
| 1 | `GET /stats` (first) | 200 | 2xx | **yes**: it was counted when it finished, before #6 started |
| 2 | `GET /hello` | 200 | 2xx | yes |
| 3 | `GET /nope` | 404 | 4xx | yes |
| 4 | `GET /fail` | **500** | **5xx** | yes |
| 5 | `GARBAGE` | 400 | 4xx | yes: Malformed requests count, because Pulse sent them a response |
| 6 | `GET /stats` (second, the one reading the numbers) | 200 | (2xx, later) | **not yet**: `on_response()` runs only *after* its handler has built the text you're reading |

**First `/stats`:**

```
total_connections: 1     ← itself: ActiveConnectionGuard's constructor ran when its worker started
active_connections: 1    ← itself: still running (it's the one reading the numbers)
total_requests: 0        ← itself NOT yet: on_response() runs after the handler returns
responses_2xx/4xx/5xx: 0
```

This is the "/stats sees itself as active but not yet as a request" rule from the design decisions, visible on the very first call.

**Second `/stats`:**

```
total_connections: 6     ← all six rows above, including itself
active_connections: 1    ← only itself. #1–#5 have all finished, and each guard's destructor did −1
total_requests: 5        ← rows #1–#5. Row #6 (itself) not yet
responses_2xx: 2         ← #1 (first /stats) and #2 (/hello)
responses_4xx: 2         ← #3 (404) and #5 (400)
responses_5xx: 1         ← #4 (/fail)
```

`active_connections: 1` after five completed connections confirms the `ActiveConnectionGuard` destructor did its −1 on every one, **including the two that weren't 200s**: `/fail` (the handler threw, was caught in `handle_client`, and the worker still ended normally) and the malformed request (`handle_client` left its loop early).

### `/fail`: the new 500 path

```
$ curl -s -w "\n/fail  -> %{http_code}\n" localhost:8080/fail

/fail  -> 500
```

The blank line is the (empty) body: `internal_server_error()` has no body text, only the status line. Pulse's log shows what happened inside:

```
[TID 957] Parsed (82 bytes total): GET /fail HTTP/1.1
[TID 957] handler threw: GET /fail always fails (deliberate, for testing 5xx) — responding 500 Internal Server Error
```

1. Worker 957 parsed the request normally.
2. `router_.route()` called the `/fail` handler, which threw `std::runtime_error`.
3. The new `try/catch` around `route()` in `handle_client` caught it, logged `handler threw`, and set `response = HttpResponse::internal_server_error()`.
4. `on_response(500)` counted it as **5xx**, and the 500 was written to the client.

**Before Week 3** the same exception went up to `serve_connection()`, which logged a "connection error" and closed the connection **without sending anything**. curl would have reported an empty reply, and there was nothing to count as a 5xx.

### Pulse's log

```
Pulse listening on port 8080... (stats mode: mutex)
Client connected: 127.0.0.1:49474
[TID 906] Parsed (83 bytes total): GET /stats HTTP/1.1
Client connected: 127.0.0.1:49740
[TID 917] Parsed (83 bytes total): GET /hello HTTP/1.1
Client connected: 127.0.0.1:51642
[TID 928] Parsed (82 bytes total): GET /nope HTTP/1.1
Client connected: 127.0.0.1:43508
[TID 957] Parsed (82 bytes total): GET /fail HTTP/1.1
[TID 957] handler threw: GET /fail always fails (deliberate, for testing 5xx) — responding 500 Internal Server Error
Client connected: 127.0.0.1:47658
[TID 970] Malformed request (11 bytes so far) — responding 400 Bad Request.
Client connected: 127.0.0.1:47674
[TID 1007] Parsed (83 bytes total): GET /stats HTTP/1.1
```

- **`(stats mode: mutex)`** on the first line confirms which build ran. Always check it.
- **Six `Client connected` lines = `total_connections: 6`.**
- **`Malformed request (11 bytes so far)`**: `GARBAGE\r\n\r\n` is 7 + 2 + 2 = 11 bytes. The parser found the blank line that ends the headers, but `GARBAGE` isn't a valid request line → 400.
- **The `handler threw` line goes to stderr** (`log_error`), every other line to stdout (`log_line`). Both use the same log mutex, so they can't split each other.
- **Every line is whole.** With one request at a time that's expected anyway. Test 6 checks it under load.

<details>
<summary>Raw output: Test 1</summary>

**T1 (Pulse)**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ./build-mutex/pulse 8080
Pulse listening on port 8080... (stats mode: mutex)
Client connected: 127.0.0.1:49474
[TID 906] Parsed (83 bytes total): GET /stats HTTP/1.1
Client connected: 127.0.0.1:49740
[TID 917] Parsed (83 bytes total): GET /hello HTTP/1.1
Client connected: 127.0.0.1:51642
[TID 928] Parsed (82 bytes total): GET /nope HTTP/1.1
Client connected: 127.0.0.1:43508
[TID 957] Parsed (82 bytes total): GET /fail HTTP/1.1
[TID 957] handler threw: GET /fail always fails (deliberate, for testing 5xx) — responding 500 Internal Server Error
Client connected: 127.0.0.1:47658
[TID 970] Malformed request (11 bytes so far) — responding 400 Bad Request.
Client connected: 127.0.0.1:47674
[TID 1007] Parsed (83 bytes total): GET /stats HTTP/1.1
```

**T2**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s localhost:8080/stats; echo
stats_mode: mutex
total_connections: 1
active_connections: 1
total_requests: 0
responses_2xx: 0
responses_4xx: 0
responses_5xx: 0

aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s -o /dev/null -w "/hello -> %{http_code}\n" localhost:8080/hello
/hello -> 200
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s -o /dev/null -w "/nope  -> %{http_code}\n" localhost:8080/nope
/nope  -> 404
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s -w "\n/fail  -> %{http_code}\n" localhost:8080/fail

/fail  -> 500
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ printf 'GARBAGE\r\n\r\n' | nc -q1 localhost 8080 | head -1
HTTP/1.1 400 Bad Request
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s localhost:8080/stats; echo
stats_mode: mutex
total_connections: 6
active_connections: 1
total_requests: 5
responses_2xx: 2
responses_4xx: 2
responses_5xx: 1
```

</details>

---

# Test 2 — `active_connections` during and after 5 × `/slow`

**Question:** does `active_connections` go up while connections are open, and come back down to exactly 1 (the `/stats` request itself) afterwards?

```bash
# T2 (same Pulse as Test 1)
for i in 1 2 3 4 5; do curl -s -o /dev/null localhost:8080/slow & done
sleep 1; echo "DURING:"; curl -s localhost:8080/stats
wait
echo "AFTER:";  curl -s localhost:8080/stats
```

**Paste back:** T2. Then `Ctrl+C` Pulse in T1.

## Result: active rose to 6, then came back to exactly 1

<div class="tldr">
<b>What I did:</b> 5 × <code>/slow</code> at once (each worker sleeps 5 s), read <code>/stats</code> one second in, waited for all five, and read it again.
<br><br>
<b>What happened:</b> <b>during</b>, <code>active_connections</code> was <b>6</b> (the 5 sleeping <code>/slow</code> workers + the <code>/stats</code> request itself) and the five <code>/slow</code>s were <b>not yet</b> counted as requests. <b>After</b>, active was back to exactly <b>1</b>, and requests had gone up by exactly the 6 that finished in between. Every number is accounted for.
</div>

<div class="stat-row">
<div class="stat-card">
<div class="label">active_connections: during → after</div>
<div class="values">6 <span class="flat">&rarr; 1</span></div>
<div class="note">5 /slow + itself, then only itself</div>
</div>
<div class="stat-card">
<div class="label">total_requests: during → after</div>
<div class="values">6 &rarr; 12</div>
<div class="note">+5 /slow, +1 the "during" /stats</div>
</div>
<div class="stat-card">
<div class="label">2xx + 4xx + 5xx = total_requests</div>
<div class="values"><span class="flat">9+2+1 = 12</span></div>
<div class="note">the invariant holds</div>
</div>
</div>

### Predicted vs actual

No Test 2 predictions were written in `notes.txt`. From the design:

| | Expected | Actual | Verdict |
|---|---|---|---|
| `active_connections` during | 6: 5 `/slow` + this `/stats` | **6** | <span class="verdict-right">As expected</span> |
| `active_connections` after | 1: only this `/stats` | **1** | <span class="verdict-right">As expected</span> |
| `/slow`s counted as requests *during*? | no: `on_response()` runs only after the handler's 5 s sleep | **no** (requests 6, see below) | <span class="verdict-right">As expected</span> |

### Working out every number

Starting point: the end of Test 1, **same Pulse process**. Test 1's second `/stats` showed 6 connections and 5 requests, and then it finished, so it was counted as one more request (a 2xx). Going into Test 2: **6 connections, 6 requests (3 × 2xx, 2 × 4xx, 1 × 5xx), 0 active.**

**DURING** (1 s in: all five `/slow` workers asleep in the handler):

| Counter | Value | Why |
|---|---|---|
| `total_connections` | **12** | 6 before + 5 `/slow` + this `/stats` |
| `active_connections` | **6** | 5 `/slow` workers (asleep, not finished) + this `/stats` |
| `total_requests` | **6** | Unchanged from the start of Test 2. The five `/slow`s are still sleeping, so none has reached `on_response()` yet. This `/stats` isn't counted yet either |
| `responses_2xx` | **3** | Test 1's two 2xx + Test 1's second `/stats` |
| `4xx` / `5xx` | 2 / 1 | Unchanged from Test 1 |

**`active 6` while `requests` didn't move** is the clearest picture of what the two counters mean. "Active" = connections being worked on *right now*, counted from the moment the worker starts. "Requests" = responses *produced*, counted at the very end. During those 5 seconds there are five connections in the first state and not yet in the second.

**AFTER** (all five finished):

| Counter | Value | Why |
|---|---|---|
| `total_connections` | **13** | + this new `/stats` |
| `active_connections` | **1** | Only this `/stats`. All five `/slow` workers ended, and each guard's destructor did its −1, as did the "during" `/stats` |
| `total_requests` | **12** | 6 + the 5 `/slow`s + the "during" `/stats` |
| `responses_2xx` | **9** | 3 + 5 `/slow` + 1 "during" `/stats` |
| `4xx` / `5xx` | 2 / 1 | Unchanged: no errors in Test 2 |

Check: **9 + 2 + 1 = 12 = `total_requests`.** The invariant from the design decisions holds.

### The bash lines

`[1] 1150` … `[5] 1154` are bash's job numbers and PIDs for the five background `curl`s, and `[1] Done …` is bash reporting that job 1 finished (Week 2 / Test 3 has the same lines). bash only prints `Done` lines at certain moments, which is why only one appears. `wait` waited for all five, which is why "AFTER" ran only once they'd all finished.

### What this proves

- **`ActiveConnectionGuard` works in both directions.** +1 at the start of each worker (active reached 6), −1 at the end (back to exactly 1), with no manual `on_connection_closed()` call anywhere.
- **The counters distinguish "in progress" from "done"** exactly as designed.
- **The mutex build's snapshot adds up**, both during and after, even while five other workers were alive.

This was all at low concurrency (6 threads). Whether these numbers stay exact under **20,000 requests from 50 clients at once**, without any protection, is Test 3.

<details>
<summary>Raw output: Test 2</summary>

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ for i in 1 2 3 4 5; do curl -s -o /dev/null localhost:8080/slow & done
sleep 1; echo "DURING:"; curl -s localhost:8080/stats
wait
echo "AFTER:";  curl -s localhost:8080/stats
[1] 1150
[2] 1151
[3] 1152
[4] 1153
[5] 1154
DURING:
stats_mode: mutex
total_connections: 12
active_connections: 6
total_requests: 6
responses_2xx: 3
responses_4xx: 2
responses_5xx: 1
[1]   Done                       curl -s -o /dev/null localhost:8080/slow
AFTER:
stats_mode: mutex
total_connections: 13
active_connections: 1
total_requests: 12
responses_2xx: 9
responses_4xx: 2
responses_5xx: 1
```

</details>

---

# Test 3 — The unsafe counter under load

**Question:** the spec's Week 3 experiment. With no protection, under 20,000 requests from 50 clients at once, does Pulse count wrong? The Week 3 doc (Part 3) gave reasons it might *look* right anyway. Write your prediction before running.

The method: read `/stats`, send exactly 20,000 requests, read `/stats` again, and check the differences.

```bash
# T1: the UNSAFE build, log thrown away (Week 2 lesson: don't log to /mnt/c under load)
./build-unsafe/pulse 8080 > /dev/null 2>&1

# T2
curl -s localhost:8080/stats; echo "-----"
ab -n 20000 -c 50 http://localhost:8080/hello 2>&1 | grep -E "Complete requests|Failed requests"
sleep 0.5; echo "-----"
curl -s localhost:8080/stats
```

Then repeat the `ab` + `/stats` part **two more times** (same Pulse, don't restart it) and paste all three rounds.

**How to check the numbers** (work these out from your output):

| Check | Correct if… | Why |
|---|---|---|
| `total_requests` after − before | **20,001** | 20,000 from `ab` + the first `/stats` call (counted only *after* it responded, so it shows up in the second reading) |
| `responses_2xx` after − before | **20,001** | Same, since all are 200s |
| `total_requests` vs `2xx + 4xx + 5xx` | **equal** | Every counted response lands in exactly one bucket |
| `active_connections` after | **exactly 1** | Nothing is running except the `/stats` request reading it |
| `total_connections` | ≥ the request count. **Not exact** | `ab` with `-c 50` can open a few extra connections at the end that it closes without sending anything. They're real connections, but not requests |

**Paste back:** all of T2.

## Result: the unsafe build lost updates, but you'd only notice if you checked the right counter

<div class="tldr">
<b>What I did:</b> unsafe build, three rounds of 20,000 requests from 50 clients at once, reading <code>/stats</code> before and after each round.
<br><br>
<b>What happened:</b> <code>ab</code> reported 20,000 complete and <b>0 failed</b> every round. Every client got the right answer. But the counters were wrong:
<br>• <code>total_requests</code> was exactly right in rounds 1 and 3, and <b>off by one</b> in round 2: <b>1 lost update in 60,000</b>.
<br>• <code>active_connections</code> should be <b>1</b> after each round, and was <b>7</b>, then <b>9</b>, then <b>15</b>. It climbs further away from the truth every round and never recovers.
<br><br>
<b>The key insight:</b> the Week 3 doc (Part 3) predicted the unsafe counter might <i>look</i> right, and for <code>total_requests</code> it nearly did (2 of 3 rounds perfect, 99.998% accurate overall). If that were the only counter checked, the bug would have passed the test. <code>active_connections</code> exposed it because it gets <b>two</b> updates per connection, from <b>every</b> worker, at the busiest moments.
</div>

<div class="stat-row">
<div class="stat-card">
<div class="label">total_requests: lost updates</div>
<div class="values">1 <span style="font-size:0.6em">in 60,000</span></div>
<div class="note">rounds 1 and 3 exact, round 2 off by one</div>
</div>
<div class="stat-card">
<div class="label">active_connections after each round</div>
<div class="values"><span class="up">7 &rarr; 9 &rarr; 15</span></div>
<div class="note">should be 1 every time: never recovers</div>
</div>
<div class="stat-card">
<div class="label">ab: failed requests</div>
<div class="values"><span class="flat">0</span></div>
<div class="note">clients never noticed anything</div>
</div>
</div>

### Predicted vs actual

The `notes.txt` lines were left blank. The prediction given in chat was that the unsafe counter **would be wrong**. The Week 3 doc (Part 3) predicted the opposite risk: that it might *look* right.

| Prediction | Actual | Verdict |
|---|---|---|
| "It would be wrong" (chat) | Wrong: 1 lost request count, and `active_connections` off by up to 14 | <span class="verdict-right">Right</span> |
| Doc Part 3: "may well show the right number most of the time" | `total_requests` **was** right in 2 of 3 rounds, and off by only 1 in the third | <span class="verdict-right">Right too</span>, for that counter |
| `total_requests` +20,001 each round? | +20,001 / **+20,000** / +20,001 | <span class="verdict-wrong">Not every round</span> |
| `active_connections` exactly 1 afterwards? | 7 / 9 / 15 | <span class="verdict-wrong">Never</span> |

Both predictions were right: the bug is real, *and* the most obvious counter almost hides it.

### Working out the numbers

**Why +20,001 is the right answer for `total_requests`.** Each round reads `/stats`, sends 20,000 requests, then reads `/stats` again. The first `/stats` is counted as a request only *after* it answers (Test 1's rule), so it appears in the second reading: 20,000 + 1 = **20,001**.

| Round | `total_requests` before → after | Change | Expected | Lost |
|---|---|---|---|---|
| 1 | 0 → 20,001 | +20,001 | +20,001 | 0 |
| 2 | 20,002 → 40,002 | **+20,000** | +20,001 | **1** |
| 3 | 40,003 → 60,004 | +20,001 | +20,001 | 0 |

(Between rounds the number also goes up by exactly 1, e.g. 20,001 → 20,002, because the "after" `/stats` of one round is counted before the "before" `/stats` of the next.)

**Overall:** 60,000 from `ab` + 5 `/stats` that had finished = 60,005 should have been counted, and **60,004** were. One update lost, in round 2. `responses_2xx` lost the same one: it equals `total_requests` in every reading, because `on_response()` increments both, one after the other, and both lost an update at the same collision.

**Why 1 is the right answer for `active_connections`.** After `ab` finishes, nothing is connected except the `/stats` request doing the reading. Every connection ever made did exactly one +1 (guard constructor) and one −1 (guard destructor), so they should cancel out.

| Reading | `active_connections` | Should be | Off by |
|---|---|---|---|
| before round 1 | 1 | 1 | 0 |
| after round 1 | **7** | 1 | +6 |
| after round 2 | **9** | 1 | +8 |
| after round 3 | **15** | 1 | +14 |

**Too high** means more −1s were lost than +1s. In each lost update, two workers read the same value, and one write overwrote the other. When the overwritten update was a −1, the count is stuck one too high, **forever**: nothing ever corrects it. That's why it only ratchets upward from round to round (6, then 2 more, then 6 more). (Lost +1s happen too, and they'd push it the other way. The visible number is the *net* result.)

**Why `active_connections` broke much more than `total_requests`:**

| | `total_requests` | `active_connections` |
|---|---|---|
| Updates per connection | 1 (in `on_response`) | **2** (+1 at start, −1 at end) |
| Where | once, near the end of the request | at the **first** and **last** moment of every worker. With 50 clients and thread-per-connection, those are exactly the moments when many threads are starting and ending at once |
| Lost in 60,000 connections | 1 | at least 14 (net) |

More updates on the same variable, packed into the same moments, means more chances for two threads to land in the same three instructions (Week 3 doc, Part 2: `mov` / `add` / `mov`).

**`total_connections` can't be checked exactly** (as the test plan said): `ab -c 50` opens a few extra connections that it closes without sending a request. Readings of 20,027 / 40,037 / 60,083 are a little above the request count, as expected. That's `ab`'s doing, not a counting error, and Test 4 will show a similar excess on the safe builds.

### What this means

1. **The data race is real in Pulse, under realistic load.** Not just in a microbenchmark.
2. **It was almost invisible.** `ab`: 0 failed. `total_requests`: 99.998% right, and exactly right in two of three runs. A quick "does the count match?" test could easily pass. The Week 3 doc (Part 3) warned about exactly this: *"works in testing" is not "correct"*.
3. **Wrong counters stay wrong.** `active_connections` drifts and never comes back. A server using it to decide "am I overloaded?" (Week 4's question) would believe it had 14 extra connections that don't exist, and the error keeps growing the longer it runs.
4. **Clients never noticed.** The race corrupted only Pulse's own bookkeeping. Every response was correct. That's what makes this class of bug dangerous: nothing fails visibly.

Test 4 runs the same load on the mutex and atomic builds, where the expectation is +20,001 every round and active exactly 1. Test 5 asks ThreadSanitizer to find the race directly, instead of hoping it shows up in the numbers.

<details>
<summary>Raw output: Test 3 (unsafe build)</summary>

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s localhost:8080/stats; echo "-----"
ab -n 20000 -c 50 http://localhost:8080/hello 2>&1 | grep -E "Complete requests|Failed requests"
sleep 0.5; echo "-----"
curl -s localhost:8080/stats
stats_mode: unsafe
total_connections: 1
active_connections: 1
total_requests: 0
responses_2xx: 0
responses_4xx: 0
responses_5xx: 0
-----
Complete requests:      20000
Failed requests:        0
-----
stats_mode: unsafe
total_connections: 20027
active_connections: 7
total_requests: 20001
responses_2xx: 20001
responses_4xx: 0
responses_5xx: 0
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s localhost:8080/stats; echo "-----"
ab -n 20000 -c 50 http://localhost:8080/hello 2>&1 | grep -E "Complete requests|Failed requests"
sleep 0.5; echo "-----"
curl -s localhost:8080/stats
stats_mode: unsafe
total_connections: 20028
active_connections: 7
total_requests: 20002
responses_2xx: 20002
responses_4xx: 0
responses_5xx: 0
-----
Complete requests:      20000
Failed requests:        0
-----
stats_mode: unsafe
total_connections: 40037
active_connections: 9
total_requests: 40002
responses_2xx: 40002
responses_4xx: 0
responses_5xx: 0
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s localhost:8080/stats; echo "-----"
ab -n 20000 -c 50 http://localhost:8080/hello 2>&1 | grep -E "Complete requests|Failed requests"
sleep 0.5; echo "-----"
curl -s localhost:8080/stats
stats_mode: unsafe
total_connections: 40038
active_connections: 9
total_requests: 40003
responses_2xx: 40003
responses_4xx: 0
responses_5xx: 0
-----
Complete requests:      20000
Failed requests:        0
-----
stats_mode: unsafe
total_connections: 60083
active_connections: 15
total_requests: 60004
responses_2xx: 60004
responses_4xx: 0
responses_5xx: 0
```

</details>

---

# Test 4 — The same load on the mutex and atomic builds

Exactly Test 3's commands, once with `./build-mutex/pulse 8080 > /dev/null 2>&1` in T1 and once with `./build-atomic/pulse 8080 > /dev/null 2>&1`. Run three `ab` rounds each, and apply the same checks.

**Paste back:** T2 for both builds.

## Result: mutex and atomic both count exactly

<div class="tldr">
<b>What I did:</b> one round of Test 3's load (20,000 requests, 50 at a time) on each safe build.
<br><br>
<b>What happened:</b> both builds showed <code>total_requests</code> <b>+20,001 exactly</b> and <code>active_connections</code> back to <b>exactly 1</b>. The two counters the unsafe build got wrong are now right. <code>total_connections</code> was a little above 20,001 (20,005 and 20,024). That isn't a counting error: it's <code>ab</code> opening a few connections it never uses, measured and explained below.
</div>

### Unsafe vs mutex vs atomic, side by side

| | Unsafe (Test 3, round 1) | **Mutex** | **Atomic** | Correct value |
|---|---|---|---|---|
| `total_requests` change | +20,001 (round 2: **+20,000**) | **+20,001** | **+20,001** | +20,001 |
| `responses_2xx` | 20,001 (round 2: lost 1) | **20,001** | **20,001** | 20,001 |
| `active_connections` after | **7** (then 9, then 15) | **1** | **1** | 1 |
| `ab` failed | 0 | 0 | 0 | 0 |
| `total_connections` | 20,027 | 20,005 | 20,024 | ≥ 20,001 (not exact, below) |

### Predicted vs actual

No Test 4 predictions were written in `notes.txt`.

| | Expected | Actual | Verdict |
|---|---|---|---|
| Exact every round? | yes, for both | +20,001 and active = 1 on both | <span class="verdict-right">As expected</span> (one round each instead of the planned three. With the unsafe build, round 1 already showed active off by 6, so one round was enough to see the difference) |
| Any difference between mutex and atomic? | not in correctness | none in the counts. The `total_connections` difference (20,005 vs 20,024) is from `ab`, not the build (below) | <span class="verdict-right">As expected</span> |

**Why both are right:** in the mutex build, the read-add-write of each counter happens while holding the stats mutex, so no other worker can be inside it at the same time. In the atomic build, each `++` / `--` is one `lock add` / `lock sub` instruction that no other core can interrupt (Week 3 doc, Parts 6 and 7). Either way, Part 2's lost update is impossible: no two workers can both read 41 and both write 42.

## Why `total_connections` is more than 20,001

`total_requests` is exact, so where do the extra connections come from? (20,005 − 20,001 = **4** extra on the mutex run, 20,024 − 20,001 = **23** on the atomic run, and 26 on Test 3's first unsafe round.)

**Measured, not guessed.** The same `ab -n 20000` was run against the mutex build at three different concurrency levels, with Pulse's log saved to `/tmp`. Pulse logs every accepted connection (`Client connected`), every parsed request (`Parsed`), and every connection that closes before sending a complete request (`Client disconnected before completing a request (N bytes …)`):

| `ab -c` | `Client connected` | `Parsed` | `disconnected before completing` | bytes those sent | `/stats` total_connections |
|---|---|---|---|---|---|
| 1 | 20,001 | 20,001 | **0** | | 20,001 |
| 10 | 20,007 | 20,001 | **6** | **0** each | 20,007 |
| 50 | 20,024 | 20,001 | **23** | **0** each | 20,024 |

(`Parsed` = 20,000 from `ab` + 1 `/stats`.)

So:

1. **Every extra connection is one that connected, sent 0 bytes, and closed.** Connected + extra exactly accounts for every connection. Nothing is miscounted.
2. **They come from `ab`, and grow with concurrency:** none at `-c 1`, 6 at `-c 10`, 23 at `-c 50`. `ab` keeps `-c` connections busy at all times. Towards the end of the run, it has already opened connections for requests it will turn out not to need: once the last of the 20,000 responses arrives, it stops and closes the connections it opened but never sent anything on. With `-c 1` there's never a spare connection, so there's no excess. The exact count varies from run to run (4 one time, 23 another at the same `-c 50`), because it depends on timing at the very end.
3. **Pulse handled them correctly, by design.** Each one got a worker, whose `read()` returned 0 (client closed) → log `Client disconnected before completing a request (0 bytes received so far)` → `return` without a response. So each one is counted as a **connection** (the guard ran) but **not a request** (`on_response()` never ran). That's exactly the rule from the design decisions: *"a client that disconnects before sending a complete request is a connection but not a request."* `active_connections` still returned to 1, because their guards' destructors ran too.

**Takeaway:** `total_connections` counts *TCP connections*, `total_requests` counts *HTTP requests answered*. Under a load-testing tool, the first can legitimately be a bit higher than the second. That's why Test 3 checks `total_requests` and `active_connections` exactly, and treats `total_connections` only as "≥".

<details>
<summary>Raw output: Test 4 (mutex and atomic builds)</summary>

**Mutex build**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s localhost:8080/stats; echo "-----"
ab -n 20000 -c 50 http://localhost:8080/hello 2>&1 | grep -E "Complete requests|Failed requests"
sleep 0.5; echo "-----"
curl -s localhost:8080/stats
stats_mode: mutex
total_connections: 1
active_connections: 1
total_requests: 0
responses_2xx: 0
responses_4xx: 0
responses_5xx: 0
-----
Complete requests:      20000
Failed requests:        0
-----
stats_mode: mutex
total_connections: 20005
active_connections: 1
total_requests: 20001
responses_2xx: 20001
responses_4xx: 0
responses_5xx: 0
```

**Atomic build**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ curl -s localhost:8080/stats; echo "-----"
ab -n 20000 -c 50 http://localhost:8080/hello 2>&1 | grep -E "Complete requests|Failed requests"
sleep 0.5; echo "-----"
curl -s localhost:8080/stats
stats_mode: atomic
total_connections: 1
active_connections: 1
total_requests: 0
responses_2xx: 0
responses_4xx: 0
responses_5xx: 0
-----
Complete requests:      20000
Failed requests:        0
-----
stats_mode: atomic
total_connections: 20024
active_connections: 1
total_requests: 20001
responses_2xx: 20001
responses_4xx: 0
responses_5xx: 0
```

**The extra-connections measurement** (run during this write-up: mutex build, `stdbuf -oL` log to `/tmp`, `ab -n 20000` at each concurrency):

```
Complete requests: 20000 Failed requests: 0
  -c 1: connected=20001  parsed=20001  disconnected_before_request=0
  stats_mode: mutex total_connections: 20001 active_connections: 1 total_requests: 20000 responses_2xx: 20000 responses_4xx: 0 responses_5xx: 0
Complete requests: 20000 Failed requests: 0
  -c 10: connected=20007  parsed=20001  disconnected_before_request=6
  bytes received by those:       6 (0
  stats_mode: mutex total_connections: 20007 active_connections: 1 total_requests: 20000 responses_2xx: 20000 responses_4xx: 0 responses_5xx: 0
Complete requests: 20000 Failed requests: 0
  -c 50: connected=20024  parsed=20001  disconnected_before_request=23
  bytes received by those:      23 (0
  stats_mode: mutex total_connections: 20024 active_connections: 1 total_requests: 20000 responses_2xx: 20000 responses_4xx: 0 responses_5xx: 0
```

(Here `/stats` was read *once*, after the run, so it isn't counted in its own reading: `total_requests` = 20,000, while `parsed` = 20,001 includes that `/stats` request.)

</details>

---

# Test 5 — ThreadSanitizer on all three builds

**Question:** Week 3 doc Part 5. Does TSan find the race in the unsafe build, and stay silent on the other two?

Build three TSan versions (slower to build and run, used only for finding races):

```bash
cmake -S . -B build-tsan-unsafe -DPULSE_STATS_MODE=unsafe -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" && cmake --build build-tsan-unsafe
cmake -S . -B build-tsan-mutex  -DPULSE_STATS_MODE=mutex  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" && cmake --build build-tsan-mutex
cmake -S . -B build-tsan-atomic -DPULSE_STATS_MODE=atomic -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" && cmake --build build-tsan-atomic
```

For **each** of the three (shown for unsafe; swap the folder name for the other two):

```bash
# T1: TSan writes its reports to stderr, so send stderr to a file. Use /tmp, not /mnt/c
./build-tsan-unsafe/pulse 8080 2> /tmp/tsan-unsafe.log

# T2
ab -n 300 -c 10 http://localhost:8080/hello > /dev/null
curl -s localhost:8080/stats; echo
# then Ctrl+C Pulse in T1, and:
grep -c "WARNING: ThreadSanitizer: data race" /tmp/tsan-unsafe.log
grep -m1 -A12 "WARNING: ThreadSanitizer" /tmp/tsan-unsafe.log
```

| New piece | Meaning |
|---|---|
| `2> /tmp/tsan-unsafe.log` | Send **stderr** (stream 2) to a file. stdout still goes to the terminal |
| `grep -c` | **c**ount matching lines: here, how many separate race reports |
| `grep -m1 -A12` | Show the **first** match (`-m1`) plus the 12 lines **a**fter it: the first full report |

**Paste back:** for each build, the count and the first report (if any).

## Result (run by Claude, not by hand): TSan names every racing line in unsafe, and finds nothing in mutex or atomic

This test was run by Claude using the commands above (`ab -n 300 -c 10`), as a check rather than a hands-on exercise. Raw output is at the bottom.

<div class="tldr">
<b>What happened:</b> the unsafe build got <b>10 data-race reports</b>, pointing at <b>exactly the five lines</b> in <code>server_stats.cpp</code> that write the counters without protection. The mutex and atomic builds got <b>0</b>.
<br><br>
<b>Bonus:</b> even with only 300 requests, the unsafe TSan build's own <code>/stats</code> came out wrong on both runs, including one where <code>active_connections</code> read <b>18,446,744,073,709,551,615</b>: the counter went below zero.
</div>

<div class="stat-row">
<div class="stat-card">
<div class="label">Data race reports</div>
<div class="values"><span class="up">10</span> / <span class="flat">0</span> / <span class="flat">0</span></div>
<div class="note">unsafe / mutex / atomic</div>
</div>
<div class="stat-card">
<div class="label">Racing lines found (unsafe)</div>
<div class="values">5 of 5</div>
<div class="note">every counter write in server_stats.cpp</div>
</div>
<div class="stat-card">
<div class="label">Requests needed</div>
<div class="values">300</div>
<div class="note">vs 60,000 to see 1 lost update in Test 3</div>
</div>
</div>

### Every report, by line

TSan prints a one-line `SUMMARY` at the end of each report. All 10:

| Reports | `server_stats.cpp` line | The code | Called from |
|---|---|---|---|
| 2 | **34** | `total_connections_++;` | `on_connection_opened()`, i.e. the guard's constructor |
| 3 | **35** | `active_connections_++;` | `on_connection_opened()`, i.e. the guard's constructor |
| 1 | **40** | `active_connections_--;` | `on_connection_closed()`, i.e. the guard's destructor |
| 2 | **45** | `total_requests_++;` | `on_response()` |
| 2 | **46** | `... responses_2xx_++;` | `on_response()` |

That is **every** line in the unsafe block that writes a counter that was exercised (4xx/5xx weren't, since all 300 requests were 200s). TSan didn't need a lost update to happen to report these. It saw two threads touching the same memory with nothing ordering them. Compare Test 3, which needed 60,000 requests to show one lost `total_requests` update.

### Reading the first full report

```
WARNING: ThreadSanitizer: data race (pid=163251)
  Read of size 8 at 0x7fffffffe198 by thread T2:
    #0 ServerStats::on_connection_opened() src/server_stats.cpp:35
    #1 ActiveConnectionGuard::ActiveConnectionGuard(ServerStats&) include/server_stats.hpp:159
    #2 TcpServer::serve_connection(Socket) const src/tcp_server.cpp:296
    ...  (std::thread's internals, down to where the thread started)
  Previous write of size 8 at 0x7fffffffe198 by thread T1:
    #0 ServerStats::on_connection_closed() src/server_stats.cpp:40
    #1 ActiveConnectionGuard::~ActiveConnectionGuard() include/server_stats.hpp:160
    #2 TcpServer::serve_connection(Socket) const src/tcp_server.cpp:314
    ...
  Location is stack of main thread.
```

| Line | Meaning |
|---|---|
| `Read of size 8 … by thread T2` | Worker T2 **read** 8 bytes, one `uint64_t`, at address `0x7fffffffe198`: the "read" step of `active_connections_++` |
| `#0 … server_stats.cpp:35` → `#1 … server_stats.hpp:159` → `#2 … tcp_server.cpp:296` | **How it got there**, newest call first: `serve_connection()` line 296 created the guard, the guard's constructor (line 159) called `on_connection_opened()`, which ran `active_connections_++` (line 35). This is the "hidden" call explained earlier: TSan shows it explicitly |
| `Previous write of size 8 at 0x7fffffffe198 by thread T1` | A **different** worker, T1, had **written** the **same address**: the `active_connections_--` |
| `#0 … :40` → `#1 … :160` → `#2 … tcp_server.cpp:314` | T1's path: the closing `}` of `serve_connection()` (line 314) ran the guard's **destructor** (line 160), which called `on_connection_closed()` (line 40). TSan even shows the destructor running at the closing brace, where no code is written |
| `Location is stack of main thread` | Where that memory lives: **the main thread's stack**. That's `ServerStats stats;` in `main()` (Week 3 doc Part 8 / Week 2 doc Part 5: shared objects living in `main()`'s stack frame, reached by every worker through a reference). Same address in both accesses = same counter |

**In one sentence:** one worker starting (`++`) and another finishing (`--`) touched `active_connections` at the same time with no lock, which is exactly the counter Test 3 caught drifting to 7, 9, 15.

### The unsafe TSan build's own numbers were wrong too

Running under TSan slows every memory access down, which changes the timing and makes threads overlap more. Even at 300 requests, `/stats` came out wrong on **both** runs of the unsafe TSan build:

| | Run 1 (first attempt) | Run 2 | Correct |
|---|---|---|---|
| `active_connections` | **18446744073709551615** | **4** | 1 |
| `total_requests` | 300 | **299** | 300 |
| `responses_2xx` | **299** | 300 | 300 |

- **`18446744073709551615` is 2⁶⁴ − 1**, the largest number a `uint64_t` can hold. `active_connections` went **below zero**: a lost `+1` followed by both `−1`s took it from 0 to "−1", and an unsigned integer can't be negative, so it **wraps around** to its maximum. That's exactly the failure the comment in `server_stats.cpp`'s unsafe block warned about. A server checking "am I overloaded?" would think it had 18 quintillion connections.
- **`responses_2xx` ≠ `total_requests`**, in both directions. Both are incremented one line apart in `on_response()`, so a lost update in one but not the other breaks the "they always add up" invariant. In run 2, `2xx` (300) is even **greater than** `total_requests` (299), which a correct program can never produce.

The mutex and atomic TSan builds both read `active 1, requests 300, 2xx 300`: exact.

(Run 1 is from a first attempt that stalled: the test script couldn't stop the server, because `kill -INT` is ignored by programs started in the background of a script. Only its `/stats` reading is used here. Run 2 used `kill -TERM` and is the full run below. In your own terminal, Ctrl+C works as normal.)

### Why this test matters, even if it's just a check

| Way of finding the bug | What it needed | What it told us |
|---|---|---|
| Watching the numbers (Test 3) | 60,000 requests, and checking the *right* counter | *that* something is wrong |
| ThreadSanitizer (this test) | 300 requests | *exactly which lines*, *which threads*, *which variable*, *where it lives in memory* |

And a clean TSan run (mutex, atomic) is the evidence that the fix covers **every** access, not just the ones a particular load test happened to collide on.

<details>
<summary>Raw output: Test 5 (run by Claude)</summary>

```
built build-tsan-unsafe
built build-tsan-mutex
built build-tsan-atomic
################ unsafe
Complete requests:      300
Failed requests:        0
stats_mode: unsafe
total_connections: 301
active_connections: 4
total_requests: 299
responses_2xx: 300
responses_4xx: 0
responses_5xx: 0
data race warnings: 10
---- first full report:
WARNING: ThreadSanitizer: data race (pid=163251)
  Read of size 8 at 0x7fffffffe198 by thread T2:
    #0 ServerStats::on_connection_opened() src/server_stats.cpp:35 (pulse+0x1e708)
    #1 ActiveConnectionGuard::ActiveConnectionGuard(ServerStats&) include/server_stats.hpp:159 (pulse+0xfade)
    #2 TcpServer::serve_connection(Socket) const src/tcp_server.cpp:296 (pulse+0xe7f4)
    #3 void std::__invoke_impl<void, void (TcpServer::*)(Socket) const, TcpServer*, Socket>(std::__invoke_memfun_deref, void (TcpServer::*&&)(Socket) const, TcpServer*&&, Socket&&) /usr/include/c++/15/bits/invoke.h:76 (pulse+0x127d2)
    #4 std::__invoke_result<void (TcpServer::*)(Socket) const, TcpServer*, Socket>::type std::__invoke<void (TcpServer::*)(Socket) const, TcpServer*, Socket>(void (TcpServer::*&&)(Socket) const, TcpServer*&&, Socket&&) /usr/include/c++/15/bits/invoke.h:98 (pulse+0x1262e)
    #5 void std::thread::_Invoker<std::tuple<void (TcpServer::*)(Socket) const, TcpServer*, Socket> >::_M_invoke<0ul, 1ul, 2ul>(std::_Index_tuple<0ul, 1ul, 2ul>) /usr/include/c++/15/bits/std_thread.h:303 (pulse+0x124f2)
    #6 std::thread::_Invoker<std::tuple<void (TcpServer::*)(Socket) const, TcpServer*, Socket> >::operator()() /usr/include/c++/15/bits/std_thread.h:310 (pulse+0x1245e)
    #7 std::thread::_State_impl<std::thread::_Invoker<std::tuple<void (TcpServer::*)(Socket) const, TcpServer*, Socket> > >::_M_run() /usr/include/c++/15/bits/std_thread.h:255 (pulse+0x12410)
    #8 <null> <null> (libstdc++.so.6+0xf5ef8)
  Previous write of size 8 at 0x7fffffffe198 by thread T1:
    #0 ServerStats::on_connection_closed() src/server_stats.cpp:40 (pulse+0x1e782)
    #1 ActiveConnectionGuard::~ActiveConnectionGuard() include/server_stats.hpp:160 (pulse+0xfb3a)
    #2 TcpServer::serve_connection(Socket) const src/tcp_server.cpp:314 (pulse+0xe852)
    #3 ... (same std::thread frames as above)
  Location is stack of main thread.
SUMMARY lines (count, then line):
      2 SUMMARY: ThreadSanitizer: data race src/server_stats.cpp:34 in ServerStats::on_connection_opened()
      3 SUMMARY: ThreadSanitizer: data race src/server_stats.cpp:35 in ServerStats::on_connection_opened()
      1 SUMMARY: ThreadSanitizer: data race src/server_stats.cpp:40 in ServerStats::on_connection_closed()
      2 SUMMARY: ThreadSanitizer: data race src/server_stats.cpp:45 in ServerStats::on_response(int)
      2 SUMMARY: ThreadSanitizer: data race src/server_stats.cpp:46 in ServerStats::on_response(int)
################ mutex
Complete requests:      300
Failed requests:        0
stats_mode: mutex
total_connections: 301
active_connections: 1
total_requests: 300
responses_2xx: 300
responses_4xx: 0
responses_5xx: 0
data race warnings: 0
################ atomic
Complete requests:      300
Failed requests:        0
stats_mode: atomic
total_connections: 301
active_connections: 1
total_requests: 300
responses_2xx: 300
responses_4xx: 0
responses_5xx: 0
data race warnings: 0
```

Run 1's unsafe `/stats` (first attempt, which stalled at shutdown):

```
stats_mode: unsafe
total_connections: 301
active_connections: 18446744073709551615
total_requests: 300
responses_2xx: 299
responses_4xx: 0
responses_5xx: 0
```

(Paths shortened to `src/…`. The `(BuildId: …)` suffixes on each frame are removed for readability.)

</details>

---

# Test 6 — Scrambled log lines: gone?

**Question:** Week 2 / Test 6 sometimes had lines from two threads mixed into one. With `log_line()`, can that still happen?

```bash
# T1: the mutex build, log to /tmp, line-buffered (both Week 2 lessons)
stdbuf -oL ./build-mutex/pulse 8080 > /tmp/pulse3.log 2>&1

# T2
ab -n 5000 -c 20 http://localhost:8080/hello > /dev/null
echo "Parsed lines:          $(grep -o Parsed /tmp/pulse3.log | wc -l)"
echo "lines with 2+ TIDs:    $(grep -c 'TID.*TID' /tmp/pulse3.log)"
```

**Paste back:** T2.

## Where the log went

T1's command ends in `> /tmp/pulse3.log 2>&1`, so **nothing appears in T1**: everything Pulse prints goes into the file `/tmp/pulse3.log` instead of the screen. `/tmp` is inside WSL's own Linux filesystem, **not** in the Windows Pulse folder, so it doesn't show up in VS Code's file list. That's deliberate: Week 2 / Test 6 measured that logging to the Windows drive (`/mnt/c`) cuts throughput ~3.5×. To look at it from WSL:

```bash
ls -l /tmp/pulse3.log          # is it there, and how big?
head /tmp/pulse3.log           # first 10 lines
tail /tmp/pulse3.log           # last 10 lines
less /tmp/pulse3.log           # scroll through it (q to quit)
```

(Windows can also open it at `\\wsl$\Ubuntu\tmp\pulse3.log`.)

## Result: zero scrambled lines in 5,000 requests

The T2 output wasn't pasted, so the log file itself was analysed afterwards (it was still in `/tmp`, with Pulse still running).

<div class="tldr">
<b>What happened:</b> <b>5,000</b> requests, <b>5,000</b> <code>Parsed</code> lines, <b>0</b> lines containing two threads' output, and <b>0</b> lines that aren't a complete, well-formed log line. Every one of the 10,017 lines is whole.
<br><br>
<b>Compared with Week 2:</b> the same kind of test produced a line like <code>[TID [TID 549548] Parsed (] Parsed (87 bytes total): 87 bytes total)…</code>. With <code>log_line()</code> building each line privately and writing it under the log mutex, a line can no longer be split.
</div>

| Check on `/tmp/pulse3.log` | Result | Meaning |
|---|---|---|
| `Parsed` occurrences | **5,000** | one per request: every line made it into the file (`stdbuf -oL`, Week 2's lesson) |
| lines with 2+ `TID` tags | **0** | no worker's line was mixed with another's |
| lines with 2+ `Client connected` | **0** | no main-thread line was mixed either |
| lines **not** matching any known complete format | **0** | the strictest check: every line is exactly one of `Pulse listening…`, `Client connected: IP:port`, or `[TID n] Parsed / Client disconnected / …`, start to end |
| `Client connected` lines | 5,008 | 5,000 requests + **8** of `ab`'s unused extra connections (Test 4's finding, at `-c 20`) |
| `Client disconnected before completing a request (0 bytes …)` | **8** | exactly those 8 extras, which are the **last lines in the file**, closed by `ab` after the final response |

### Predicted vs actual

The `notes.txt` line was left blank. The design (Week 3 doc Part 9, `logger.hpp`) predicts 0.

| | Expected | Actual | Verdict |
|---|---|---|---|
| lines with 2+ TIDs | 0 | **0** | <span class="verdict-right">As expected</span> |

### Why "0" means more here than Week 2's "0"

Week 2 / Test 6 *also* happened to get 0 scrambled lines on one run, and a different run of the same test got 11. Back then, "0" was **luck**: nothing stopped the mixing, it just didn't happen that time (the Experiment 1 "98 / 2" lesson). Now it's **guaranteed by design**:

1. Each line is built completely in a local `std::ostringstream`, on the worker's own stack. Nothing is shared while it's being built.
2. `log_line()` writes the finished line with **one** `<<`, while holding `log_mutex`, so only one thread can be writing at any moment.

So no line can ever be split, on any run, at any load. What *can* still vary is the **order** of whole lines. Lines from different workers still interleave as whole lines, as in Week 2's Test 3. That's expected and fine.

### What the last lines show

```
[TID 169291] Client disconnected before completing a request (0 bytes received so far).
[TID 169290] Client disconnected before completing a request (0 bytes received so far).
[TID 169293] Client disconnected before completing a request (0 bytes received so far).
```

These are the end of the run: `ab` closing the connections it opened but never used (Test 4). They came from three different workers finishing at almost the same moment, in the order 169291, 169290, 169293 (not sorted, because finish order isn't creation order). Each one is still a complete, separate line.

<details>
<summary>Raw output: Test 6 (log file analysed afterwards)</summary>

```
-rw-r--r-- 1 aagam aagam 461028 Oct 24 10:19 /tmp/pulse3.log
first lines:
Pulse listening on port 8080... (stats mode: mutex)
Client connected: 127.0.0.1:33546
[TID 164289] Parsed (87 bytes total): GET /hello HTTP/1.0
Client connected: 127.0.0.1:33560
last lines:
[TID 169291] Client disconnected before completing a request (0 bytes received so far).
[TID 169290] Client disconnected before completing a request (0 bytes received so far).
[TID 169293] Client disconnected before completing a request (0 bytes received so far).
Client connected lines: 5008
Parsed lines:           5000
disconnected-before:    8
lines with 2+ TIDs:     0
lines with 2+ 'Client connected': 0
lines not matching any known format: 0
```

The "known format" check was:

```bash
grep -vcE '^(Pulse listening on port|Client connected: [0-9.]+:[0-9]+$|\[TID [0-9]+\] (Parsed|Client disconnected|Malformed|handler threw|connection error|read:|Request exceeded))' /tmp/pulse3.log
```

`-v` = lines that do **not** match. `-c` = count them. `-E` = extended regex. `^` = the match must start at the beginning of the line. So this counts lines that don't **start** exactly like one of Pulse's log lines.

</details>

---

# Test 7 — Mutex vs atomic vs unsafe: throughput

**Question:** the spec's "do NOT conclude atomics are always faster. Measure." The doc's microbenchmark had atomic about 3× faster than mutex. Does that show up in Pulse? (Only run this *after* Tests 3–5, because the unsafe build's speed doesn't count if its numbers are wrong.)

```bash
# For each build, one at a time. T1:
./build-unsafe/pulse 8080 > /dev/null 2>&1      # then mutex, then atomic

# T2, twice per build:
ab -n 20000 -c 10 http://localhost:8080/hello 2>&1 | grep -E "Requests per second|Failed"
```

Run all three builds back to back in one session (same conditions). Then do one more round of all three, to see how much the numbers move between runs of the *same* build.

**Paste back:** all six results.

## Result: all three builds run at the same speed, and that's the point

<div class="tldr">
<b>What happened:</b> every run came out at <b>~8,000–8,500 requests/s</b>, whichever build. The differences between unsafe, mutex and atomic are <b>smaller than the difference between two runs of the same build</b>. In Pulse, the choice of mutex vs atomic has <b>no measurable effect on speed</b>.
<br><br>
<b>Why:</b> each request makes <b>3</b> counter updates, costing tens of nanoseconds each. The rest of the request (a new thread, <code>accept</code>, <code>read</code>, parse, route, <code>write</code>, <code>close</code>, two log lines) costs <b>thousands</b> of times more. The doc's microbenchmark, where atomic was ~3× faster than mutex, measured a loop doing <i>nothing but</i> incrementing. In a real server, that difference disappears into everything else.
<br><br>
<b>So the choice is made on correctness, not speed:</b> mutex, because it keeps the <code>/stats</code> snapshot consistent (Test 4, and the decisions at the top of this file). The unsafe build isn't "free speed": it's the same speed, with wrong numbers.
</div>

### The results

**Your run** (six `ab -n 20000 -c 10` runs, log to `/dev/null`). The pasted output doesn't show which build each line came from, because the `stats mode:` line was printed in T1:

| Run | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| req/s | 8,172.67 | 8,341.75 | 8,372.38 | 8,517.66 | 8,456.96 | 8,489.93 |
| failed | 0 | 0 | 0 | 0 | 0 | 0 |

All six within **8,173–8,518**, a spread of about 4%.

**A labelled run** (by Claude, in the same session, immediately afterwards: each build started fresh on its own port, log to `/dev/null`, `ab -n 20000 -c 10`):

| | unsafe | mutex | atomic |
|---|---|---|---|
| Round 1 | 8,494 | 8,367 | 8,357 |
| Round 2 | 8,274 | 8,392 | 8,048 |
| **Spread within the same build** | 220 (2.6%) | 25 (0.3%) | **309 (3.7%)** |

- **No consistent winner.** Unsafe was fastest in round 1. Mutex was fastest in round 2.
- **Run-to-run noise is as big as the gap between builds.** Atomic alone moved 309 req/s between two identical runs. The largest gap between two *different* builds in the same round was 344 (round 2: mutex vs atomic). Any "winner" here is noise.
- **Every run: 0 failed.**

### Predicted vs actual

The `notes.txt` lines were left blank.

| | Expected (Week 3 doc, Part 7) | Actual | Verdict |
|---|---|---|---|
| Atomic ~3× faster than mutex, as in the microbenchmark? | No: the doc warned the microbenchmark wouldn't carry over | All three the same, within noise | <span class="verdict-right">As the doc warned</span> |
| The unsafe build faster, since it does no locking? | No measurable difference | Same speed | <span class="verdict-right">As expected</span> |

### Why the counter method doesn't show up: the arithmetic

**Cost of the stats per request.** Each request makes **3** counter updates: `on_connection_opened` (guard constructor), `on_response`, `on_connection_closed` (guard destructor). From the Week 3 doc's microbenchmark (4 million increments):

| | Time per increment, worst case (4 threads fighting over one counter) |
|---|---|
| unprotected | 0.044 s ÷ 4,000,000 ≈ **11 ns** |
| atomic | 0.054 s ÷ 4,000,000 ≈ **14 ns** |
| mutex | 0.17 s ÷ 4,000,000 ≈ **43 ns** |

So even mutex, the slowest, costs about **3 × 43 ns ≈ 0.13 µs per request**, and that's in a worst-case pile-up. In Pulse, workers rarely hit the stats mutex at the same instant (Test 3 needed 60,000 requests to lose one `total_requests` update), so it's usually less.

**Cost of everything else per request.** At ~8,400 req/s with 10 in flight, each request takes about 10 ÷ 8,400 ≈ **1.2 ms** from the client's side. Even counting only the server's own CPU work per request, it's tens to hundreds of **microseconds**: creating and destroying a thread (Week 1 / Experiment 3: ~1 ms of kernel time per ~100 threads), the `accept` / `read` / `write` / `close` system calls, parsing, routing, building the response, and two `log_line()` calls.

**Ratio:** about 0.13 µs out of at least tens of µs, so **well under 1%** of each request, and run-to-run noise here is **~4%**. A difference of under 1% can't be seen through 4% of noise. That's why all three builds look identical.

The doc's microbenchmark wasn't *wrong*. It measured the counter in isolation, where the counter is 100% of the work. **Where an operation sits in the real program decides whether its cost matters.** That's the spec's "do not conclude atomics are always faster: measure", demonstrated.

### A side observation: today's numbers are much higher than earlier ones

| When | Same kind of test (`ab -c 10`, log to `/dev/null`) | req/s |
|---|---|---|
| Week 2 write-up (2026-10-18) | Week 2 build | ~4,300 |
| Week 3 implementation check (2026-10-23) | all three Week 3 builds | ~3,200 |
| **This test (2026-10-24)** | all three Week 3 builds | **~8,000–8,500** |

The code didn't get 2.5× faster overnight: the Week 3 builds measured on 10-23 and today are the same code. What changed is the **machine's conditions**: this is a laptop running WSL2 under Windows, so CPU boost speed (plugged in or not, heat), Windows' power mode, and whatever else Windows is doing all move the numbers a lot. This wasn't measured, so it's a likely explanation rather than a proven one.

**The lesson, which Week 4 depends on:** a throughput number only means something **compared with another number from the same session**. Today's 8,400 can't be compared with Month 1's 5,380 (a different day), or with Week 3's 3,200 (a different day). That's why Test 7 runs every build back to back, and why Week 4's Month 1 vs Month 2 benchmark must run **both versions in the same sitting**.

<details>
<summary>Raw output: Test 7</summary>

**Your run**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ab -n 20000 -c 10 http://localhost:8080/hello 2>&1 | grep -E "Requests per second|Failed"
Failed requests:        0
Requests per second:    8172.67 [#/sec] (mean)
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ab -n 20000 -c 10 http://localhost:8080/hello 2>&1 | grep -E "Requests per second|Failed"
Failed requests:        0
Requests per second:    8341.75 [#/sec] (mean)
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ab -n 20000 -c 10 http://localhost:8080/hello 2>&1 | grep -E "Requests per second|Failed"
Failed requests:        0
Requests per second:    8372.38 [#/sec] (mean)
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ab -n 20000 -c 10 http://localhost:8080/hello 2>&1 | grep -E "Requests per second|Failed"
Failed requests:        0
Requests per second:    8517.66 [#/sec] (mean)
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ab -n 20000 -c 10 http://localhost:8080/hello 2>&1 | grep -E "Requests per second|Failed"
Failed requests:        0
Requests per second:    8456.96 [#/sec] (mean)
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ab -n 20000 -c 10 http://localhost:8080/hello 2>&1 | grep -E "Requests per second|Failed"
Failed requests:        0
Requests per second:    8489.93 [#/sec] (mean)
```

**Labelled run (Claude, same session)**

```
round 1  unsafe: 8493.98
round 1  mutex: 8366.72
round 1  atomic: 8357.37
round 2  unsafe: 8274.27
round 2  mutex: 8392.59
round 2  atomic: 8047.66
```

</details>

---

# Week 3 done when

- [x] Shared statistics added: total and active connections, total requests, 2xx / 4xx / 5xx, plus `GET /stats`
- [x] Built three ways, unsafe → mutex → atomic, the spec's order, from one codebase (`PULSE_STATS_MODE`)
- [x] **Unsafe counter observed going wrong** (Test 3: 1 lost request count, `active_connections` 7 → 9 → 15; Test 5: active wrapped to 2⁶⁴ − 1)
- [x] **Mutex and atomic exact** under the same load (Test 4)
- [x] ThreadSanitizer: 10 races in unsafe (every counter write), 0 in mutex and atomic (Test 5)
- [x] `active_connections` returns to 1 on every exit path, via RAII (Tests 2, 4)
- [x] 5xx responses exist and are counted (`/fail` → 500, Test 1)
- [x] Log lines can no longer be split (Test 6: 0 in 5,000 requests, guaranteed by design)
- [x] Mutex vs atomic measured in Pulse: no difference within noise (Test 7)
- [x] Shared state documented (`tcp_server.hpp`'s inventory, `server_stats.hpp`'s decisions)
- [ ] Predictions written before running: only Test 3's was given (in chat). Worth doing in Week 4, where the questions are numbers
- **Default chosen: `mutex`.** Same speed as atomic (Test 7), and it keeps `/stats` consistent: `total_requests` always equals `2xx + 4xx + 5xx` in any snapshot (Tests 1, 2)
- Carried forward to **Week 4**:
    - thread limits and stress: what happens at 100s and 1000s of connections
    - the Month 1 vs Month 2 benchmark, **both run in the same session** (Test 7's lesson)
