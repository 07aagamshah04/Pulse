---
title: "Pulse — Month 2, Week 2: Test Log"
subtitle: "Thread-per-connection Pulse: what changed, how to test it, what happened"
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

Same as Week 1's `notes.html`: for each test, write predictions in `notes.txt` first, run the commands, paste the full raw output into the chat, and the results get written up here.

| # | Test | Status |
|---|---|---|
| 1 | Experiment 0 again: silent client + `/hello` | **Done** |
| 2 | `/slow` + `/hello` at the same time | **Done** |
| 3 | 5 × `/slow` in parallel: threads and fds | **Done** |
| 4 | A client that resets mid-request (error handling) | **Done** |
| 5 | Nothing broke: every Month 1 behavior | **Done** (automated run) |
| 6 | `ab -c 10` + scrambled logs | **Done** |

---

# What changed in the code

Every change is commented in the code with `MONTH 2 / WEEK 2`. Read those comments alongside this table.

| File | Change | Why (Week 2 doc) |
|---|---|---|
| `CMakeLists.txt` | `find_package(Threads REQUIRED)` + `target_link_libraries(pulse PRIVATE Threads::Threads)` | `std::thread` needs the pthreads library (Part 8) |
| `src/tcp_server.cpp`, `run()` | `handle_client(Socket(client_fd));` became `Socket client(client_fd);` + `std::thread(&TcpServer::serve_connection, this, std::move(client)).detach();`, inside a `try/catch` | The main thread only accepts and hands over. The `Socket` is **moved** into the thread, so the main thread can't close it (Parts 2–4). The `try/catch` drops just that connection if a thread can't be created |
| `src/tcp_server.cpp`, new `serve_connection()` | The function every worker starts in: calls `handle_client()` inside `try { } catch (const std::exception&) { } catch (...) { }` | An exception escaping a thread would `std::terminate()` the whole server, and `main`'s `catch` can't see it (Part 6) |
| `src/tcp_server.cpp`, `handle_client()` | Body unchanged. Log lines now start with `[TID n]` | Its locals already live on whichever thread runs it. The TID tells you which worker printed what |
| `include/tcp_server.hpp` | Declares `serve_connection()`. Class comment updated | |
| `src/main.cpp` | `std::signal(SIGPIPE, SIG_IGN);` at the start of `main()` | So a client disconnecting at the wrong moment can't kill the whole process (Part 6) |
| `src/main.cpp` | New route `GET /slow`: sleeps 5 s, then responds | The spec's test: a slow handler must not block other requests (Part 7) |
| `Month2/Week2/reset_client.py` | New test client: sends `/slow`, then resets the connection | Exercises the error path (Test 4) |

**Build** (in WSL, from the Pulse folder). `make` notices `CMakeLists.txt` changed and re-runs CMake by itself:

```bash
cd /mnt/c/Users/aagam/OneDrive/Desktop/Pulse/build && cmake .. && make
```

You should see `[100%] Built target pulse` with **no warnings**.

---

# Test 1 — Experiment 0 again: silent client + `/hello`

**Question:** Week 1 / Experiment 0's exact steps. Does one silent client still freeze everyone?

**Week 1's result, for comparison:** `/hello` timed out after 30.04 s, `Threads: 1`, `S wait_woken`, LISTEN `Recv-Q 1`, curl's `Recv-Q 83`.

Four terminals, all in WSL in the Pulse folder:

```bash
# T1: run Pulse
cd build && ./pulse 8080

# T2: the silent client: connect, send nothing, leave it open
nc localhost 8080

# T3: normal request, 30 s limit
time curl -s -m 30 -w "\nhttp_code=%{http_code}\n" localhost:8080/hello; echo "curl exit code: $?"

# T4: look inside, while nc is still connected
P=$(pgrep -x pulse)
grep Threads /proc/$P/status
ps -L -o tid,stat,wchan:22,comm -p $P
ss -ltn "sport = :8080"
ss -tn state established "sport = :8080"
```

Then `Ctrl+C` the `nc` in T2 and look at T1's log.

**Paste back:** T1, T3, T4.

## Result: one silent client no longer freezes Pulse

<div class="tldr">
<b>What I did:</b> exactly Week 1's Experiment 0: a silent <code>nc</code> client connected and sending nothing, then a normal <code>curl /hello</code>.
<br><br>
<b>What happened:</b> <code>/hello</code> answered in <b>0.018 s</b> with <code>200</code>. In Week 1 the same request waited <b>30.04 s</b> and got nothing. Pulse now has <b>2 threads</b>: the main thread back in <code>accept()</code>, and one worker stuck in <code>read()</code> on the silent client, stuck <b>alone</b>.
</div>

<div class="stat-row">
<div class="stat-card">
<div class="label">curl /hello with nc connected</div>
<div class="values">30.04 s <span class="flat">&rarr; 0.018 s</span></div>
<div class="note">Week 1 (timed out, no reply) vs Week 2 (200 OK)</div>
</div>
<div class="stat-card">
<div class="label">Threads</div>
<div class="values">1 &rarr; 2</div>
<div class="note">main + one worker for the silent client</div>
</div>
<div class="stat-card">
<div class="label">LISTEN Recv-Q</div>
<div class="values">1 <span class="flat">&rarr; 0</span></div>
<div class="note">nobody stuck waiting to be accepted</div>
</div>
</div>

### Week 1 vs Week 2, field by field

Week 1's `notes.html` has a table, *"What Week 2 should show instead"*, written **before** any Week 2 code existed. Here it is next to what actually happened:

| | Week 1 (measured) | Week 2: expected (written in Week 1) | Week 2: actual | Verdict |
|---|---|---|---|---|
| `curl /hello` with `nc` connected | 30.044 s, `http_code=000`, exit code `28` | ≈ 0.02 s, `200` | **0.018 s, `200`, exit code `0`** | <span class="verdict-right">As expected</span> |
| `Threads:` | 1 | at least 2 | **2** | <span class="verdict-right">As expected</span> |
| `ps -L` | one row: `S wait_woken` | main in `accept()`, nc's thread in `read()` | **`837 Sl+ inet_csk_accept`** and **`857 Sl+ wait_woken`** | <span class="verdict-right">As expected</span> |
| LISTEN `Recv-Q` | 1 (curl stuck in the accept queue) | 0 | **0** | <span class="verdict-right">As expected</span> |
| curl's connection `Recv-Q` | 83 (request never read) | 0 (read immediately) | **not in the list at all**: already answered and closed | <span class="verdict-right">Better than expected</span> (below) |

(No Test 1 predictions were written in `notes.txt`, so the "expected" column from Week 1 is the prediction being tested here.)

### Reading each output

**`time curl ...`**

```
Hello from Pulse!
http_code=200
real    0m0.018s
curl exit code: 0
```

A normal, instant answer: 18 ms, status `200`, exit code `0` (success). Week 1's run of this exact command, with the same silent client connected, sat for 30 s and got exit code `28` (timeout).

