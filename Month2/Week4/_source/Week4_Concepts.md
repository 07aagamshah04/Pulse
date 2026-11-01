---
title: "Pulse — Month 2, Week 4: Concepts"
subtitle: "Every connection costs a thread. What happens when there are lots of them?"
---

<style>
details { background:#fafafa; border:1px solid #ddd; border-radius:6px; padding:0.3em 1.2em 1em; margin:1.2em 0; }
details[open] { padding-bottom:1.2em; }
summary { cursor:pointer; font-weight:600; padding:0.8em 0; color:#444; }
.big { background:#fff8e1; border:2px solid #f0c040; border-radius:8px; padding:1em 1.4em; margin:1.4em 0; font-size:1.05em; }
</style>

# The whole week in one paragraph

Since Week 2, Pulse gives **every connection its own thread**. That fixed the big Month 1 problem: one slow client no longer blocks everyone. But it means **more clients = more threads**, with nothing to stop it. This week asks two simple questions:

1. **What does each thread cost Pulse?** (time, memory, CPU)
2. **What happens when there are a lot of them?** (hundreds, thousands)

Then we **measure** it, and run Month 1 and Month 2 side by side to see what thread-per-connection really gained, and what it costs.

There's no new feature to build this week. It's all about understanding.

**How to read this doc:** the main text explains each idea in plain words. Grey boxes marked **"▶ The measured numbers"** hold the exact measurements from your machine. They're optional: open them if you're curious, skip them if not.

---

# Part 1 — The picture: one thread per open connection

You already saw this in Week 2, Test 3:

```
5 clients sending /slow  →  Pulse had 6 threads  (1 main + 5 workers)
all 5 finished           →  Pulse had 1 thread   (just main)
```

The rule is simple:

<div class="big">
<b>Pulse's thread count = 1 (the main thread) + 1 for every connection that's open right now.</b>
</div>

So if 1,000 clients are connected at the same moment, Pulse has 1,001 threads. There's no limit in Pulse's code: `run()` creates a thread for every connection, always.

**One important detail: "open right now".** A fast request like `/hello` finishes in a fraction of a millisecond, and its thread ends straight away. So sending 1,000 *quick* requests doesn't give you 1,000 threads at once. They mostly come and go one at a time. You only get lots of threads when lots of connections **stay open at the same time**:
- slow clients (like the silent `nc` from Week 1, Experiment 0)
- slow handlers (like `/slow`)
- very many clients sending at the same moment

<div class="analogy">
<b>Analogy:</b> Pulse is a restaurant that hires <b>one waiter per customer</b>, the moment the customer walks in. The waiter stays with that customer until they leave, then goes home. A customer who orders quickly and leaves needs their waiter for a minute. A customer who sits there for an hour (a slow client) keeps their waiter for an hour. So the number of waiters at any moment = the number of customers sitting in the restaurant right now.
</div>

The rest of this doc asks: **what does each waiter cost, and how many can the restaurant hold?**

---

# Part 2 — What each thread costs

Each connection's thread costs Pulse three things.

### Cost 1: time to create it

Every time a client connects, the main thread has to **create a brand-new thread**. That isn't free: the operating system has to set up a new stack and new bookkeeping, then start it running. On your machine, creating (and later finishing) one thread takes about **0.13 milliseconds**.

That sounds tiny, but compare it with how long one `/hello` takes. In Week 3, Pulse handled about 8,000 requests per second, which is about **0.12 milliseconds per request**. So **creating the thread takes about as long as the whole request.** For fast requests, Pulse spends roughly as much effort *hiring the waiter* as the waiter spends *serving the customer*.

<div class="analogy">
<b>Analogy:</b> hiring a new waiter for every customer means paperwork every time someone walks in. If customers stay an hour, a minute of paperwork doesn't matter. If customers stay one minute, the paperwork takes as long as serving them.
</div>

### Cost 2: memory

Each thread needs memory, mainly for its **stack** (its "notebook", Week 1, Part 4). There are two numbers to keep apart:

| | Amount per thread | What it means |
|---|---|---|
| **Reserved** | 8 MB | Memory *set aside* for the stack in case it's needed. Like booking a hotel room: booking costs nothing until someone actually sleeps there |
| **Actually used** | about **38 KB** | The memory that's really taken: the small part of the stack actually used, plus the operating system's own bookkeeping for the thread |

So **1,000 open connections ≈ 38 MB of real memory**, which is easy for your 3.5 GB machine. Memory is **not** what breaks first.

One surprise: Pulse's own memory number (`VmRSS`, from Week 1) shows **less** than 38 KB per thread. That's because every thread also has a small **second stack inside the kernel** (16 KB), used when the thread makes a system call like `read()`. That kernel memory isn't counted in Pulse's own number. To see the true cost, look at the **whole system's** free memory instead.

<details>
<summary>▶ The measured numbers: memory (optional)</summary>

A test program created N threads that just sleep (like N slow clients), and measured memory:

```
PID 40506: created 1000 sleeping threads in 120.5 ms
n=1000: VmSize: 8203152 kB VmRSS: 11904 kB Threads: 1001
      KernelStack +15984 kB  (= 15 kB per thread);  MemAvailable -39416 kB (= 39 kB per thread)

PID 41525: created 3000 sleeping threads in 432.3 ms
n=3000: VmSize: 24595944 kB VmRSS: 28416 kB Threads: 3001
      KernelStack +47952 kB  (= 15 kB per thread);  MemAvailable -112604 kB (= 37 kB per thread)
```

- `VmSize` = reserved. 3,000 threads reserved **24.6 GB**, on a machine with only 3.5 GB of RAM. That's possible because reserving costs nothing until it's used.
- `VmRSS` = Pulse's own used memory: 28 MB for 3,000 threads.
- `KernelStack` = the kernel's second stack: exactly 16 KB per thread, and **not** included in `VmRSS`.
- `MemAvailable` = how much free memory the whole system lost: **~38 KB per thread**. That's the true cost.

</details>

### Cost 3: one file descriptor

Each connection is one open **file descriptor** (fd), the numbers 4, 5, 6… you saw in Week 2, Test 3. A process can only have so many fds open at once. That's the first limit Pulse will hit. See Part 3.

### The three costs together

| Cost per connection | Amount | Does it matter? |
|---|---|---|
| Time to create its thread | ~0.13 ms | **Yes, for fast requests**: it's about the same as the request itself |
| Real memory | ~38 KB | Not until many thousands of connections |
| File descriptors | 1 | **Yes**: there's a fixed maximum per process (next part) |

<details>
<summary>▶ The measured numbers: thread creation time (optional)</summary>

A test program created a thread that does nothing, then waited for it to finish, 20,000 times in a row:

```
20000 threads created+joined one after another: 3047 ms total, 152.3 us per thread
20000 threads created+joined one after another: 2552 ms total, 127.6 us per thread
```

130–150 µs (microseconds) = 0.13–0.15 ms per thread.

</details>

---

# Part 3 — How many connections can Pulse hold?

Every connection needs **one thread and one fd**. Linux limits both. Whichever limit comes first is Pulse's maximum.

On your machine:

| Limit | Value | What it means for Pulse |
|---|---|---|
| **Open files per process** | **10,240** | Pulse can hold about **10,000** connections, then `accept()` fails with "Too many open files". **This is the first wall.** |
| Threads per user | 14,293 | Would stop Pulse at ~14,000 threads, but the fd limit comes first |
| Memory | 3.5 GB | ~38 KB per thread gives room for ~80,000+ threads. Not the problem |

So on this machine, **Pulse tops out at about 10,000 simultaneous connections**, because of the open-file limit.

**We won't go near that this week.** The spec says not to push the machine to its limits. The tests stay at 2,000 connections or fewer. That's enough to see the trend without breaking anything.

**A question to think about (no answer here, it's for your predictions):** look at what `run()` does when `accept()` fails:

```cpp
int client_fd = ::accept(listen_socket_.fd(), ...);
if (client_fd == -1)
{
    // log the error, then...
    continue;     // ...go straight back and call accept() again
}
```

When the error is "Too many open files", the client that couldn't be accepted is **still waiting in line** to be accepted. So what happens the next time round the loop? And the time after that?

<details>
<summary>▶ All the limits on this machine (optional)</summary>

```
ulimit -n (open files per process): 10240   hard: 1048576
ulimit -u (processes+threads per user): 14293
ulimit -s (stack KiB): 8192
threads-max: 28587
pid_max: 4194304
max_map_count: 1048576
somaxconn: 4096
ip_local_port_range: 32768	60999
```

- `ulimit -n` = max open files per process (the first wall for Pulse).
- `ulimit -u` = max threads + processes for your user, counting everything you run.
- `threads-max` = max threads on the whole system.
- `somaxconn` = the biggest "waiting line" (backlog) any program may ask for. Pulse asks for 128.
- `ip_local_port_range` = how many client-side ports exist. This limits how many connections a client like `ab` can open, not Pulse.

The **client** has the same open-file limit: `ab` opening 2,000 connections needs 2,000 fds of its own.

</details>

---

# Part 4 — Lots of threads, but only 6 cores

Your laptop has **6 real CPU cores** (Week 1, Experiment 2 found this). Only 6 threads can truly run at the same instant. So what happens with 100 threads?

It depends on what the threads are doing, and you already measured both cases in Week 1:

| | Threads that are **waiting** (Week 1, Experiment 3) | Threads that are **working** (Week 1, Experiment 2) |
|---|---|---|
| Example in Pulse | a slow client, `/slow`, an idle connection | 100 clients all sending `/hello` at the same moment |
| Need a CPU? | **No.** They're asleep | **Yes**, all of them at once |
| Cost of having many | almost nothing (just memory) | they **queue for the 6 cores**, so each request waits longer |

<div class="analogy">
<b>Analogy:</b> the restaurant has <b>6 cooks</b> (cores). Waiters whose customers are still reading the menu (waiting threads) don't need a cook at all, so you can have 100 of them and the kitchen is fine. But if 100 customers all order at the same moment (working threads), the 6 cooks can only cook 6 dishes at a time. The other 94 orders wait. More waiters doesn't mean more cooking. It means a longer queue in the kitchen.
</div>

**Taking turns also costs something.** When the CPU switches from one thread to another, it has to save one thread's place and load the next one's (a **context switch**, Week 1, Part 5). That takes a few microseconds every time. With 6 threads on 6 cores, there's almost no switching. With 100 busy threads sharing 6 cores, the system switches constantly, and that switching is wasted time.

<div class="big">
<b>So more threads helps when they're <i>waiting</i> (that's why Week 2 worked so well for slow clients), but not when they're <i>working</i>.</b> Past the number of cores, extra working threads just add queueing and switching.
</div>

<details>
<summary>▶ The measured number: one context switch (optional)</summary>

Two threads passed a message back and forth 200,000 times, both on the same CPU, so every hand-off was a real switch:

```
200000 round trips = 400000 switches in 2431 ms -> 6.08 us per switch (incl. the pipe read/write)
```

About **6 µs** per switch on your machine, inside WSL2. (On normal Linux, without the virtual machine, it's usually less.)

You can watch switches happening with `vmstat 1`. Its `cs` column is "context switches per second" for the whole machine.

</details>

---

# Part 5 — The doorman: Pulse's main thread

Here's something special about Pulse's design. **Every single connection goes through the same one thread: the main thread.** For each client it does:

```
accept() the connection  →  write the "Client connected" log line  →  create a new thread  →  back to accept()
```

This is a **one-at-a-time job**. It doesn't matter if you have 6 cores or 600 worker threads: there is only **one** main thread, and it handles connections one after another.

<div class="analogy">
<b>Analogy:</b> the restaurant has <b>one doorman</b>. Every customer, without exception, is greeted by him, and he personally hires a waiter for them (that's the ~0.13 ms of paperwork from Part 2) before he can greet the next one. However many waiters and cooks are inside, customers can't get in faster than the doorman can do his paperwork.
</div>

This leads to a real **hypothesis** to test this week:

> **When lots of fast requests arrive, Pulse's speed may be limited by the main thread creating threads, not by the workers.**

The numbers make it believable: creating a thread takes ~0.13 ms, and Pulse handled about one request every ~0.12 ms. Those are suspiciously close.

**How we'll check it, rather than guess:** during a heavy load test, look at **each thread's CPU usage** (`top -H`, Week 1). If the main thread is near **100%** busy while the workers are mostly idle, the doorman is the bottleneck.

(This is exactly what **Month 5's worker pool** fixes. Instead of hiring a new waiter for every customer, you keep a fixed team of waiters and reuse them, so there's no paperwork per customer.)

---

# Part 6 — Measuring "how long did it take?": latency

So far we've measured **throughput**: requests per second. This week we also measure **latency**: how long *each* request took. The spec asks for "p50, p95, p99". Here's what that means, with a tiny example.

Say 10 requests took these times (in milliseconds), sorted from fastest to slowest:

```
1   1   1   1   1   1   1   1   2   50
```

| Name | Meaning | In this example |
|---|---|---|
| **average** (mean) | add them all, divide by 10 | (8 + 2 + 50) ÷ 10 = **6 ms** |
| **p50** (median) | the middle one: half were faster, half slower | **1 ms** |
| **p90** | 90% were this fast or faster | **2 ms** |
| **max** | the slowest one | **50 ms** |

The **average says 6 ms, but no request actually took 6 ms**: nine took 1–2 ms and one took 50 ms. The average mixes them together and hides what really happened. That's why the spec asks for **percentiles**:

- **p50** tells you what a **typical** request experienced.
- **p95 / p99** tell you what the **unlucky** ones experienced: 1 in 20 or 1 in 100 requests were slower than this.

The unlucky ones matter, because a real web page makes many requests, and the page is only as fast as its **slowest** one.

`ab` prints this for you, as the table **"Percentage of the requests served within a certain time (ms)"**. The `50%` row is p50, `95%` is p95, `99%` is p99, and `100%` is the max.

**What makes the unlucky requests slow in Pulse, at high load:**
- waiting for one of the 6 cores (Part 4)
- waiting for the doorman (Part 5)
- the **waiting line overflowing**: Pulse lets 128 connections wait to be accepted. If more arrive faster than the main thread can accept them, the kernel turns them away, and the client automatically **tries again after 1 second**. So if you ever see some requests taking **about 1,000 ms** while the rest take a few ms, that's the cause.

<details>
<summary>▶ A shortcut for predicting latency (optional)</summary>

`ab -c 10` always keeps exactly **10** requests in flight. If Pulse finishes **1,000** requests per second, then each request must take about **10 ÷ 1,000 = 0.01 s = 10 ms**. In general:

```
average time per request  ≈  how many in flight (ab's -c)  ÷  requests per second
```

So once you know the requests per second, you can predict the average latency before looking. If you raise `-c` from 10 to 100 and requests per second stays the same, each request must take about **10× longer**. This rule is called **Little's law**.

</details>

---

# Part 7 — The three experiments this week

Everything above turns into three experiments.

### Experiment A: hold lots of connections open

**Question:** how does Pulse's cost grow as connections grow?

**How:** a small test client opens N connections to Pulse and sends **nothing**, like Week 1's silent `nc` client, N times over. Each one makes Pulse create a thread that sits waiting. Repeat for N = 100, 500, 1,000, 2,000.

**What to watch:** Pulse's thread count (should be N + 1), its open fds, and the system's memory. Prediction from Part 2: memory grows by about 38 KB per connection, in a straight line.

### Experiment B: increasing concurrency on Month 2

**Question:** what happens to speed and latency as more clients send at the same time?

**How:** `ab` with `-c` = 1, 10, 50, 100, 200.

**What to watch:** requests per second, p50 / p95 / p99, CPU use, and **the main thread's CPU** (the Part 5 hypothesis).

### Experiment C: the same on Month 1, side by side

**Question:** what did thread-per-connection actually gain, and what did it cost?

**How:** rebuild the Month 1 version of Pulse (from git, Part 8) and run Experiment B against **both**, back to back, in the same sitting.

**Be ready for a surprise.** Month 2 was built to stop **slow** clients from blocking everyone, and it does that (Week 2). For **fast** `/hello` requests, Month 2 pays the thread-creation cost on every request, and Month 1 doesn't. So it's genuinely not obvious which one is faster here. Predict before you measure.

### The tools (you've used all of them before)

| What | Tool |
|---|---|
| Requests per second, latency percentiles | `ab` |
| CPU per thread (is the main thread at 100%?) | `top -H -p <PID>`, or `pidstat -t -p <PID> 1` |
| Thread count | `grep Threads /proc/<PID>/status` |
| Pulse's memory | `grep VmRSS /proc/<PID>/status` |
| System memory | `free -m` |
| Context switches | `vmstat 1` (the `cs` column) |

The exact commands go in the test plan (`Week4/notes.html`), as in Weeks 2 and 3.

---

# Part 8 — Comparing Month 1 and Month 2 fairly

**Why not just use Month 1's numbers from the README?** Because on this laptop, the **same code** measured ~3,200 requests/s one day and ~8,400 the next (Week 3, Test 7). Numbers from different days can't be compared. So **both versions must run on the same day, one after the other.**

**But Month 1's code is gone.** It was changed in Weeks 2 and 3. Git still has it, though. This command puts the Month 1 version into a **separate folder**, without touching your current code:

```bash
git worktree add ../pulse-month1 dc0ce76
```

- `dc0ce76` is the commit called **"Week4 benchmarking completed"**: the last commit of Month 1.
- `../pulse-month1` is the new folder, next to your Pulse folder.
- Your current Pulse folder isn't changed at all.

Then build it as usual, and you have a Month 1 `pulse` and a Month 2 `pulse` to run next to each other. When finished, `git worktree remove ../pulse-month1` deletes that extra folder.

**The fairness rules:**
1. Same day, back to back.
2. Same `ab` command for both.
3. Pulse's log goes to `/dev/null` for both (Week 2's lesson: logging to `/mnt/c` slows Pulse down a lot).
4. Run each test at least twice, to see how much it moves on its own.

And the spec's reminder: **the goal isn't "Month 2 must win". The goal is "I understand *why* the numbers changed."**

---

# Part 9 — Concept check

**1. How many threads does Pulse have when 50 clients are connected at the same moment?**
51: one main thread, plus one per open connection.

**2. If 1,000 `/hello` requests are sent quickly, one after another, does Pulse reach 1,000 threads?**
Usually not. Each `/hello` finishes in a fraction of a millisecond and its thread ends, so they mostly don't overlap. Lots of threads only build up when connections stay open at the same time.

**3. What are the three costs of each connection?**
Time to create its thread (~0.13 ms), real memory (~38 KB), and one file descriptor.

**4. Why does the thread-creation time matter so much for `/hello`?**
Because a whole `/hello` request takes about the same time (~0.12 ms). Pulse spends roughly as long creating the thread as doing the actual work.

**5. "Reserved" 8 MB vs "used" 38 KB per thread: what's the difference?**
Reserved is memory set aside in case the stack grows, which costs nothing until it's used. Used is what's really taken. Only the used amount fills up RAM.

**6. What limit does Pulse hit first on this machine, and at roughly what number?**
The open-files limit: about 10,000 simultaneous connections, because each connection is one fd.

**7. Why are 100 *waiting* threads cheap, but 100 *working* threads expensive?**
Waiting threads are asleep and don't need a CPU. Working threads all need one of only 6 cores at once, so they queue, and the system wastes time switching between them.

**8. What is the "doorman problem"?**
Every connection must pass through the single main thread, which accepts it and creates its thread one at a time. However many cores or workers there are, Pulse can't accept connections faster than that one thread can.

**9. How would you check whether the main thread is the bottleneck?**
During a heavy load test, look at per-thread CPU (`top -H`). If the main thread is near 100% and the workers aren't, it's the bottleneck.

**10. Why is the average latency not enough?**
It can describe no real request. Nine 1 ms requests and one 50 ms request average 6 ms, which hides both the typical case and the slow one. p50 shows the typical case, p99 the unlucky one.

**11. Some requests take ~1,000 ms while most take a few ms. What's the likely cause?**
The waiting line (backlog) overflowed, so the kernel turned connections away, and the clients retried after 1 second.

**12. Why must Month 1 and Month 2 be measured on the same day?**
This laptop's speed changes a lot from day to day (the same code gave ~3,200 and ~8,400 req/s on different days), so only same-session numbers can be compared.

**13. Why might Month 1 be as fast as, or faster than, Month 2 on `/hello`?**
Month 2 pays to create a new thread for every request, and Month 1 doesn't. Month 2's real advantage is with slow clients and slow handlers, not fast requests.
