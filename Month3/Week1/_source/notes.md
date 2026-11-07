---
title: "Pulse — Month 3, Week 1: Experiments & Results"
---

<style>
details { background:#fafafa; border:1px solid #ddd; border-radius:6px; padding:0.3em 1.2em 1em; margin:1.2em 0; }
summary { cursor:pointer; font-weight:600; padding:0.6em 0; }
.key { background:#fff8e1; border-left:4px solid #f0c040; padding:0.8em 1.2em; margin:1.2em 0; }
</style>

# How to run the tests

Build both programs (in WSL, from the Pulse folder):

```bash
g++ -std=c++17 -Wall -Wextra Month3/Week1/blocking_socket_experiment.cpp    -o Month3/Week1/blocking.out
g++ -std=c++17 -Wall -Wextra Month3/Week1/nonblocking_socket_experiment.cpp -o Month3/Week1/nonblocking.out
```

Each program waits for **one** client on port **9090**, then exits when that client closes. You need up to three terminals:

- **Terminal 1** runs the experiment program. It prints its PID on the first line.
- **Terminal 2** is the client: `nc -N localhost 9090`. Type a line and press **Enter** to send it. **Ctrl+D** closes the connection cleanly (the program then sees `read() == 0`).
- **Terminal 3** is for looking at the program from outside (`ps`, `top`, `pidstat`, `kill`).

What the inspection commands show:

| Command | What it shows |
|---|---|
| `ps -o pid,stat,wchan:22,cmd -p <PID>` | `STAT`: `S` = asleep, `R` = running. `WCHAN`: which kernel function it's asleep in (blank / `-` when running) |
| `top -p <PID>` | live `%CPU` (press `q` to quit) |
| `pidstat -u -p <PID> 1 5` | `%CPU` once a second, 5 times. If it complains about screen size, run `export COLUMNS=120` first |
| `kill -USR1 <PID>` | sends signal `SIGUSR1` to the program |

**Write your predictions in notes.txt before each experiment.**

---

# Experiment A — Blocking vs non-blocking

**What we'll do:** connect a client that sends nothing at first, and compare what each program does while it waits.

**A1 — blocking**

```bash
./Month3/Week1/blocking.out                       # Terminal 1
nc -N localhost 9090                                 # Terminal 2: connect, but DON'T type yet
ps -o pid,stat,wchan:22,cmd -p <PID>              # Terminal 3: while nothing has been typed
```

Wait about 10 seconds, then type `hello` + Enter in Terminal 2, then `world` + Enter, then Ctrl+D.

**A2 — non-blocking (patient)**

```bash
./Month3/Week1/nonblocking.out                    # Terminal 1
nc -N localhost 9090                                 # Terminal 2: connect, wait ~5 s, then type hello + Enter, then Ctrl+D
ps -o pid,stat,wchan:22,cmd -p <PID>              # Terminal 3: while nothing has been typed
```

**What to look at:**
- How many lines does each program print while you're not typing?
- How long does each `read()` take to come back when there's no data?
- `STAT` and `WCHAN` for each. Are they asleep in the same place?
- The `flags before` / `flags after` lines in A2.
- In A2: when you press Enter, how long until the program shows your text?

**What we did:** connected `nc` to each program, sent nothing for a while, then typed `hello`, then closed the client. While nothing had been typed, we looked at each program with `ps`.

**What we saw:**

| | Blocking (A1) | Non-blocking, patient (A2) |
|---|---|---|
| Lines printed while you weren't typing | **1** (`calling read()`, then silence for 41 s) | **one per second** (12 `EAGAIN` attempts before `hello`) |
| A `read()` with nothing to read… | waited **41 s**, until `hello` arrived | came back in **4–21 microseconds** with `-1`, `errno = EAGAIN (11)` |
| `STAT` | `S` (asleep) | `S` (asleep) |
| `WCHAN`: where it was asleep | `wait_woken`: **inside `read()`**, waiting for data | `hrtimer_nanosleep`: **inside `sleep(1)`**, not in `read()` at all |
| Flags | O_NONBLOCK off | `0x2` → `0x802` |
| When `hello` was typed | `read()` returned **immediately** | shown at the **next 1-second check** (up to 1 s late) |
| Client closed (Ctrl+C on `nc`) | `read()` returned `0` | `read()` returned `0` |

**What it means:**

- **The blocking `read()` is one call that waits as long as it has to.** It was called once and didn't return for 41 seconds. During that time the program could do nothing else. This is exactly Pulse's `handle_client()` with a silent client (Month 2, Week 1, Experiment 0).
- **The non-blocking `read()` never waits.** "Nothing yet" came back in about 10 microseconds, which is about 4 million times faster than the blocking one. `-1` with `EAGAIN` was the normal answer, not an error: the connection was healthy the whole time, and `hello` arrived on attempt #13.
- **Both programs were asleep, but in different places.** Blocking slept inside `read()`, and only data on **that one socket** could wake it. Patient slept inside `sleep()`, so between naps it is free to check other sockets. That freedom is what Month 3 needs, and the reason `O_NONBLOCK` exists.
- **The price of "patient" is delay.** Data that arrives just after a check sits unnoticed until the next one, up to 1 second. Blocking noticed `hello` the instant it arrived. Experiment B asks what happens if we remove the delay by not sleeping at all.
- **`fcntl` kept the old flag and added one bit.** `0x2` (`O_RDWR`, "readable and writable") + `0x800` (`O_NONBLOCK`) = `0x802`. That's the read, `|`, write-back pattern from Part 2 of the concepts doc.
- **Closing the client gives `0` in both modes.** `0` means "the other side closed", not "nothing yet". `nc` was killed with Ctrl+C, and the kernel closed its socket for it, so the server still got a clean EOF.

**Your predictions:** none were written in notes.txt for A. Please fill them in **before** running B and C.

<details>
<summary>▶ Raw output (optional)</summary>

```
=== A1: blocking.out
PID 728, mode: plain
Waiting in accept() on port 9090 ... connect with:  nc localhost 9090
[  0.000 s] accepted client, fd 4. O_NONBLOCK is OFF -> read() will WAIT for data.
[  0.000 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[ 41.007 s] read() returned 6 after waiting 41.006 s: "hello\n"
[ 41.007 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[ 58.143 s] read() returned 6 after waiting 17.137 s: "enter\n"
[ 58.144 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[111.665 s] read() returned 0 after waiting 53.521 s: the client closed its side (EOF). Done.

$ ps -o pid,stat,wchan:22,cmd -p 728
    PID STAT WCHAN                  CMD
    728 S+   wait_woken             ./Month3/Week1/blocking.out

=== A2: nonblocking.out (patient)
PID 1014, mode: patient
On this system: EAGAIN = 11, EWOULDBLOCK = 11
[  0.000 s] accepted client, fd 4. flags before = 0x2 (O_NONBLOCK OFF)
[  0.000 s] flags after  = 0x802 (O_NONBLOCK ON, O_NONBLOCK bit = 0x800)
[  0.000 s] attempt #1: read() returned -1, errno = EAGAIN (11, "Resource temporarily unavailable") in 4 us -> nothing yet, try again in 1 s
[  1.000 s] attempt #2: read() returned -1, errno = EAGAIN (11, ...) in 10 us
   ... attempts #3 - #12 identical, 10-21 us each ...
[ 12.004 s] attempt #13: read() returned 6 (in 19 us): "hello\n"
[ 13.004 s] attempt #14: read() returned -1, errno = EAGAIN (11, ...) in 10 us
   ... attempts #15 - #25 identical ...
[ 25.008 s] attempt #26: read() returned 0 (in 14 us): client closed its side (EOF). Done.

$ ps -o pid,stat,wchan:22,cmd -p 1014
    PID STAT WCHAN                  CMD
   1014 S+   hrtimer_nanosleep      ./Month3/Week1/nonblocking.out
```
</details>

---

# Experiment B — CPU behavior (busy polling)

### Why we run this experiment

Experiment A left us with a problem. A non-blocking `read()` never waits, so **the program has to keep asking** "anything yet?". The patient version asked once a second, used no CPU, but noticed data **up to a second late**. The obvious fix is "just don't sleep: ask again immediately, and you'll notice data instantly." This experiment checks what that fix really costs.

**The intuition:** imagine Pulse in Week 3 with 1,000 connected clients, most of them idle. If the single event-loop thread "waits" for them by asking each socket again and again, **the CPU never rests**, even when no client is sending anything. We want to see that cost with **one** idle client first, measure it, and understand where it comes from. That's what motivates `poll()` in Week 3: a way to sleep **and** wake up instantly.

The question in one line: **"If `read()` never waits, what does it cost to keep asking?"**

### How we ran it

**What we did:** connected one client that stayed silent for 8 seconds, then sent `hello`, then closed. We ran this against all three versions: busy, patient, and blocking. During the silent part we measured CPU with `pidstat` (one reading per second for 5 seconds) and checked `ps`. *(Run by Claude, with a small Python client standing in for `nc`.)*

```bash
./Month3/Week1/nonblocking.out busy               # Terminal 1
nc -N localhost 9090                                 # Terminal 2: connect, type nothing for ~10 s
pidstat -u -p <PID> 1 5                           # Terminal 3: while idle
ps -o pid,stat,wchan:22,cmd -p <PID>              # Terminal 3
```

Then type `hello` + Enter, then Ctrl+D.

Then, for comparison, run the same `pidstat` against `nonblocking.out` (patient mode) and `blocking.out` while their client is idle.

**What to look at:**
- `last second: N read() calls` lines: roughly how big is N?
- `%CPU` in busy mode vs patient vs blocking.
- `STAT` in busy mode.
- Does busy mode notice your text faster than patient mode?

### What we saw

| While the client was silent | Busy | Patient | Blocking |
|---|---|---|---|
| CPU used (`pidstat %CPU`) | **100%** (one whole core) | **0%** | **0%** |
| …of which inside the kernel (`%system`) | 84% | 0% | 0% |
| `read()` calls per second | **about 460,000** | 1 | 1 call, waiting the whole time |
| Of those, how many found data | **0** | 0 | — |
| `STAT` / `WCHAN` | **`R`** (running) / none | `S` / `hrtimer_nanosleep` | `S` / `wait_woken` |
| `hello` noticed | **at once** (8.001 s) | 8.003 s, lucky timing: up to 1 s late in the worst case | **at once** (8.001 s) |

### What it means

- **Busy polling kept one CPU core at 100% to do nothing.** In 8 silent seconds it made about **3.7 million `read()` calls**, and every one of them answered "nothing yet". The client wasn't sending anything, but the program was working as hard as it possibly could.
- **Most of that time was spent inside the kernel (84% `%system`).** Every `read()` is a system call: jump into the kernel, check the socket, find it empty, jump back. Each round trip costs about 2 microseconds. Repeat that half a million times a second and you have a full core.
- **`STAT` was `R`, not `S`.** The blocking and patient versions let the scheduler put them to sleep and give the core to someone else. Busy polling never gives the core up, so the core is taken away from other programs, including (in a real server) Pulse's own useful work like parsing and routing.
- **Busy did notice `hello` instantly.** That's the one thing it does better than patient. So we have a trade-off: **patient = no CPU but slow, busy = fast but burns a core.** Neither is acceptable for a server.
- **Blocking got both right: 0% CPU *and* instant.** The kernel woke it up exactly when data arrived. But it could only wait on **one** socket. What we want is blocking's behaviour for **many sockets at once**: "sleep, and wake me the moment *any* of them has data". That is exactly what **`poll()`** does (Week 3).

<div class="key">
<b>Takeaway:</b> non-blocking I/O stops the thread from getting stuck, but on its own it gives you a bad choice: waste CPU, or react late. The kernel has to tell us <i>when</i> to read. That's I/O multiplexing, Week 3.
</div>

<details>
<summary>▶ Raw output (optional)</summary>

```
=== busy
pidstat:  %usr %system  %CPU
          19.00  81.00  100.00
          17.00  83.00  100.00
          14.00  86.00  100.00
          16.00  84.00  100.00
          16.00  84.00  100.00
Average:  16.40  83.60  100.00
ps:       1291 R+   -                      /tmp/nonblocking.out busy

[  1.000 s] last second: 455299 read() calls, 455299 returned EAGAIN, 0 bytes received
[  2.000 s] last second: 463886 read() calls, 463886 returned EAGAIN, 0 bytes received
[  3.000 s] last second: 458825 read() calls, 458825 returned EAGAIN, 0 bytes received
[  4.000 s] last second: 464967 read() calls, 464967 returned EAGAIN, 0 bytes received
[  5.000 s] last second: 461165 read() calls, 461165 returned EAGAIN, 0 bytes received
[  6.000 s] last second: 462901 read() calls, 462901 returned EAGAIN, 0 bytes received
[  7.000 s] last second: 457583 read() calls, 457583 returned EAGAIN, 0 bytes received
[  8.000 s] last second: 463393 read() calls, 463393 returned EAGAIN, 0 bytes received
[  8.001 s] DATA after 278 read() calls this second: "hello\n"
[  9.000 s] last second: 474521 read() calls, 474520 returned EAGAIN, 6 bytes received
[  9.002 s] read() returned 0: client closed its side (EOF). Done.

=== patient
pidstat:  %CPU 0.00 (all 5 seconds)
ps:       1300 S+   hrtimer_nanosleep      /tmp/nonblocking.out
[  7.003 s] attempt #8: read() returned -1, errno = EAGAIN (11, ...) in 26 us -> nothing yet, try again in 1 s
[  8.003 s] attempt #9: read() returned 6 (in 15 us): "hello\n"
[  9.003 s] attempt #10: read() returned 0 (in 14 us): client closed its side (EOF). Done.

=== blocking
pidstat:  %CPU 0.00 (all 5 seconds)
ps:       1309 S+   wait_woken             /tmp/blocking.out
[  0.000 s] calling read() ...
[  8.001 s] read() returned 6 after waiting 8.001 s: "hello\n"
[  9.001 s] read() returned 0 after waiting 1.000 s: the client closed its side (EOF). Done.
```

(This machine has 12 CPU cores, so 100% here means one of the 12.)
</details>

---

# Experiment C — Error handling: EAGAIN, EWOULDBLOCK, EINTR

### Why we run this experiment

Pulse's `handle_client()` today treats **every** `-1` from `read()` as "connection broken": it logs an error and closes the client. In Experiment A, `-1` turned out to be the **normal** answer for a healthy non-blocking socket (`EAGAIN`). This experiment checks the other "harmless" `-1`, **`EINTR`**, by sending a real signal to a program that is waiting in `read()`.

**The intuition:** later on Pulse will want to handle Ctrl+C itself (graceful shutdown: finish current requests, then exit). The moment it installs a signal handler, a waiting call can be woken up by a signal and return `-1 / EINTR`. If Pulse treats that as an error, **a signal would drop a perfectly good client**. So we need to see with our own eyes that after `EINTR` the connection is still fine, and what changes with `SA_RESTART` or with no handler at all.

The question in one line: **"When a signal hits a waiting `read()`, is the connection broken, or just interrupted?"**

### How we ran it

**What we did:** for each of the three modes, a client connected and stayed silent for 4 seconds. At about 2 seconds we sent `kill -USR1` while the program was waiting in `read()`. At 4 seconds the client sent `hello`, then closed. The client also reported what it saw from its side. *(Run by Claude, with a small Python client standing in for `nc`.)*

**C1 — EAGAIN / EWOULDBLOCK:** already in Experiment A2's output. Look at the `EAGAIN = __, EWOULDBLOCK = __` line and the `errno` on each "nothing yet" attempt.

**C2 — EINTR, three ways.** For each mode: start it, connect with `nc` but don't type, send the signal, then type `hello` + Enter and Ctrl+D.

```bash
./Month3/Week1/blocking.out eintr                 # Terminal 1   (handler, no SA_RESTART)
./Month3/Week1/blocking.out restart               # Terminal 1   (handler, with SA_RESTART)
./Month3/Week1/blocking.out plain                 # Terminal 1   (no handler at all)

nc -N localhost 9090                                 # Terminal 2, each time
kill -USR1 <PID>                                  # Terminal 3, while read() is waiting
```

**What to look at:**
- Which mode prints `EINTR`? Which doesn't?
- After the signal, does the connection still work? Does `hello` still arrive?
- What happens to the program in `plain` mode? And to the `nc` client?

### What we saw

| `kill -USR1` while waiting in `read()` | `eintr` (handler, no `SA_RESTART`) | `restart` (handler + `SA_RESTART`) | `plain` (no handler) |
|---|---|---|---|
| What `read()` did | returned **`-1`, `errno = EINTR` (4)** at 1.7 s | **nothing visible**: kept waiting | never returned: **program killed** |
| Did the program see `EINTR`? | yes | no ("signals received: 1", but `read()` was restarted) | — |
| Did `hello` still arrive? | **yes**, at 4.0 s on the next `read()` | **yes**, at 4.0 s | **no** |
| Clean EOF at the end? | yes (`read() == 0`) | yes | — |
| Program exit status | 0 (normal) | 0 (normal) | **138**: shell printed `User defined signal 1` |
| What the client saw | connection fine | connection fine | **connection closed** (`recv` got EOF) |

And C1, from Experiment A: **`EAGAIN = 11`, `EWOULDBLOCK = 11`**, the same number on Linux.

### What it means

- **`EINTR` is not a broken connection.** In `eintr` mode, `read()` returned `-1` with `EINTR`, the program simply called `read()` again, and `hello` arrived normally 2 seconds later. Nothing was lost. Closing the client here, as Pulse's `handle_client()` would, would have been a **bug**.
- **`SA_RESTART` hides it.** Same signal, same moment, but the kernel quietly restarted `read()` after the handler. The program only knows a signal happened because the handler counted it. Easier to write, but the thread goes straight back to waiting, so it can't check a flag like `stop`. That's why a graceful shutdown deliberately **wants** to see `EINTR`.
- **With no handler, the signal kills the program.** `SIGUSR1`'s default action is "terminate", exactly like Ctrl+C on Pulse today. Exit status **138 = 128 + 10**, where 10 is `SIGUSR1`'s number (`kill -l 10` prints `USR1`). The kernel then closed the program's sockets, so the client saw its connection end. No `EINTR`, because there was no program left to return to.
- **So `EINTR` only exists when the program chose to handle the signal.** That's why Pulse never sees it today, and will see it once it installs a Ctrl+C handler.
- **`EAGAIN` and `EWOULDBLOCK` are one thing on Linux** (both 11). Checking both is a habit for other Unix systems, where they can differ.

<div class="key">
<b>Takeaway:</b> <code>-1</code> from <code>read()</code> means "look at <code>errno</code>", not "close the connection".<br>
<code>EAGAIN</code> / <code>EWOULDBLOCK</code> → nothing yet, try later. <code>EINTR</code> → interrupted, try again (or check your flags). Anything else → a real error.
</div>

<details>
<summary>▶ Raw output (optional)</summary>

```
=== mode: eintr        (SIGUSR1 sent at ~1.7 s)
ps before signal:  1360 S+   wait_woken   /tmp/blocking.out eintr
[  0.000 s] accepted client, fd 4. O_NONBLOCK is OFF -> read() will WAIT for data.
[  0.002 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[  1.726 s] read() returned -1, errno = EINTR (4, "Interrupted system call") after 1.724 s. A signal interrupted the wait. Connection is fine -> calling read() again.
[  1.726 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[  4.004 s] read() returned 6 after waiting 2.278 s: "hello\n"
[  4.006 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[  6.025 s] read() returned 0 after waiting 2.016 s: the client closed its side (EOF). Done.
client: sent hello OK; connection still open

=== mode: restart
ps before signal:  1371 S+   wait_woken   /tmp/blocking.out restart
ps after signal:   1371 S+   wait_woken   /tmp/blocking.out restart     <- still waiting, same place
[  0.000 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[  4.001 s] read() returned 6 after waiting 4.001 s: "hello\n"  (signals received so far: 1 — read() was restarted, we never saw EINTR)
[  6.005 s] read() returned 0 after waiting 2.005 s: the client closed its side (EOF). Done.
program exit status: 0
client: sent hello OK; connection still open

=== mode: plain
ps before signal:  1382 S+   wait_woken   /tmp/blocking.out plain
ps after signal:   (process gone)
[  0.000 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
bash: 1382 User defined signal 1      /tmp/blocking.out plain
program exit status: 138
client: sent hello OK; recv -> b'' (server closed the connection)
```

(The `eintr` mode's `ps` lines scrolled off the capture. The program output above shows the same thing: waiting in `read()` until the signal.)
</details>

---

# Week 1 summary

| Experiment | Question | Answer |
|---|---|---|
| **A** | What does `O_NONBLOCK` change? | A blocking `read()` waits (41 s, asleep in `wait_woken`). A non-blocking one answers in ~10 µs: data, or `-1 / EAGAIN` = "nothing yet". |
| **B** | What does it cost to keep asking? | Asking non-stop = **100% of a core**, ~460,000 empty `read()`s per second. Asking once a second = 0% CPU but up to 1 s late. |
| **C** | Which `-1`s are real errors? | Not `EAGAIN` and not `EINTR`: after both, the connection was fine and the data still arrived. |

**What this means for Pulse:**

1. `O_NONBLOCK` stops a thread getting stuck on one client. That's the first thing a single-threaded Pulse needs.
2. Pulse's `handle_client()` closes the client on **any** `-1`. With non-blocking sockets (or signal handlers) that would drop healthy clients, so the error handling has to check `errno`.
3. Non-blocking alone forces a bad choice between wasted CPU and slow reactions. The fix is to let the kernel say **when** a socket is ready: `poll()`, Week 3.
4. If one thread leaves a client halfway through a request, the half-request (`raw_request`) has to live somewhere other than the stack: the `Connection` object, **Week 2**.

---

# In plain words: why we ran these experiments

### The big goal (Month 3)

Month 2 gave **every client its own thread**. That works, but every thread costs memory. Month 3 wants **one thread serving all clients**.

### The problem stopping that

```cpp
read(client_A)   // client A hasn't sent anything...
```

`read()` **waits** here. While the one thread is stuck waiting for client A, clients B and C are ignored. That's the Month 1 problem again.

### Week 1's question: can `read()` stop waiting?

Each experiment checks one thing.

**A: Does `O_NONBLOCK` really stop the waiting?**
Yes. Without it, `read()` sat there for 41 seconds. With it, `read()` answered straight away with "nothing yet" (`EAGAIN`). So the one thread is free to go and check client B.

**B: If `read()` says "nothing yet", when do we ask again?**

- Ask non-stop: one CPU core at 100%, doing nothing useful.
- Ask once a second: no CPU, but up to 1 second late.

Both are bad. We need the kernel to tell us **when** data has arrived. That's `poll()`, in Week 3.

**C: When `read()` returns `-1`, should we close the client?**
Pulse's code today does this:

```cpp
if (n < 0) { log error; close the client; }
```

But A showed that `-1` can mean "nothing yet", and the connection is fine. C checked one more case where `-1` is **not** an error: `EINTR`, "a signal woke me up early". After it, `hello` still arrived. The connection was fine.

So **the whole point of C is this one if-statement**, which goes into Pulse in Week 2:

```cpp
n = read(...)
if (n > 0)  -> got bytes, keep them
if (n == 0) -> client left, close
if (n == -1):
    EAGAIN -> nothing yet, come back later    (DON'T close)
    EINTR  -> interrupted, just try again     (DON'T close)
    other  -> real error, close
```

C is the smallest of the three. Pulse doesn't get `EINTR` today; it will once Pulse handles Ctrl+C itself (graceful shutdown). The thing to remember: **not every `-1` means "close the client".**

### Week 1 in three lines

1. `O_NONBLOCK` means `read()` never waits, so one thread can move between clients. **(A)**
2. But asking again and again wastes CPU, so we need `poll()`. **(B, leads to Week 3)**
3. A `-1` from `read()` isn't always an error, so Pulse's error check must change. **(C, used in Week 2)**

---

# `SA_RESTART`, simply

### The only question `SA_RESTART` answers

Your program is **waiting inside `read()`**. A signal arrives (`kill -USR1`). The program wakes up and runs its handler, a tiny function. Then the handler finishes.

**Now what? Who goes back to waiting in `read()`?**

| | Who goes back to waiting? |
|---|---|
| **without `SA_RESTART`** | **your code**. `read()` comes back with `-1, EINTR`, and your code has to call `read()` again itself |
| **with `SA_RESTART`** | **the kernel, automatically**. Your code never even notices it was woken up |

That's the whole concept: **the kernel goes back to waiting for you, or your code does it.** Nothing more.

### You saw this in Experiment C

**Without `SA_RESTART` (`eintr` mode):** your code was woken up and had to call `read()` again itself. You can see the extra line:

```
[0.002 s] calling read() ...
[1.726 s] read() returned -1, errno = EINTR    <- woken up by the signal
[1.726 s] calling read() ...                    <- OUR code calls read() again
[4.004 s] read() returned 6: "hello"
```

**With `SA_RESTART` (`restart` mode):** same signal, but the kernel went back to waiting by itself, so there's no extra line:

```
[0.000 s] calling read() ...
[4.001 s] read() returned 6: "hello"           <- looks like nothing happened
```

### Real-life version

<div class="key">
You're sleeping, waiting for a delivery, and your phone rings. You answer it. After you hang up:<br>
<b>Without <code>SA_RESTART</code>:</b> you're awake and have to decide yourself: "go back to sleep, or get up?"<br>
<b>With <code>SA_RESTART</code>:</b> you fall straight back asleep automatically, without thinking about it.
</div>

### Do you need this for Pulse?

**Not really, not now.** For Weeks 2 and 3, all you need is the one rule:

> `read()` returns `-1` with `EINTR` → **don't close the client, just try again.**

`SA_RESTART` only matters much later, if we build graceful shutdown. It's fine to skip it for now.