**`grep Threads` → `Threads: 2`**

| Thread | Why it exists |
|---|---|
| 1. main thread | Every process starts with one. Pulse's runs `run()`'s accept loop |
| 2. the silent client's worker | Created when `nc` connected. It's still alive because `nc` hasn't sent anything, so `handle_client` is still waiting in `read()` |

curl's worker isn't counted. It was created, answered `/hello`, closed the connection and **ended**, all within those 18 ms, before you ran T4. In Week 2, the thread count is "1 + the number of connections being handled *right now*", and finished connections leave no thread behind.

**`ps -L -o tid,stat,wchan:22,comm`**

```
    TID STAT WCHAN                  COMMAND
    837 Sl+  inet_csk_accept        pulse      ← main thread: waiting in accept() for the NEXT client
    857 Sl+  wait_woken             pulse      ← nc's worker: waiting in read() for bytes that never come
```

- **TID 837 = the PID**, so it's the main thread (Month 2 / Week 1 doc, Part 6). It's in `inet_csk_accept`, the kernel side of `accept()`, the same as idle Month 1 Pulse in Week 2 doc Part 1. **It's back in `accept()` even though a client is stuck.** That's the whole fix in one row.
- **TID 857, the worker, is in `wait_woken`.** That's exactly the wait channel Month 1 Pulse's *only* thread was stuck in during Week 1's Experiment 0. Same wait, same place in `handle_client` (the `::read()` call). The difference is that this time it's a worker stuck there, not the whole server.
- **`STAT` is now `Sl+`, not `S+`.** Compare Week 1's Experiment 0: `773 S+ wait_woken`. The new **`l`** means "this process is multi-threaded". It appeared because Pulse now has more than one thread. It's the same `l` you saw on every thread in Week 1's Experiments 2 and 3.
- **The gap between 837 and 857:** TIDs come from **one counter shared by the whole system**. Every new process *and* every new thread takes the next number. Between Pulse starting (837) and `nc` connecting (857), other things on the machine took the numbers in between. Opening a terminal, starting `nc`, each one gets a PID from the same counter. (curl's worker is *not* in that gap. T1's log below shows it was TID 866, created after nc's.)

**`ss -ltn "sport = :8080"`**

```
LISTEN    0          128        0.0.0.0:8080      0.0.0.0:*
```

`Recv-Q 0`: **no finished connections waiting to be `accept()`ed**. In Week 1 this was `1`: curl's connection, completed by the kernel but never picked up, because Pulse's only thread was stuck in `read()`. Now the main thread `accept()`s each connection immediately and hands it off, so nothing piles up in the queue.

**`ss -tn state established "sport = :8080"`**

```
Recv-Q      Send-Q     Local Address:Port      Peer Address:Port
0           0          127.0.0.1:8080          127.0.0.1:40140      ← nc, still connected, still silent
```

**Only one row now.** Week 1 had two: `nc` (`Recv-Q 0`) and curl (**`Recv-Q 83`**, its whole request sitting unread). This time curl's connection had already been accepted, read, answered and closed before you ran this command, so it's not in the list at all. That's why the last row of the comparison table says "better than expected": the Week 1 table predicted curl's `Recv-Q` would show `0`, but in fact the connection was completely gone.

`nc`'s row is unchanged from Week 1: connected, `Recv-Q 0` (it sent nothing). Its worker (TID 857) is waiting in `read()` for it.

### What this proves

> **A slow client now blocks only its own thread.** The kernel side is unchanged: connections still complete the handshake on their own, and data still waits in `Recv-Q`. What changed is that Pulse always has a thread free to `accept()` and a thread per client to `read()`. Experiment 0's 30-second freeze is gone.

### T1: Pulse's own log, matched to the threads

```
Pulse listening on port 8080...
Client connected: 127.0.0.1:40140
Client connected: 127.0.0.1:50454
[TID 866] Parsed (83 bytes total): GET /hello HTTP/1.1
Client connected: 127.0.0.1:58778
[TID 933] Parsed (83 bytes total): GET /hello HTTP/1.1
[TID 857] Client disconnected before completing a request (0 bytes received so far).
```

| Client port | Who | Worker TID | What happened |
|---|---|---|---|
| `40140` | `nc` (the silent client) | **857** | Connected first. Its worker went into `read()` and stayed there. The same TID 857 shows up as `wait_woken` in T4's `ps -L` |
| `50454` | the `time curl ... /hello` from T3 | **866** | Connected **while nc's worker was stuck**, got its own thread, parsed its 83-byte request and was answered at once: the 0.018 s |
| `58778` | a second `curl /hello` | **933** | Also answered while nc was still connected. More proof the server stays open to everyone |
| `40140` again | `nc`, after `Ctrl+C` | **857** | `read()` finally returned 0 ("client closed"), so worker 857 logged the disconnect, closed the socket and ended |

What this log shows that Week 1's couldn't:

1. **Clients are answered while another is stuck.** In Week 1 the log order was: nc connected → *30 s of nothing* → nc disconnected → only then curl got `Client connected`. Here, `Client connected: 50454` and its `Parsed` line come **right after** nc connects, while nc's worker is still waiting.
2. **Each line says which thread wrote it.** `Client connected` lines have no TID: they're printed by the **main** thread, in `run()`, right after `accept()`. `Parsed` / `Client disconnected` lines carry the **worker's** TID, because they're printed inside `handle_client()` on that worker.
3. **The nc worker's lifetime is visible from start to end.** It was created when `nc` connected (port 40140), showed up as TID 857 `wait_woken` in `ps -L`, and its *last* line is the disconnect. After that, the thread ended. It's the only worker that lived longer than a few milliseconds.
4. **TIDs jump (866 → 933)** for the same reason as the 837 → 857 gap above: the commands you ran in T4 (`pgrep`, `grep`, `ps`, `ss`) are each new processes, and each took numbers from the same shared counter in between.
5. **"83 bytes total"** is the same 83 bytes Week 1 saw sitting unread in curl's `Recv-Q`. This time they were read immediately.

<details>
<summary>Raw output: Test 1</summary>

**T1 (Pulse)**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse/build$ ./pulse 8080
Pulse listening on port 8080...
Client connected: 127.0.0.1:40140
Client connected: 127.0.0.1:50454
[TID 866] Parsed (83 bytes total): GET /hello HTTP/1.1
Client connected: 127.0.0.1:58778
[TID 933] Parsed (83 bytes total): GET /hello HTTP/1.1
[TID 857] Client disconnected before completing a request (0 bytes received so far).
```

**T3**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ time curl -s -m 30 -w "\nhttp_code=%{http_code}\n" localhost:8080/hello; echo "curl exit code: $?"
Hello from Pulse!
http_code=200

real    0m0.018s
user    0m0.000s
sys     0m0.014s
curl exit code: 0
```

**T4**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ P=$(pgrep -x pulse)
grep Threads /proc/$P/status
ps -L -o tid,stat,wchan:22,comm -p $P
ss -ltn "sport = :8080"
ss -tn state established "sport = :8080"
Threads:        2
    TID STAT WCHAN                  COMMAND
    837 Sl+  inet_csk_accept        pulse
    857 Sl+  wait_woken             pulse
State     Recv-Q     Send-Q         Local Address:Port         Peer Address:Port
LISTEN    0          128                  0.0.0.0:8080              0.0.0.0:*
Recv-Q      Send-Q           Local Address:Port             Peer Address:Port
0           0                    127.0.0.1:8080                127.0.0.1:40140
```

</details>

---

# Test 2 — `/slow` + `/hello` at the same time

**Question:** the spec's key experiment. Can `/hello` finish while `/slow` is still running?

```bash
# T2: start a slow request (takes 5 s)
time curl -s localhost:8080/slow; echo

# T3: within those 5 seconds:
time curl -s localhost:8080/hello; echo
ps -L -o tid,stat,wchan:22,comm -p $(pgrep -x pulse)
```

**Paste back:** T1, T2, T3.

## Result: `/hello` finishes while `/slow` is still sleeping

<div class="tldr">
<b>What I did:</b> started <code>curl /slow</code> (the handler sleeps 5 s), and while it was still waiting, ran <code>curl /hello</code> and <code>ps -L</code>.
<br><br>
<b>What happened:</b> <code>/hello</code> answered in <b>0.030 s</b> while <code>/slow</code> was still asleep. <code>/slow</code> took its full <b>5.08 s</b>, but only for itself. <code>ps -L</code> caught the <code>/slow</code> worker asleep in <b><code>hrtimer_nanosleep</code></b>, the same wait as Week 1's Experiment 3 sleeping threads, now inside Pulse. This is the spec's "one of the most important experiments of Month 2", and it passed.
</div>

<div class="stat-row">
<div class="stat-card">
<div class="label">/slow</div>
<div class="values">5.08 s</div>
<div class="note">its own 5 s sleep + ~0.08 s</div>
</div>
<div class="stat-card">
<div class="label">/hello, started during /slow</div>
<div class="values"><span class="flat">0.030 s</span></div>
<div class="note">didn't wait for /slow at all</div>
</div>
<div class="stat-card">
<div class="label">/slow worker's WCHAN</div>
<div class="values" style="font-size:1.1em">hrtimer_nanosleep</div>
<div class="note">= Experiment 3's sleeping threads</div>
</div>
</div>

### Predicted vs actual

No Test 2 predictions were written in `notes.txt`. The predictions being tested are the Week 2 concepts doc's (Part 7), written before the code:

| | Predicted (Week 2 doc, Part 7) | Actual | Verdict |
|---|---|---|---|
| `/slow` time | ~5 s | **5.077 s** | <span class="verdict-right">As predicted</span> |
| `/hello` during `/slow` | ~0.02 s, not waiting behind `/slow` | **0.030 s** | <span class="verdict-right">As predicted</span> |
| `/slow` worker's `WCHAN` | `hrtimer_nanosleep` | **`hrtimer_nanosleep`** (TID 1106) | <span class="verdict-right">As predicted</span> |
| main thread's `WCHAN` | `inet_csk_accept` | **`inet_csk_accept`** (TID 837) | <span class="verdict-right">As predicted</span> |

The doc's predicted *Month 1* behavior (`/hello` waiting ~5 s behind `/slow`) wasn't measured, because Month 1 Pulse has no `/slow` route. The measured Month 1 equivalent is Week 1's Experiment 0: one thread stuck on one client froze every other request for 30 s.

### Reading each output

**T2: `time curl -s localhost:8080/slow`**

```
Slow response from Pulse (slept 5 s)
real    0m5.077s
user    0m0.012s
sys     0m0.029s
```

- **`real 5.077 s`**: the handler's `sleep_for(5 s)` plus ~77 ms for everything else (connect, send, the worker's parse and write, and starting `curl` itself).
- **`user` + `sys` ≈ 0.04 s**: curl was asleep for almost the whole 5 seconds, waiting for the response. The same "waiting, not working" pattern as Week 1's Experiment 0, but this time it's curl waiting on a deliberately slow handler, not a blocked server.

**T3: `time curl -s localhost:8080/hello`**

```
Hello from Pulse!
real    0m0.030s
```

**30 ms** while `/slow`'s worker was asleep. A little slower than Test 1's 18 ms. At this size, a few milliseconds is normal run-to-run variation (starting a new `curl` process is most of it), not a sign of waiting on `/slow`. If `/hello` had been waiting behind `/slow`, this would read about 5 seconds.

**T3: `ps -L -o tid,stat,wchan:22,comm`**, taken while `/slow` was still sleeping:

```
    TID STAT WCHAN                  COMMAND
    837 Sl+  inet_csk_accept        pulse      ← main thread: back in accept()
   1106 Sl+  hrtimer_nanosleep      pulse      ← /slow's worker: asleep inside the handler
```

- **TID 837** is the same main thread as in Test 1. It's the same Pulse process, still running. Still in `accept()`.
- **TID 1106** is `/slow`'s worker. The log line `[TID 1106] Parsed (82 bytes total): GET /slow` confirms it. Its `WCHAN` is **`hrtimer_nanosleep`**: the kernel's high-resolution timer sleep, which is what `std::this_thread::sleep_for` uses. That's exactly the wait channel of the 100 threads in Week 1 / Experiment 3 Part C (`100 hrtimer_nanosleep`). **The `/slow` worker is an Experiment 3 thread living inside Pulse:** state `S`, using no CPU, waiting for a timer.
- **`/hello`'s worker (TID 1113) isn't in the list.** It had already answered and ended in those 30 ms, before `ps` ran. (Same as curl's worker in Test 1.)

**T1: Pulse's log**

```
Client connected: 127.0.0.1:47152
[TID 1106] Parsed (82 bytes total): GET /slow HTTP/1.1
Client connected: 127.0.0.1:47156
[TID 1113] Parsed (83 bytes total): GET /hello HTTP/1.1
```

| Line | Thread | Meaning |
|---|---|---|
| `Client connected: …:47152` | main | `/slow`'s connection accepted and handed to a new worker |
| `[TID 1106] Parsed (82 bytes total): GET /slow` | worker 1106 | The request was **fully read and parsed straight away**. Only *then* does the handler sleep. 82 bytes, not 83, because `/slow` is one character shorter than `/hello` |
| `Client connected: …:47156` | main | `/hello` accepted **while worker 1106 is asleep**. The main thread was free, because it never runs handlers |
| `[TID 1113] Parsed (83 bytes total): GET /hello` | worker 1113 | Its own thread, its own parse, answered immediately |

There's no "finished" line for `/slow`, because `handle_client` doesn't log when a response is sent, only when a request is parsed. The 5.077 s in T2 is the evidence that it completed.

### Silent client (Test 1) vs slow handler (Test 2)

The Week 2 doc (Part 7) said these were two different *kinds* of slow, and `ps -L` and the log show exactly where each one waits:

| | Test 1: silent client | Test 2: `/slow` handler |
|---|---|---|
| Log before the wait | `Client connected` only, **no** `Parsed` line (nothing arrived to parse) | `Client connected` **and** `Parsed … GET /slow` (request complete) |
| Where the worker waits | in `read()`, inside `handle_client`'s read loop | in the handler, inside `router_.route()` |
| `WCHAN` | `wait_woken`: waiting for the **network** | `hrtimer_nanosleep`: waiting for a **timer** |
| Whose "fault" | the client's | the server's (stands in for a slow database query or slow computation) |
| Effect on other requests in Week 2 | none | none |

> **Both kinds of slow now block only their own thread.** This was the spec's central Week 2 deliverable: *"slow handlers without blocking every other connection."*

<details>
<summary>Raw output: Test 2</summary>

**T1 (Pulse log)**

```
Client connected: 127.0.0.1:47152
[TID 1106] Parsed (82 bytes total): GET /slow HTTP/1.1
Client connected: 127.0.0.1:47156
[TID 1113] Parsed (83 bytes total): GET /hello HTTP/1.1
```

**T2**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ time curl -s localhost:8080/slow; echo
Slow response from Pulse (slept 5 s)
real    0m5.077s
user    0m0.012s
sys     0m0.029s
```

**T3**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ time curl -s localhost:8080/hello; echo
ps -L -o tid,stat,wchan:22,comm -p $(pgrep -x pulse)
Hello from Pulse!
real    0m0.030s
user    0m0.011s
sys     0m0.008s

    TID STAT WCHAN                  COMMAND
    837 Sl+  inet_csk_accept        pulse
   1106 Sl+  hrtimer_nanosleep      pulse
```

</details>

---

# Test 3 — 5 × `/slow` in parallel: threads and file descriptors

**Question:** does Pulse really make one thread per connection, and do those threads and fds go away when the connections finish?

All in one terminal (T2), with Pulse still running in T1:

```bash
P=$(pgrep -x pulse)
echo "BEFORE:"; grep Threads /proc/$P/status; ls /proc/$P/fd

for i in 1 2 3 4 5; do curl -s -o /dev/null -w "slow #$i done in %{time_total} s\n" localhost:8080/slow & done
sleep 1
echo "DURING:"; grep Threads /proc/$P/status; ls /proc/$P/fd
ps -L -o stat=,wchan:22= -p $P | sort | uniq -c
wait

sleep 0.5
echo "AFTER:"; grep Threads /proc/$P/status; ls /proc/$P/fd
```

| New piece | Meaning |
|---|---|
| `curl ... &` inside the `for` loop | `&` starts each curl **in the background** and immediately moves on, so all 5 run at the same time |
| `-w "... %{time_total} s"` | curl prints how long *that* request took |
| `ls /proc/$P/fd` | The process's open file descriptors: 0/1/2 = stdin/out/err, 3 = listening socket, 4+ = client connections |
| `wait` | Wait for this shell's background jobs (the 5 curls) to finish. Pulse is in a different terminal, so it's not one of them |

**Paste back:** all of T2, plus T1's log lines from this test.

## Result: one thread per connection, and every thread and fd cleaned up afterwards

<div class="tldr">
<b>What I did:</b> fired 5 <code>/slow</code> requests at the same time, and looked at Pulse's threads and open file descriptors before, during and after.
<br><br>
<b>What happened:</b> all 5 finished in <b>~5.00 s each, about 5 s total</b>, not 25 s. During the requests Pulse had <b>6 threads</b> (main + one per connection) and <b>5 extra fds</b> (4–8, one socket per client). Afterwards it was back to <b>exactly 1 thread and fds 0–3</b>, so nothing leaked. Every connection was closed exactly once, by its own worker.
</div>

<div class="stat-row">
<div class="stat-card">
<div class="label">Threads: before / during / after</div>
<div class="values">1 &rarr; 6 <span class="flat">&rarr; 1</span></div>
<div class="note">main + 5 workers, then back to main only</div>
</div>
<div class="stat-card">
<div class="label">Open fds: before / during / after</div>
<div class="values">0–3 &rarr; 0–8 <span class="flat">&rarr; 0–3</span></div>
<div class="note">5 client sockets opened, all 5 closed</div>
</div>
<div class="stat-card">
<div class="label">5 × /slow</div>
<div class="values"><span class="flat">5.00 s each</span></div>
<div class="note">side by side, not 5 × 5 = 25 s</div>
</div>
</div>

### Predicted vs actual

No Test 3 predictions were written in `notes.txt`. The expectations being tested come from the Week 2 concepts doc (Parts 2, 3 and 8):

| | Expected (Week 2 doc) | Actual | Verdict |
|---|---|---|---|
| Time for 5 × `/slow` | ~5 s total: they sleep side by side | each **5.002–5.005 s**, all done ~5 s after starting | <span class="verdict-right">As expected</span> |
| `Threads:` during | 1 + one per connection = 6 | **6** | <span class="verdict-right">As expected</span> |
| `Threads:` after | back to 1: finished workers end | **1** | <span class="verdict-right">As expected</span> |
| fds during | 3 + five client sockets (4, 5, 6, 7, 8) | **0 1 2 3 4 5 6 7 8** | <span class="verdict-right">As expected</span> |
| fds after | back to 0–3. If not, ownership is broken (Part 8) | **0 1 2 3** | <span class="verdict-right">As expected</span> |
| Thread states during | 5 asleep in the handler, main in `accept()` | **`5 Sl+ hrtimer_nanosleep`**, **`1 Sl+ inet_csk_accept`** | <span class="verdict-right">As expected</span> |

### Reading each output

**BEFORE**

```
Threads:        1
0  1  2  3
```

Only the main thread (Test 1's `nc` worker ended when `nc` disconnected). The fds are 0, 1, 2 (stdin, stdout, stderr: every process starts with these) and **3, the listening socket** (`listen_socket_`, created in `TcpServer`'s constructor). This is Pulse's resting state.

**The `[1] 1408` … `[5] 1412` lines** aren't Pulse. They're **bash's job control**: when you start a command with `&`, bash prints its job number in brackets and its PID. Five curls, five jobs, PIDs 1408–1412. (The `Done` lines later are bash saying those jobs finished. Bash only prints them at certain moments, which is why not all five appear.)

**DURING** (1 second in, all 5 workers asleep in the handler)

```
Threads:        6
0  1  2  3  4  5  6  7  8
      5 Sl+  hrtimer_nanosleep
      1 Sl+  inet_csk_accept
```

- **`Threads: 6`** = 1 main + **5 workers, one per connection**: thread-per-connection, counted directly.
- **fds 4–8**: one socket per connection. `accept()` always hands out the **lowest free number**, so five connections got 4, 5, 6, 7, 8. In Month 1, Pulse could never have more than one client fd open (always 4), because it closed each client before accepting the next. **Five client fds open at once is new in Month 2.**
- **`5 Sl+ hrtimer_nanosleep`**: all 5 workers asleep in `/slow`'s `sleep_for`, using no CPU. This is Week 1's Experiment 3 again, with 5 sleeping threads, except these are real Pulse connections.
- **`1 Sl+ inet_csk_accept`**: the main thread, in `accept()`, ready for a 6th client. It never waits on any of them.

**The five curls**

```
slow #3 done in 5.003427 s
slow #1 done in 5.001812 s
slow #2 done in 5.003572 s
slow #4 done in 5.004575 s
slow #5 done in 5.003864 s
```

- **Each took 5.00 s**: just its own 5-second sleep, plus 2–5 ms. None waited behind another. **Total ≈ 5 s.** Month 1 Pulse would have handled them one after another, ~5, 10, 15, 20, 25 s.
- **Finish order 3, 1, 2, 4, 5**, not 1, 2, 3, 4, 5. The five sleeps all ended within ~3 ms of each other, so which one gets its response first is up to the scheduler. The same lesson as Week 1's Experiments 1 and 3: start order doesn't decide finish order.

**AFTER**

```
Threads:        1
0  1  2  3
```

**Back to exactly the resting state.** This is the most important line of the test:

- **Threads back to 1**: each worker's thread **ended** when `serve_connection()` returned. Detached threads clean themselves up. Nobody had to `join()` them (Week 1 doc, Part 8).
- **fds back to 0–3**: each of the 5 client sockets was **closed exactly once**. This is the ownership rule from the Week 2 doc (Part 3), checked in practice:
    - if the main thread had *also* closed them (two owners), workers' reads would have failed and responses would be missing, but all 5 curls got answers;
    - if nobody had closed them (no owner), fds 4–8 would still be in this list, the leak that ends in `EMFILE`.
  The `Socket` was moved into each worker, and its destructor closed the fd when `handle_client` returned, **on the worker thread**: the "Ticket" demo from Week 2 doc Part 4, now in Pulse.

**T1: Pulse's log**

```
Client connected: 127.0.0.1:41524
[TID 1418] Parsed (82 bytes total): GET /slow HTTP/1.1
Client connected: 127.0.0.1:41522
Client connected: 127.0.0.1:41534
[TID 1419] Parsed (82 bytes total): GET /slow HTTP/1.1
[TID 1420] Parsed (82 bytes total): GET /slow HTTP/1.1
Client connected: 127.0.0.1:41546
[TID 1421] Parsed (82 bytes total): GET /slow HTTP/1.1
Client connected: 127.0.0.1:41550
[TID 1422] Parsed (82 bytes total): GET /slow HTTP/1.1
```

- **Five workers, TIDs 1418–1422, consecutive.** They were created one right after another by the main thread's accept loop, with nothing else on the system grabbing numbers in between (unlike Test 1's gaps).
- **Two threads' lines are interleaved.** `Client connected` lines come from the **main** thread and `Parsed` lines from the **workers**, so they mix: two `Client connected` lines in a row, then two `Parsed` lines in a row. The main thread was accepting the next client while workers were still parsing. Each line is intact this time. Mid-line scrambling needs more traffic, which is Test 6.
- **`41524` was logged before `41522`**, even though its port number is higher. All five curls started at almost the same instant. Each curl's port was picked when it *started* connecting, but `accept()` returns connections in the order their **handshakes finished**, and that order isn't the same. One more case of "order is not guaranteed".
- **You can't tell from this log which TID served which port.** `Client connected` (with the port) and `Parsed` (with the TID) are printed by different threads, and nothing links them. If that ever matters for debugging, the fix is to log the port inside the worker too. Not needed now, but worth knowing why it's ambiguous.

<details>
<summary>Raw output: Test 3</summary>

**T1 (Pulse log)**

```
Client connected: 127.0.0.1:41524
[TID 1418] Parsed (82 bytes total): GET /slow HTTP/1.1
Client connected: 127.0.0.1:41522
Client connected: 127.0.0.1:41534
[TID 1419] Parsed (82 bytes total): GET /slow HTTP/1.1
[TID 1420] Parsed (82 bytes total): GET /slow HTTP/1.1
Client connected: 127.0.0.1:41546
[TID 1421] Parsed (82 bytes total): GET /slow HTTP/1.1
Client connected: 127.0.0.1:41550
[TID 1422] Parsed (82 bytes total): GET /slow HTTP/1.1
```

**T2**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ P=$(pgrep -x pulse)
echo "BEFORE:"; grep Threads /proc/$P/status; ls /proc/$P/fd

for i in 1 2 3 4 5; do curl -s -o /dev/null -w "slow #$i done in %{time_total} s\n" localhost:8080/slow & done
sleep 1
echo "DURING:"; grep Threads /proc/$P/status; ls /proc/$P/fd
ps -L -o stat=,wchan:22= -p $P | sort | uniq -c
wait

sleep 0.5
echo "AFTER:"; grep Threads /proc/$P/status; ls /proc/$P/fd
BEFORE:
Threads:        1
0  1  2  3
[1] 1408
[2] 1409
[3] 1410
[4] 1411
[5] 1412
DURING:
Threads:        6
0  1  2  3  4  5  6  7  8
      5 Sl+  hrtimer_nanosleep
      1 Sl+  inet_csk_accept
slow #3 done in 5.003427 s
slow #1 done in 5.001812 s
slow #2 done in 5.003572 s
slow #4 done in 5.004575 s
[1]   Done                       curl -s -o /dev/null -w "slow #$i done in %{time_total} s\n" localhost:8080/slow
slow #5 done in 5.003864 s
[4]-  Done                       curl -s -o /dev/null -w "slow #$i done in %{time_total} s\n" localhost:8080/slow
[5]+  Done                       curl -s -o /dev/null -w "slow #$i done in %{time_total} s\n" localhost:8080/slow
AFTER:
Threads:        1
0  1  2  3
```

</details>

---

# Test 4 — A client that resets mid-request (error handling)

**Question:** Week 2 doc Part 6. When a client vanishes before Pulse writes its response, is only that connection affected?

```bash
# T2
python3 Month2/Week2/reset_client.py 8080

# wait ~6 seconds, then:
pgrep -x pulse && echo "Pulse is still running"
curl -s localhost:8080/hello; echo
```

Read the comments in `reset_client.py` first. They explain how it forces a TCP reset.

**Paste back:** T2, and T1's log lines from this test.

## Result: a client that vanishes only ends its own connection

<div class="tldr">
<b>What I did:</b> sent <code>GET /slow</code>, then reset the connection while Pulse's worker was asleep in the handler. Five seconds later the worker tried to write its response to a client that no longer existed.
<br><br>
<b>What happened:</b> the <code>write()</code> failed with <b>"Connection reset by peer"</b>. The exception was caught in <code>serve_connection()</code>, which logged one line, and <b>only that connection ended</b>. Pulse kept running (same main PID, <b>837</b>, as in Tests 1–3) and answered the next <code>/hello</code> normally.
<br><br>
<b>Why it matters:</b> in Month 1, this exact error would have gone all the way up to <code>main()</code> and shut the whole server down. Without Week 2's <code>try/catch</code>, it would have hit <code>std::terminate()</code> and killed every connection. One bad client can now only hurt itself.
</div>

### Predicted vs actual

No Test 4 predictions were written in `notes.txt`. Expectations from the Week 2 concepts doc (Part 6) and the comments in `reset_client.py`:

| | Expected | Actual | Verdict |
|---|---|---|---|
| Pulse's log | `[TID n] connection error: write: ...` | `[TID 1621] connection error: write: Connection reset by peer — closing this connection, server keeps running` | <span class="verdict-right">As expected</span> |
| Pulse still running? | yes | `pgrep` printed **837**: the same main thread/PID as in Tests 1, 2, 3 | <span class="verdict-right">As expected</span> |
| Next request works? | yes | `Hello from Pulse!` (worker TID 1651) | <span class="verdict-right">As expected</span> |

### What happened, step by step

Pulse's log:

```
Client connected: 127.0.0.1:34472
[TID 1621] Parsed (39 bytes total): GET /slow HTTP/1.1
[TID 1621] connection error: write: Connection reset by peer — closing this connection, server keeps running
Client connected: 127.0.0.1:35694
[TID 1651] Parsed (83 bytes total): GET /hello HTTP/1.1
```

1. **`Client connected: …:34472`**: `reset_client.py` connected. The main thread accepted it and **moved** its `Socket` into a new worker, TID 1621.
2. **`[TID 1621] Parsed (39 bytes total): GET /slow`**: the worker read and parsed the request, then went to sleep in `/slow`'s handler (`hrtimer_nanosleep`, as in Test 2).
    - Why **39** bytes, not 82 like Tests 2 and 3? The Python script sends the bare minimum: `GET /slow HTTP/1.1` (18) + `\r\n` (2) + `Host: localhost` (15) + `\r\n` (2) + the blank line `\r\n` (2) = **39**. `curl` adds `User-Agent:` and `Accept:` headers, hence 82.
3. **0.5 s later, the script reset the connection.** `SO_LINGER` with timeout 0, then `close()`: instead of the normal polite FIN, the client's kernel sent a TCP **RST** ("reset"), meaning "this connection is gone, forget it". Pulse's kernel received it and marked the socket as reset. Worker 1621 didn't notice, because it was asleep in the handler, not reading.
4. **~5 s later the handler woke up** and returned its response. `handle_client` called `write_all()` → `::write()` → the kernel said no: **`-1`, `errno = ECONNRESET`**, which `strerror` turns into "Connection reset by peer".
5. **`write_all()` called `throw_errno("write")`**, which threw `std::runtime_error("write: Connection reset by peer")`.
6. **The exception unwound `handle_client`'s stack frame.** On the way out, C++ ran `~Socket()` on its `client` parameter, which called `::close()`. **The fd was still closed exactly once.** RAII works even when a function is left by an exception (Month 1 / Week 1).
7. **It landed in `serve_connection()`'s `catch (const std::exception& e)`**, which printed the `connection error` line. Then `serve_connection()` returned normally and **thread 1621 ended**. Nothing reached `std::terminate()`.
8. **`Client connected: …:35694` / `[TID 1651] Parsed … /hello`**: your follow-up `curl /hello`, served by a brand-new worker, as normal.

### The same error in three versions of Pulse

| Version | What `write()` failing for one client does |
|---|---|
| **Month 1** | The exception escapes `handle_client` → `run()` → reaches `main()`'s `catch` → `Fatal error: write: Connection reset by peer` → **the whole server exits.** (Worked out by following the code. Month 1 has no `/slow` route, so this test can't be run on it directly.) |
| **Week 2 code without `serve_connection()`'s `catch`** | The exception escapes the worker's starting function → **`std::terminate()`** → the process aborts (exit code 134), killing every connection in progress. That's exactly the `throw_in_thread` demo in Week 2 doc Part 6, which even used the same kind of error message |
| **Week 2 as written** | Caught in `serve_connection()`, one log line, **only this connection ends** |

