---
title: "Pulse — Month 3, Week 1: Concepts"
subtitle: "Making read() stop waiting: non-blocking sockets, O_NONBLOCK and EAGAIN"
---

<style>
details { background:#fafafa; border:1px solid #ddd; border-radius:6px; padding:0.3em 1.2em 1em; margin:1.2em 0; }
details[open] { padding-bottom:1.2em; }
summary { cursor:pointer; font-weight:600; padding:0.8em 0; color:#444; }
.big { background:#fff8e1; border:2px solid #f0c040; border-radius:8px; padding:1em 1.4em; margin:1.4em 0; font-size:1.05em; }
</style>

# The whole week in one paragraph

In Months 1 and 2, every time Pulse calls `read()` on a client and the client hasn't sent anything yet, **the thread falls asleep inside `read()`** and stays asleep until bytes arrive. Month 1 had one thread, so one silent client froze everyone. Month 2 fixed that by giving every client its own thread, so a sleeping thread only freezes its own client, but each of those threads costs about 49 KB (Month 2, Week 4). Month 3's goal is **one thread looking after many clients**. That can only work if the thread is **never allowed to fall asleep inside one client's `read()`**. This week learns the switch that stops it: **`O_NONBLOCK`**. With it, `read()` answers immediately: either "here are some bytes", or "nothing yet" (`-1` with `errno = EAGAIN`). We test it in two small standalone programs, and find the catch: if `read()` never waits, **when do you call it again?** Calling it again straight away burns a whole CPU core. That catch is what Weeks 2 and 3 solve.

**Nothing in Pulse's own code changes this week.** Only two small experiment programs on port 9090.

**How to read this doc:** the main text explains each idea in plain words. Grey boxes marked **"▶ … (optional)"** hold the exact output from real runs on your machine.

| Part | The Pulse question it answers |
|---|---|
| 1 | What exactly happens at the `read()` line in `handle_client()` when the client is silent? |
| 2 | What is `O_NONBLOCK`, and how do we turn it on for a `client_fd`? |
| 3 | What does `read()` return now, and what would Pulse's current code do with each answer? |
| 4 | What is `EINTR`, and why isn't it an error either? |
| 5 | If `read()` doesn't wait, how often do we call it? (busy polling) |
| 6 | "Non-blocking" vs "asynchronous": are they the same thing? |
| 7 | Why isn't `O_NONBLOCK` enough on its own to fix Pulse? |
| 8 | The experiments this week |
| 9 | Concept check |

---

# Part 1 — The line in Pulse that waits

This is the line in [src/tcp_server.cpp](../../src/tcp_server.cpp), inside `handle_client()`, that every Pulse connection has run since Month 1:

```cpp
ssize_t n = ::read(client.fd(), chunk, sizeof(chunk));
```

You have already seen what happens when the client sends nothing. In Month 2, Week 1 (Experiment 0) you connected a silent `nc`, and `ps` showed the Pulse thread in state **`S`** (sleeping), waiting inside the kernel.

That is what **blocking** means:

<div class="big">
<b>A blocking <code>read()</code> = "don't come back to me until there is at least one byte to give you (or the connection closes)."</b><br>
While it waits, the thread is <b>asleep inside the kernel</b>. It isn't using any CPU, but it also <b>can't do anything else at all</b>.
</div>

`blocking_socket_experiment.cpp` is that one line on its own, with a timer around it. A client connected, waited 3 seconds, sent `GET /hel`, then `lo\r\n`, then closed:

```
[  0.000 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[  3.000 s] read() returned 8 after waiting 3.000 s: "GET /hel"
[  3.000 s] calling read() ...
[  4.201 s] read() returned 4 after waiting 1.200 s: "lo\r\n"
[  4.201 s] calling read() ...
[  5.401 s] read() returned 0 after waiting 1.200 s: the client closed its side (EOF). Done.
```

`read()` returned **exactly when the client sent something**, never before. For 3 whole seconds the program was stuck on that one line. `ps` at that moment:

```
    PID STAT WCHAN                  CMD
    421 S+   wait_woken             /tmp/blocking.out
```

`WCHAN` = "the kernel function this thread is sleeping in". `wait_woken` = "sleeping until someone wakes me up", and the thing that wakes it is data arriving on the socket.

(Also notice: one request arrived as **two `read()`s**, `"GET /hel"` then `"lo\r\n"`. That's the Month 1, Week 3 lesson again: one `read()` is not one request.)

### What this meant for Month 1 and Month 2

| | Who is asleep in `read()`? | What can't happen while they sleep? |
|---|---|---|
| **Month 1** | the only thread | **everything**: no `accept()`, no other clients |
| **Month 2** | that client's own worker thread | only that client's work. Fine, but it costs a whole thread per client |
| **Month 3 goal** | **nobody, ever** | one thread serves many clients, so it must never sleep on any one of them |

<div class="analogy">
<b>Analogy:</b> a blocking <code>read()</code> is standing at your mailbox until a letter arrives. If you have one mailbox, that's fine. Month 2 hired one person to stand at each mailbox. Month 3 wants one person to look after a whole street of mailboxes, so that person cannot stand at any single one of them.
</div>

---

# Part 2 — `O_NONBLOCK`: a switch on the socket

Every open file descriptor carries a small set of **status flags**: one `int`, one bit per flag. One of those bits is **`O_NONBLOCK`**. When it is on, any call that would normally wait (`read()`, `write()`, `accept()`) **returns immediately instead**.

You read and change those flags with **`fcntl()`** ("file control"):

```cpp
int flags = fcntl(client_fd, F_GETFL);           // 1. read ALL the current flags
fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);   // 2. write them back with ONE extra bit turned on
```

Real output from `nonblocking_socket_experiment.cpp`:

```
flags before = 0x2   (O_NONBLOCK OFF)
flags after  = 0x802 (O_NONBLOCK ON, O_NONBLOCK bit = 0x800)
```

- `0x2` is `O_RDWR`: "this fd can be read and written". It was already there.
- `0x800` is the `O_NONBLOCK` bit.
- `0x802` = both. The old flag was **kept**, and the new one was added.

### Why read the flags first?

`F_SETFL` **replaces** the whole set of flags. If you wrote just:

```cpp
fcntl(client_fd, F_SETFL, O_NONBLOCK);   // wrong habit
```

you'd be saying "the flags should be **only** `O_NONBLOCK`", which switches off any other status flag that was on. On a socket that usually changes nothing visible, but on a file opened with `O_APPEND` it would silently stop appending. So the habit is always **read, add your bit with `|`, write back**. It's the same idea as "don't overwrite, append" from Pulse's `raw_request.append(...)`.

<div class="example">
<b>Where this would go in Pulse (Week 2, not now):</b> right after <code>accept()</code> in <code>TcpServer::run()</code>, before the new <code>client_fd</code> is used. A socket fresh from <code>accept()</code> is always blocking (the experiment prints <code>O_NONBLOCK is OFF</code> right after accept). Linux does <b>not</b> copy the flag from the listening socket to the accepted one.
</div>

---

# Part 3 — What `read()` returns now

Same experiment, same client (connects, waits, sends `GET /hel`, then `lo\r\n`, then closes), but now the socket is non-blocking and the program tries `read()` once per second:

```
attempt #1: read() returned -1, errno = EAGAIN (11, "Resource temporarily unavailable") in 3 us
attempt #2: read() returned -1, errno = EAGAIN (11, ...) in 8 us
attempt #3: read() returned -1, errno = EAGAIN (11, ...) in 12 us
attempt #4: read() returned 8 (in 29 us): "GET /hel"
attempt #5: read() returned 4 (in 22 us): "lo\r\n"
attempt #6: read() returned 0 (in 20 us): client closed its side (EOF). Done.
```

Compare with Part 1: **the blocking `read()` took 3 seconds to come back. The non-blocking one came back in 3 microseconds**, with the answer "nothing yet". It never waits, whatever the answer is.

So `read()` now has four kinds of answer, and Pulse's code needs to treat them differently:

| `read()` returns | Meaning | What Pulse's `handle_client()` does **today** |
|---|---|---|
| `n > 0` | here are `n` bytes (maybe not the whole request) | appends them to `raw_request`, parses again ✔ |
| `0` | the client closed its sending side (EOF) | logs "disconnected", closes ✔ |
| `-1`, `errno = EAGAIN` / `EWOULDBLOCK` | **nothing to read right now.** Connection is fine | logs `read: Resource temporarily unavailable` and **drops the connection** ✘ |
| `-1`, `errno = EINTR` | a signal interrupted the call (Part 4). Connection is fine | logs and **drops the connection** ✘ |
| `-1`, any other `errno` | a real problem (e.g. `ECONNRESET`) | logs and drops the connection ✔ |

Look at the `n < 0` branch in `handle_client()`:

```cpp
if (n < 0)
{
    ... log_error("read: " + strerror(errno));
    return;   // connection closed
}
```

It treats **every** `-1` as fatal. With a blocking socket that was nearly true, because `read()` waited until something real happened. With a non-blocking socket, `-1 / EAGAIN` is the **normal** answer whenever the client hasn't sent yet. So if we just turned on `O_NONBLOCK` in Pulse today and changed nothing else, a client whose request arrived even slightly after Pulse's first `read()` would get its connection **closed with no response at all**. That's the first concrete reason this week says: *don't touch Pulse's architecture yet.*

<div class="big">
<b>The rule:</b> <code>-1</code> from <code>read()</code> is not one thing. Always look at <code>errno</code>.<br>
<code>EAGAIN</code> / <code>EWOULDBLOCK</code> = "not now, try later". <code>EINTR</code> = "interrupted, try again". Everything else = real error.
</div>

### `EAGAIN` vs `EWOULDBLOCK`

The experiment printed `EAGAIN = 11, EWOULDBLOCK = 11`. **On Linux they are the same number**, two names for one thing ("try **again**" / "this **would block**"). Some older Unix systems gave them different numbers, which is why portable code checks both: `err == EAGAIN || err == EWOULDBLOCK`.

### Save `errno` straight away

`errno` is overwritten by the next call that fails, and even `printf` is allowed to change it. That's why both experiments do `int err = errno;` on the very next line after `read()`, before printing anything.

---

# Part 4 — `EINTR`: interrupted, not broken

<div class="big">
<b><code>EINTR</code> in one sentence:</b> "I stopped waiting early because a signal woke me up. Nothing is wrong with the connection. Just call me again."
</div>

### When does it happen?

Only when **both** of these are true:

1. The thread is **waiting** inside a call (a blocking `read()`, `accept()`, or later `poll()`).
2. A **signal** arrives, *and* the program has its own function set up to handle that signal.

A **signal** is a short message the kernel sends to a program from outside. You already know one: **Ctrl+C** sends `SIGINT` ("please stop"). Right now Pulse doesn't handle any signals itself, so Ctrl+C simply kills it on the spot. Pulse **never sees `EINTR` today**.

### Where Pulse will actually need it

The day Pulse handles Ctrl+C itself, `EINTR` will appear. Here's an example of why it might:

> **Graceful shutdown:** on Ctrl+C, instead of dying instantly in the middle of sending someone a response, Pulse sets a flag `stop = true`, finishes the requests it's working on, then exits cleanly.

To do that, Pulse installs a small Ctrl+C handler. Now picture Pulse waiting in `accept()` (or, from Week 3, in `poll()`) when you press Ctrl+C:

- The kernel wakes Pulse up to run the handler, which sets `stop = true`.
- The call Pulse was waiting in returns `-1`, `errno = EINTR`.
- Pulse's loop goes round again, sees `stop == true`, and shuts down cleanly.

**That's exactly why `EINTR` exists:** it's how a waiting thread gets woken up so it can notice "something changed, check your flags". Without it, Pulse could sit in `accept()` forever, never noticing you pressed Ctrl+C.

### What to do when you see it

| Situation | What the code does |
|---|---|
| `EINTR` and nothing needs to change | call the same function again |
| `EINTR` and a flag like `stop` is set | stop waiting and clean up |
| Never | treat it as a broken connection and close the client |

Pulse's current `handle_client()` breaks the third rule: any `-1` from `read()` makes it log an error and close the connection (Part 3). That's harmless today because Pulse never gets `EINTR`. It becomes a bug the moment Pulse handles signals.

### What the experiment shows

`blocking.out eintr`: you send a signal with `kill -USR1 <PID>` while `read()` is waiting:

```
[  0.000 s] calling read() ...
[  1.455 s] read() returned -1, errno = EINTR ... Connection is fine -> calling read() again.
[  1.455 s] calling read() ...
[  3.000 s] read() returned 8 after waiting 1.545 s: "GET /hel"
```

The signal woke `read()` up early. The program called it again, and the client's data still arrived normally. **The connection was never broken.**

<div class="analogy">
<b>Analogy:</b> you're waiting at the mailbox and your phone rings. You answer it. Afterwards you either go back to waiting (call <code>read()</code> again), or, if the call said "come home, we're leaving", you leave (<code>stop = true</code>). The mailbox was fine the whole time.
</div>

<details>
<summary>▶ The other two modes: <code>restart</code> and <code>plain</code> (optional)</summary>

- **`restart`**: the handler is set up with the flag `SA_RESTART`, which tells the kernel "after the handler, restart the call for me". The program never sees `EINTR`:
  ```
  [  3.000 s] read() returned 8 after waiting 3.000 s: "GET /hel"  (signals received so far: 1 — read() was restarted, we never saw EINTR)
  ```
  Convenient, but it doesn't help with graceful shutdown, because the thread goes straight back to waiting and never gets a chance to check `stop`. (And some calls, including `poll()`, are never restarted, whatever the flag says.)
- **`plain`**: no handler at all. `SIGUSR1`'s default action is to kill the program, like Ctrl+C kills Pulse today. There's no `EINTR` because there's no program left to return to.

Also: a **non-blocking** `read()` basically never gets `EINTR`, because it never waits, so there's nothing for a signal to interrupt. `EINTR` comes back in Week 3, because `poll()` is a call that **does** wait.
</details>

---

# Part 5 — The catch: when do you call `read()` again? (busy polling)

Non-blocking `read()` said "nothing yet" in 3 microseconds. Good, the thread is free. But the data **will** arrive at some point, and nobody is going to tell us. The only way to find out is to **ask again**. How often?

The experiment tries both extremes.

### Option 1 — ask again immediately, forever ("busy polling")

`nonblocking.out busy`: call `read()`, get `EAGAIN`, call it again straight away, and so on:

```
[  1.000 s] last second: 685569 read() calls, 685569 returned EAGAIN, 0 bytes received
[  2.000 s] last second: 692924 read() calls, 692924 returned EAGAIN, 0 bytes received
[  2.500 s] DATA after 339693 read() calls this second: "GET /hel"
```

About **690,000 `read()` calls every second**, every single one answering "nothing". `ps` shows the state is **`R`** (running), not `S`. The thread is using a whole CPU core to ask a question whose answer is "no" 690,000 times a second. Data is noticed the instant it arrives, but at the price of a full core spent on **nothing**. And the core it burns is one that Pulse's actual work (parsing, routing) could have used.

### Option 2 — ask once a second ("patient")

`nonblocking.out` (default): try, then `sleep(1)`:

```
[  2.001 s] attempt #3: read() returned -1, errno = EAGAIN ...
[  3.001 s] attempt #4: read() returned 8 (in 29 us): "GET /hel"
```

Hardly any CPU. `ps` shows `S` with `WCHAN = hrtimer_nanosleep`, so the thread is asleep **in `sleep()`**, not in `read()`. But look at the times: the client sent `GET /hel` at **2.5 s**, and the program noticed at **3.0 s**. The data **sat there unnoticed for half a second.** For a web server, that's a terrible delay added to every request.

### The three options side by side

| | CPU while the client is silent | How fast data is noticed | Can one thread watch many clients? |
|---|---|---|---|
| **Blocking `read()`** (Months 1–2) | none (asleep) | instantly | **no**, it's stuck on one fd |
| **Non-blocking, busy polling** | **a whole core** | instantly | yes, but burning a core |
| **Non-blocking, patient (sleep 1 s)** | almost none | **up to 1 s late** | yes, but slow |
| **What we actually want** | none | instantly | yes |

The last row is: "**put the thread to sleep, and wake it the moment ANY of my clients has data.**" Only the kernel knows when data arrives, so only the kernel can do that. That's **`poll()`**, Week 3. Blocking was "sleep until **this one** fd is ready". `poll()` is "sleep until **any** of these fds is ready".

<div class="analogy">
<b>Analogy:</b> busy polling is opening every mailbox on the street, over and over, as fast as you can run, all day. Patient polling is checking once an hour, so letters wait. <code>poll()</code> is asking the post office to ring your doorbell when any letter arrives on your street, then sleeping until it rings.
</div>

<details>
<summary>▶ Raw output: all five runs (optional)</summary>

The client for every run: connect, wait (3 s blocking / 2.5 s non-blocking), send `GET /hel`, wait 1.2 s, send `lo\r\n`, wait 1.2 s, close.

```
=== blocking.out (plain)
PID 421, mode: plain
[  0.000 s] accepted client, fd 4. O_NONBLOCK is OFF -> read() will WAIT for data.
[  0.000 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[  3.000 s] read() returned 8 after waiting 3.000 s: "GET /hel"
[  3.000 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[  4.201 s] read() returned 4 after waiting 1.200 s: "lo\r\n"
[  4.201 s] calling read() ... (if nothing has arrived, this thread now sleeps INSIDE read())
[  5.401 s] read() returned 0 after waiting 1.200 s: the client closed its side (EOF). Done.
ps during the wait:   421 S+   wait_woken   /tmp/blocking.out

=== blocking.out eintr   (kill -USR1 at ~1.5 s)
[  0.000 s] calling read() ...
[  1.455 s] read() returned -1, errno = EINTR (4, "Interrupted system call") after 1.455 s. A signal interrupted the wait. Connection is fine -> calling read() again.
[  1.455 s] calling read() ...
[  3.000 s] read() returned 8 after waiting 1.545 s: "GET /hel"
[  4.201 s] read() returned 4 after waiting 1.200 s: "lo\r\n"
[  5.401 s] read() returned 0 after waiting 1.200 s: the client closed its side (EOF). Done.

=== blocking.out restart   (kill -USR1 at ~1.5 s)
[  3.000 s] read() returned 8 after waiting 3.000 s: "GET /hel"  (signals received so far: 1 — read() was restarted, we never saw EINTR)
[  4.201 s] read() returned 4 after waiting 1.200 s: "lo\r\n"  (signals received so far: 1 — ...)
[  5.401 s] read() returned 0 after waiting 1.200 s: the client closed its side (EOF). Done.

=== nonblocking.out (patient)
On this system: EAGAIN = 11, EWOULDBLOCK = 11
[  0.000 s] accepted client, fd 4. flags before = 0x2 (O_NONBLOCK OFF)
[  0.000 s] flags after  = 0x802 (O_NONBLOCK ON, O_NONBLOCK bit = 0x800)
[  0.000 s] attempt #1: read() returned -1, errno = EAGAIN (11, "Resource temporarily unavailable") in 3 us -> nothing yet, try again in 1 s
[  1.000 s] attempt #2: read() returned -1, errno = EAGAIN (11, "Resource temporarily unavailable") in 8 us -> nothing yet, try again in 1 s
[  2.001 s] attempt #3: read() returned -1, errno = EAGAIN (11, "Resource temporarily unavailable") in 12 us -> nothing yet, try again in 1 s
[  3.001 s] attempt #4: read() returned 8 (in 29 us): "GET /hel"
[  4.001 s] attempt #5: read() returned 4 (in 22 us): "lo\r\n"
[  5.002 s] attempt #6: read() returned 0 (in 20 us): client closed its side (EOF). Done.
ps during the run:    444 S+   hrtimer_nanosleep   /tmp/nonblocking.out

=== nonblocking.out busy
[  1.000 s] last second: 685569 read() calls, 685569 returned EAGAIN, 0 bytes received
[  2.000 s] last second: 692924 read() calls, 692924 returned EAGAIN, 0 bytes received
[  2.500 s] DATA after 339693 read() calls this second: "GET /hel"
[  3.000 s] last second: 670977 read() calls, 670976 returned EAGAIN, 8 bytes received
[  3.701 s] DATA after 484764 read() calls this second: "lo\r\n"
[  4.000 s] last second: 695839 read() calls, 695838 returned EAGAIN, 4 bytes received
[  4.901 s] read() returned 0: client closed its side (EOF). Done.
ps 1 s in:            452 R+   -   72.9 %CPU   /tmp/nonblocking.out busy
```

(`ps`'s `%CPU` is an average over the process's whole life, including the part before the client connected, so it reads below 100%. Your `top` / `pidstat` measurement in Experiment B is the real per-second number.)
</details>

---

# Part 6 — Non-blocking is not "asynchronous"

These two words are easy to mix up:

- **Non-blocking:** "if the call **can't** do its job right now, return immediately and tell me so." When data **is** there, `read()` still does all the work right then, copying the bytes into your buffer before it returns. Nothing happens in the background. If it says `EAGAIN`, nothing has been started for you, and **you** have to come back and ask again later.
- **Asynchronous:** "start this operation, go do something else, and **I'll tell you when it's finished**." The work carries on without you. (Linux has this too, `io_uring`, explicitly out of scope until much later.)

<div class="analogy">
<b>Analogy:</b> non-blocking = you ring the bakery: "is my bread ready?" "No." Click. You have to ring again later. Asynchronous = the bakery takes your number and calls <b>you</b> when it's ready.
</div>

That's why Part 5 happened: non-blocking alone gives you "no" fast, but **you** still have to decide when to ask again. `poll()` (Week 3) doesn't make Pulse asynchronous either. It tells you **when to ask**: "this socket now has data, a `read()` will not say `EAGAIN`". The `read()` itself is still yours to do.

---

# Part 7 — Why `O_NONBLOCK` alone doesn't fix Pulse

Imagine we turned `O_NONBLOCK` on in Pulse and fixed the `EAGAIN` branch in `handle_client()` to say "try again". What would that loop do?

- It would spin on **that one client**, getting `EAGAIN` hundreds of thousands of times a second (Part 5, busy mode), and **still not leave** `handle_client()` until the request is complete. Other clients are no better off than with blocking `read()`, and now we're burning a CPU core as well.
- To actually move on to another client after `EAGAIN`, `handle_client()` would have to **return** halfway through a request. But look at where the half-request lives:

```cpp
void TcpServer::handle_client(Socket client) const
{
    std::string raw_request;   // "GET /hel" lives HERE: a local variable
    char chunk[4096];
    ...
```

`raw_request` is a **local variable on this function's stack**. In Month 2 that was fine: the worker thread stays inside this function for the whole connection, so the stack frame (and `raw_request`) stays alive. But if a single thread returns from `handle_client()` to go and serve another client, the frame is gone and `"GET /hel"` is gone with it.

So the information "how far this client has got" has to move **out of the stack and into an object** that outlives each visit: the socket, the bytes received so far, the response bytes not yet sent, and which step the connection is on. That's the **`Connection`** object and its **state machine**, **Week 2**.

<div class="big">
<b>The plan for the month, in one line each:</b><br>
Week 1: <code>read()</code> must not wait → <code>O_NONBLOCK</code>, <code>EAGAIN</code>.<br>
Week 2: if we leave a client halfway, where is its progress kept? → a <code>Connection</code> object + state machine.<br>
Week 3: how do we know when to come back to it without busy polling? → <code>poll()</code>.
</div>

---

# Part 8 — The experiments this week

The full commands, what each one does, and space for results are in **notes.html**. Write your predictions in **notes.txt** first.

| Experiment | Program | The Pulse question |
|---|---|---|
| **A — Blocking vs non-blocking** | `blocking.out` and `nonblocking.out` with a silent `nc` client | When a client connects but sends nothing, where does each version spend its time, and what does `read()` return? |
| **B — CPU behavior** | `nonblocking.out busy` vs `patient`, watched with `top` / `pidstat` | What does "never wait" cost when you keep retrying? |
| **C — Error handling** | `blocking.out eintr` / `restart` / `plain` with `kill -USR1` | Which `-1`s are real errors, and which mean "try again"? |

Both programs use **port 9090**, so they never clash with Pulse on 8080.

---

# Part 9 — Concept check

Try answering these in your own words (they're this week's success criteria):

1. What does **blocking** I/O mean? Where exactly is the thread while a blocking `read()` waits, and what is it doing?
2. What does `O_NONBLOCK` change about `read()`? What doesn't it change?
3. Why do we `F_GETFL` before `F_SETFL`? What goes wrong if we don't?
4. Why is `-1` with `EAGAIN` **not** a failure? What would Pulse's `handle_client()` do with it today?
5. Why is `EINTR` not a failure either? What does `SA_RESTART` change?
6. Why does busy polling burn a whole CPU core, even though no data is arriving?
7. Why doesn't `O_NONBLOCK` on its own let one thread serve many Pulse clients? (Hint: where does `raw_request` live?)