### Honest note: which safety net this run actually tested

Week 2 added **two** protections (Week 2 doc, Part 6), and this run exercised one of them:

- **`serve_connection()`'s `catch`: tested, it worked.** The error came back from `write()` as a normal `-1` + `errno`, became an exception, and was caught.
- **Ignoring `SIGPIPE`: not the thing tested here.** The error this time was **`ECONNRESET`** ("Connection reset by peer"), the kernel reporting the reset it had already received. `SIGPIPE` comes with a different error, **`EPIPE`** ("Broken pipe"), when writing to a connection that has *already* failed, for example a **second** `write()` after the first one hit the reset. Pulse's small responses go out in a single `write()`, so this run never got that far. `SIG_IGN` is the safety net for the case where it does (a large response sent in several `write()` calls, or a client that disconnects at a slightly different moment). It doesn't show up in this test, but without it that case would kill the whole process with no error message at all.

<details>
<summary>Raw output: Test 4</summary>

**T1 (Pulse log)**

```
Client connected: 127.0.0.1:34472
[TID 1621] Parsed (39 bytes total): GET /slow HTTP/1.1
[TID 1621] connection error: write: Connection reset by peer — closing this connection, server keeps running
Client connected: 127.0.0.1:35694
[TID 1651] Parsed (83 bytes total): GET /hello HTTP/1.1
```

**T2**

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ python3 Month2/Week2/reset_client.py 8080
Sent GET /slow. Waiting 0.5 s so Pulse's worker is inside the slow handler...
Connection RESET. In ~5 s, Pulse's worker will try to write its response to a client that's gone.
Watch Pulse's terminal for '[TID n] connection error: ...' — and check the server is still alive.
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ pgrep -x pulse && echo "Pulse is still running"
curl -s localhost:8080/hello; echo
837
Pulse is still running
Hello from Pulse!
```

</details>

---

# Test 5 — Nothing broke: every Month 1 behavior

```bash
curl -i localhost:8080/
curl -i localhost:8080/hello
curl -i localhost:8080/health
curl -i -X POST localhost:8080/echo -d "hello"
curl -i localhost:8080/does-not-exist          # 404
curl -i -X POST localhost:8080/hello           # 405
printf 'GARBAGE\r\n\r\n' | nc -q1 localhost 8080   # 400
python3 Month1/Week3/slow_client.py 8080        # partial reads (Month 1 / Week 3)
```

**Paste back:** all of it.

## Result: verified during implementation (not re-run by hand)

This test was skipped as a hands-on run, because the same checks were already run by the automated build-and-test script when the Week 2 code was written. That run, in WSL against the Week 2 build:

```
/        200
Hello from Pulse!  200        ← /hello
OK  200                       ← /health
hello  200                    ← POST /echo -d hello
404 test 404                  ← /nope
405 test 405                  ← POST /hello
HTTP/1.1 400 Bad Request      ← printf 'GARBAGE\r\n\r\n' | nc
HTTP/1.1 200 OK               ← Month1/Week3/slow_client.py (request split across two read()s)
Hello from Pulse!
```

Every Month 1 behavior is unchanged: the three GET routes, `POST /echo`, 404, 405, 400 for malformed input, and Month 1 / Week 3's partial-read handling. That's expected, since `handle_client`'s body, the parser, the router and the handlers didn't change. Only *which thread* runs them did.

Honest caveat: these are the automated results, not a run done by hand. The commands above are still here for re-checking any time.

---

# Test 6 — `ab -c 10`, and scrambled logs

**Question:** does Pulse handle many concurrent requests without failures? And does the log mix, as Week 1 / Experiment 1b predicted?

This test sends 2000 requests, so instead of scrolling Terminal 1, the log goes to a file. That needs one extra piece, explained below.

```bash
# T1: stop Pulse (Ctrl+C), then restart it with its output going to a file
cd /mnt/c/Users/aagam/OneDrive/Desktop/Pulse
stdbuf -oL ./build/pulse 8080 > pulse_ab.log 2>&1

# T2
ab -n 2000 -c 10 http://localhost:8080/hello
echo "Parsed lines:            $(grep -o Parsed pulse_ab.log | wc -l)"
echo "lines from 2+ threads:   $(grep -c 'TID.*TID' pulse_ab.log)"
grep -m 3 'TID.*TID' pulse_ab.log
```

| New piece | Meaning |
|---|---|
| `> pulse_ab.log 2>&1` | Send stdout to a file, and stderr (`2`) to the same place as stdout (`&1`) |
| `stdbuf -oL` | Make the program's stdout **line-buffered**. When stdout is a file, the C library saves output up in a ~4 KB buffer and writes it in chunks. If Pulse is then killed with Ctrl+C, whatever was still in the buffer is **lost**. I measured this while testing: without `stdbuf`, only 960 of 1000 log lines made it into the file. With it, all 1000 did. (In a terminal, output is already line-buffered, which is why you've never seen this.) |
| `ab -n 2000 -c 10` | 2000 requests, 10 at a time (the same tool as Month 1 / Week 4) |
| <code>grep -o Parsed &#124; wc -l</code> | Count every occurrence of "Parsed", even two on one scrambled line |
| `grep -c 'TID.*TID'` | Count lines with **two or more** `TID` tags: pieces from different threads mixed into one line |

When done: `Ctrl+C` Pulse in T1, and `rm pulse_ab.log`.

**Paste back:** all of `ab`'s output, and the three lines after it.

This is **not** the Month 1 vs Month 2 benchmark. That's Week 4, done carefully. Here we only check correctness: `Failed requests: 0`.

## Result: 2000 / 2000 correct, but slow, and the reason was the log file, not the threads

<div class="tldr">
<b>What I did:</b> 2000 requests to <code>/hello</code>, 10 at a time, with Pulse's log going to a file in the Pulse folder.
<br><br>
<b>What happened:</b> <b>correctness was perfect</b>: 2000 complete, <b>0 failed</b>, exactly <b>2000</b> <code>Parsed</code> lines in the log. But throughput was only <b>1,032 requests/s</b>, with nearly every request taking ~10 ms. That was much lower than expected.
<br><br>
<b>Why:</b> the log file was on <code>/mnt/c</code>, the Windows (OneDrive) drive as seen from WSL, where every write is slow. The <b>main thread</b> writes a log line for every connection before it can go back to <code>accept()</code>, so slow log writes slowed down accepting. Measured: the same test with the log on Linux's own <code>/tmp</code> gave <b>~4,000–4,300 req/s</b>. <b>The bottleneck was the log destination, not thread-per-connection.</b>
</div>

<div class="stat-row">
<div class="stat-card">
<div class="label">Failed requests</div>
<div class="values"><span class="flat">0 / 2000</span></div>
<div class="note">every request answered correctly</div>
</div>
<div class="stat-card">
<div class="label">Req/s: log on /mnt/c vs /tmp</div>
<div class="values">~1,180 &rarr; ~4,150</div>
<div class="note">same code, only the log file location changed</div>
</div>
<div class="stat-card">
<div class="label">Scrambled log lines</div>
<div class="values">0</div>
<div class="note">this run. An earlier run had 11, so it's timing-dependent</div>
</div>
</div>

### Predicted vs actual

No Test 6 predictions were written in `notes.txt`.

| | Expected | Actual | Verdict |
|---|---|---|---|
| `Failed requests` | 0 | **0** | <span class="verdict-right">As expected</span> |
| `Parsed` count | 2000. `stdbuf -oL` should stop the log losing lines | **2000** exactly | <span class="verdict-right">As expected</span>. Compare 960 / 1000 without `stdbuf` (measured during implementation) |
| Scrambled lines (2+ TIDs) | some, as in Week 1 / Experiment 1b | **0** | <span class="verdict-wrong">Not this time</span>. Timing-dependent (below) |
| Throughput | roughly Month 1's range. Not a benchmark, just a sanity check | **1,032 req/s**, well below Month 1's ~5,380 | <span class="verdict-wrong">Surprise</span>, explained and measured below |

### Every `ab` field, explained

| Field | Value | Meaning |
|---|---|---|
| `Document Length` | 17 bytes | `Hello from Pulse!` is 17 characters |
| `Concurrency Level` | 10 | `-c 10`: ab keeps 10 requests in flight at once. In Week 2 Pulse, that means up to 10 workers alive at once |
| `Time taken for tests` | 1.937 s | Wall-clock time for all 2000 |
| `Complete requests` / `Failed requests` | 2000 / **0** | **The correctness result.** Every response was a valid, complete HTTP response of the expected length |
| `Total transferred` / `HTML transferred` | 202,000 / 34,000 bytes | Per request: 101 bytes total = 17 bytes of body + 84 bytes of status line and headers |
| `Requests per second` | **1,032.31** | 2000 ÷ 1.937 s. **The surprise**, explained below |
| `Time per request` (mean) | 9.687 ms | How long one request took, from the client's view |
| `Time per request` (across all concurrent) | 0.969 ms | 1.937 s ÷ 2000: on average, a request *finished* every ~1 ms |
| `Transfer rate` | 101.82 KB/s | Bytes received ÷ time |
| `Connect` | ~0 ms | The TCP handshake is instant. The kernel does it, even when Pulse is slow (Week 1 / Experiment 0's lesson) |
| `Processing` / `Waiting` | mean 9, median 10, max 15 ms | From "request sent" to "response received". **Almost every request took ~10 ms**, very consistent (sd 0.9 ms) |
| `Total` | mean 10 ms | Connect + processing |
| Percentiles | 50%–99%: 10–11 ms, max 15 ms | Very tight: no request was much slower than the rest |
| `WARNING: The median and mean … not within a normal deviation` | | ab's own consistency check: mean (9) and median (10) differ by more than the standard deviation (0.9). With values this tight it's a quirk of rounding to whole ms. Harmless here |

**Why 10 concurrent requests × ~10 ms each ≈ 1,000 req/s:** with 10 requests always in flight and each taking ~10 ms, about 10 finish every 10 ms, so ~1,000 per second. (In general: *requests in flight = throughput × time per request*. Here 10 ≈ 1,032 × 0.0097.) So the real question is **why each request took ~10 ms** when a single `/hello` takes well under 1 ms of actual work.

### Finding the bottleneck: measured, not guessed

The log went to `pulse_ab.log` in the Pulse folder, which in WSL is `/mnt/c/...`: **the Windows drive (and OneDrive), reached through WSL's file-sharing layer.** Every write there has to cross from Linux to Windows, and with `stdbuf -oL` there's **one write per log line**.

The key detail: the **main thread** prints `Client connected: …` for every connection **before** handing it off and returning to `accept()` (see `run()`). So every slow log write is time the main thread can't spend accepting. The whole server's accept rate is capped by how fast the main thread can write to that file.

To check this rather than guess, the same `ab -n 2000 -c 10` was run with the log sent to three different places (same Week 2 build, same `stdbuf -oL`, two rounds each):

| Pulse's log goes to | Requests/s | Failed |
|---|---|---|
| `/mnt/c/…/Pulse` (Windows / OneDrive drive) | **1,176** / **1,191** | 0 |
| `/tmp` (Linux's own filesystem) | **4,286** / **4,007** | 0 |
| `/dev/null` (thrown away) | **4,332** / **4,305** | 0 |

**Logging to the Windows drive alone cut throughput by about 3.5×.** With the log on Linux's filesystem, Pulse is almost as fast as with no log at all. Your 1,032 matches the `/mnt/c` row. Lesson for every benchmark from now on: **keep the log off `/mnt/c`** (use `/tmp`, or `/dev/null` when the log isn't needed). Otherwise you're measuring Windows file sharing, not Pulse.

**Still open for Week 4:** even the best row (~4,300 req/s) is below Month 1's ~5,380 at `-c 10`, on a different day with different conditions. The obvious candidate is Week 2's new cost: **creating and destroying a thread for every single connection** (Week 1 / Experiment 3 measured roughly 1 ms of kernel time per 100 threads), plus the main thread's extra hand-off work. That's exactly the kind of question Week 4's Month 1 vs Month 2 benchmark answers properly, under the same conditions, measured rather than guessed.

### Why 0 scrambled lines this time

During implementation, an identical `ab -n 2000 -c 10` run produced **11** scrambled lines, like `[TID [TID 549548] Parsed (] Parsed (87 bytes total)…`. This run produced **0**. That's not a contradiction. Mixing only happens when two threads hit `std::cout` at the same microsecond, and that depends entirely on timing. In this run every log line ended with a slow write to `/mnt/c`, so the whole server was paced by those writes and threads rarely reached `std::cout` at the same moment. (That's the likely explanation, not a measured one.)

**0 in one run does not mean "safe".** Experiment 1 (98 AB / 2 BA) taught the same thing. The log is still unprotected shared state, and Week 3 fixes it properly.

<details>
<summary>Raw output: Test 6</summary>

```
aagam@Galu-PC:/mnt/c/Users/aagam/OneDrive/Desktop/Pulse$ ab -n 2000 -c 10 http://localhost:8080/hello
echo "Parsed lines:            $(grep -o Parsed pulse_ab.log | wc -l)"
echo "lines from 2+ threads:   $(grep -c 'TID.*TID' pulse_ab.log)"
grep -m 3 'TID.*TID' pulse_ab.log
This is ApacheBench, Version 2.3 <$Revision: 1923142 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking localhost (be patient)
Completed 200 requests
Completed 400 requests
Completed 600 requests
Completed 800 requests
Completed 1000 requests
Completed 1200 requests
Completed 1400 requests
Completed 1600 requests
Completed 1800 requests
Completed 2000 requests
Finished 2000 requests


Server Software:
Server Hostname:        localhost
Server Port:            8080

Document Path:          /hello
Document Length:        17 bytes

Concurrency Level:      10
Time taken for tests:   1.937 seconds
Complete requests:      2000
Failed requests:        0
Total transferred:      202000 bytes
HTML transferred:       34000 bytes
Requests per second:    1032.31 [#/sec] (mean)
Time per request:       9.687 [ms] (mean)
Time per request:       0.969 [ms] (mean, across all concurrent requests)
Transfer rate:          101.82 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.1      0       1
Processing:     1    9   0.9     10      15
Waiting:        1    9   0.9     10      15
Total:          2   10   0.9     10      15
WARNING: The median and mean for the processing time are not within a normal deviation
        These results are probably not that reliable.
WARNING: The median and mean for the waiting time are not within a normal deviation
        These results are probably not that reliable.

Percentage of the requests served within a certain time (ms)
  50%     10
  66%     10
  75%     10
  80%     10
  90%     10
  95%     11
  98%     11
  99%     11
 100%     15 (longest request)
Parsed lines:            2000
lines from 2+ threads:   0
```

**Log-destination comparison (run during this write-up, same build):**

```
log -> /mnt/c (OneDrive, Windows fs) line-buffered: Failed requests: 0 Requests per second: 1176.29 [#/sec] (mean)
log -> /tmp (Linux fs)            line-buffered: Failed requests: 0 Requests per second: 4286.26 [#/sec] (mean)
log -> /dev/null                  line-buffered: Failed requests: 0 Requests per second: 4331.58 [#/sec] (mean)
log -> /mnt/c (OneDrive, Windows fs) line-buffered: Failed requests: 0 Requests per second: 1191.35 [#/sec] (mean)
log -> /tmp (Linux fs)            line-buffered: Failed requests: 0 Requests per second: 4007.44 [#/sec] (mean)
log -> /dev/null                  line-buffered: Failed requests: 0 Requests per second: 4305.18 [#/sec] (mean)
```

</details>

---

# Week 2 done when

- [x] Pulse hands every connection to its own thread (`run()` + `serve_connection()`), building with no warnings
- [x] Silent client no longer freezes everyone (Test 1: 30.04 s → 0.018 s)
- [x] `/slow` doesn't block `/hello` (Test 2: 0.030 s during a 5 s `/slow`)
- [x] One thread and one fd per connection, all cleaned up afterwards (Test 3: threads 1 → 6 → 1, fds 0–3 → 0–8 → 0–3)
- [x] One bad client can't kill the server (Test 4)
- [x] Every Month 1 behavior still works (Test 5, automated run)
- [x] 2000 concurrent requests, 0 failures (Test 6)
- [ ] Predictions written before running. All Week 2 boxes were left blank. Worth doing in Week 3, where the whole point is predicting a *wrong* answer (a counter that loses updates)
- Carried forward:
    - **Week 3:** scrambled logs (shared `std::cout`), plus the shared request counter
    - **Week 4:** throughput below Month 1 (~4,300 vs ~5,380 req/s), probably the per-connection thread cost; to be measured properly
    - **Later:** `server` lifetime if shutdown is ever added (doc Part 5)
