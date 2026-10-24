---
title: "Pulse — Month 2, Week 3: Concepts"
subtitle: "Thread Safety & Shared State: when many threads write the same thing"
---

# How to use this document

Week 2 gave every connection its own thread. Everything those threads did was either **private** (locals on their own stack) or **shared but read-only** (`router_`). That's why it just worked.

Week 3 adds the first thing every thread **writes**: server statistics (total requests, active connections, 2xx/4xx/5xx counts). The spec's plan:

1. Add a request counter **with no protection**.
2. Run concurrent requests and **try to catch it counting wrong**.
3. Fix it, with a **mutex** and then an **atomic**, and compare.

This doc explains why step 2 goes wrong, how to *see* it go wrong, and the two tools that fix it. Same format as always: every part starts from Pulse, and every output shown is real, built and run in your WSL. No Pulse code here. That comes after reading.

| Part | The Pulse question it answers |
|---|---|
| 1 | What shared state does Pulse have today, and what does Week 3 add? |
| 2 | Why can't two threads both just do `total_requests++`? |
| 3 | How badly does an unprotected counter go wrong, and why might Pulse *hide* it? |
| 4 | "Race condition" vs "data race": what's the actual bug? |
| 5 | How do you *find* a data race instead of hoping to notice one? |
| 6 | `std::mutex`, `lock_guard`, `unique_lock`: how to protect shared data |
| 7 | `std::atomic`: protection for a single variable, and the "faster" trap |
| 8 | Designing Pulse's statistics: what's per-connection and what's per-server |
| 9 | Fixing Week 2's scrambled log |
| 10 | Two more things that come with locks: deadlock and `condition_variable` |
| 11 | How Week 3 will be tested |
| 12 | Concept check |

---

# Part 1 — Pulse's shared state, today and after Week 3

Week 1's rule (Week 1 doc, Part 4): **declared inside the function a thread runs → private; everything else → shared.** Apply it to Week 2 Pulse, and add one more question: **does any thread *write* it?**

| In Pulse today | Shared? | Written by worker threads? | Safe? |
|---|---|---|---|
| `raw_request`, `chunk`, `response` (locals in `handle_client`) | no, one per worker's stack | yes, but each its own | ✅ |
| the client's `Socket` | no, moved into one worker (Week 2) | yes, but only by its owner | ✅ |
| `router_` | yes, every worker uses it | **no**, read only | ✅ |
| `listen_socket_`, `port_` | yes | no, only the main thread touches them | ✅ |
| `std::cout` | yes | **yes**, every worker prints | ⚠️ scrambled lines (Week 2, Test 6) |

So far, **nothing shared is being written** except the log. Week 2 worked because of that, not because it was designed to cope with shared writes.

**What Week 3 adds** (from the spec):

| Statistic | Changed when | Changed by |
|---|---|---|
| total connections | a client connects | main thread or a worker |
| **active connections** | +1 when a worker starts, −1 when it ends | every worker, **twice** |
| **total requests** | a request is parsed | every worker |
| 2xx / 4xx / 5xx responses | a response is sent | every worker |

Every row is **one variable, written by many threads at once.** That's the new problem.

---

# Part 2 — Why `total_requests++` isn't one step

`total_requests++` looks like one action. To the CPU, it isn't.

<div class="example">
<b>Real run: what the compiler actually turns <code>counter++</code> into.</b>

```cpp
long counter = 0;
void increment() { counter++; }
```

Compiled with <code>g++ -S</code> (output assembly instead of a program), at Pulse's optimization level (<code>-O0</code>, CMake's default here):

```
mov  rax, QWORD PTR counter[rip]     ; 1. READ   counter from memory into register rax
add  rax, 1                          ; 2. MODIFY rax = rax + 1  (only in the CPU, memory unchanged)
mov  QWORD PTR counter[rip], rax     ; 3. WRITE  rax back to counter in memory
```

<b>Three separate instructions: read, modify, write</b>, exactly the spec's "READ / MODIFY / WRITE". (<code>rax</code> is one of the CPU registers from Week 1 doc Part 3. <code>QWORD PTR counter[rip]</code> means "the 8 bytes in memory where <code>counter</code> lives".)
</div>

Between step 1 and step 3, the value lives **only in that thread's CPU register**. If another thread on another CPU reads `counter` during that gap, it reads the **old** value.

### The lost update, with two Pulse workers

Two requests finish at the same moment, on two CPUs. `total_requests` is 41:

```
time   worker A (CPU 3)               worker B (CPU 7)              total_requests in memory
 1     READ  → rax = 41                                             41
 2                                    READ  → rax = 41              41
 3     ADD   → rax = 42                                             41
 4                                    ADD   → rax = 42              41
 5     WRITE → memory = 42                                          42
 6                                    WRITE → memory = 42           42   ← should be 43
```

Two requests were served, and the counter went up by **one**. A's update was overwritten by B's. Nothing crashed and nothing printed an error. The number is just wrong. This is a **lost update**.

<div class="analogy">
<b>Analogy:</b> a whiteboard tally (shared memory, Week 1's whiteboard). To add one, you <i>read</i> the number, <i>work out</i> the next number in your head, then <i>rub it out and write</i> the new one. Two people do it at the same moment: both read 41, both work out 42, both write 42. Two tally marks' worth of work, one mark's worth of result. Each person did their job correctly. The problem is that "read, work out, write" can be interrupted halfway.
</div>

### "But with optimization it's one instruction"

At `-O2` the compiler produces this instead (real output):

```
add  QWORD PTR counter[rip], 1        ; one instruction
```

It *looks* indivisible. **It isn't, across CPUs.** Inside the CPU, that single instruction still does read → add → write on memory, and without a special prefix, another core can slip in between. The thing that makes it truly indivisible is the **`lock`** prefix you'll see in Part 7. "One line of C++" or even "one instruction" is not the same as "atomic".

---

# Part 3 — How wrong does it get? And why might Pulse hide it?

<div class="example">
<b>Real run: an unprotected shared counter.</b> N threads each run <code>plain_counter++</code> a given number of times. The "expected" total is threads × increments.

```
$ ./counter unsafe 4 1000000          (4 threads × 1,000,000 increments, run 5 times)
unsafe threads=4  expected=4000000  got=1314678  lost=2685322  time=0.043 s
unsafe threads=4  expected=4000000  got=1400609  lost=2599391  time=0.045 s
unsafe threads=4  expected=4000000  got=1336052  lost=2663948  time=0.043 s
unsafe threads=4  expected=4000000  got=1312367  lost=2687633  time=0.045 s
unsafe threads=4  expected=4000000  got=1284850  lost=2715150  time=0.045 s

$ ./counter unsafe 1 4000000          (1 thread, same total)
unsafe threads=1  expected=4000000  got=4000000  lost=0  time=0.015 s

$ ./counter unsafe 2 1000             (2 threads × only 1,000 each, run 5 times)
unsafe threads=2  expected=2000  got=2000  lost=0  time=0.000 s     ← (all 5 runs identical: no loss)
```
</div>

Read those three blocks together:

1. **4 threads hammering one counter lost about two-thirds of all increments**, and a *different* amount every run (2.60–2.72 million lost). Wrong, and not even consistently wrong.
2. **1 thread: perfect.** The code isn't "buggy" in the normal sense. It's only wrong **when threads overlap**.
3. **2 threads × 1,000: perfect, five times out of five.** Each thread finished its 1,000 increments in microseconds, often before the other thread had even started (Week 1 / Experiment 1a's head start). The threads barely overlapped, so nothing was lost. **The bug was there, and the test didn't show it.**

### Why Pulse will probably *hide* this bug

Block 3 is Pulse's situation. In the demo, threads do *nothing but* increment, so they collide constantly. In Pulse, one `total_requests++` is a few nanoseconds inside a request that takes ~0.2 ms or more of other work (reading, parsing, routing, writing). Two workers have to hit the same three instructions **at the same nanosecond**. It will happen, but rarely.

So the spec's step "try to observe incorrect results" may well show the **right** number most of the time, even with 10,000 requests. If that happens, it is **not** evidence the code is safe. It's the same lesson as Experiment 1's 98/2 and Month 1 / Week 3's single `read()`: *"works in testing" is not "correct"*. A counter that's right 99.99% of the time on a test laptop is wrong on a busy server. Part 5 shows how to prove the bug is there without needing to get lucky.

---

# Part 4 — "Race condition" vs "data race"

The spec uses both terms. They're related but not the same.

| | **Data race** | **Race condition** |
|---|---|---|
| Definition | Two threads access **the same memory** at the same time, **at least one writes**, and **nothing synchronizes them** (no mutex, no atomic) | The program's result **depends on the timing** of threads |
| In C++ | **Undefined behavior.** The language promises nothing about what happens | A logic bug. The program is valid C++, it just does the wrong thing sometimes |
| Example | `plain_counter++` from 4 threads (Part 3) | Lost updates (Part 2). Or, with no data race at all: `if (active < 100) active++;` using an atomic, where two threads can both see 99 and both increment, ending at 101 |
| Fixed by | Synchronization: a mutex or an atomic | Making the *whole* check-and-act one indivisible step |

The unprotected counter is **both**: a data race (unsynchronized write to shared memory), whose visible result is a race condition (lost updates).

**Why "undefined behavior" matters beyond the wrong number:** the compiler is allowed to assume no other thread changes a plain variable that isn't synchronized. So at `-O2` it may keep `counter` in a register for a whole loop and write it back once at the end, or skip re-reading it. The program's behavior can then change with the optimization level, the compiler version, or an unrelated edit. With a data race, "it printed the right number" doesn't even promise the same code will print it next time.

**The second row is worth remembering for later.** Week 4's thread limits and Month 5's worker pool will need checks like "only if fewer than N connections". Making `active` atomic stops the *data race*, but the check followed by the increment is still two steps, and still a *race condition*. A mutex around both, or a single atomic operation that does both, fixes it.

---

# Part 5 — Finding a data race: ThreadSanitizer

Part 3 showed that testing can easily miss a race. **ThreadSanitizer (TSan)** finds races by watching memory accesses, not results. It's built into `g++`: compile with `-fsanitize=thread`, and it adds checks around every memory access. It reports any two unsynchronized accesses from different threads, **even if they didn't collide on this particular run**.

<div class="example">
<b>Real run: TSan on the 2 × 1,000 case, the one that gave the correct answer every time in Part 3.</b>

```
$ g++ -std=c++17 -g -fsanitize=thread -pthread counter.cpp -o counter_tsan
$ ./counter_tsan unsafe 2 1000
==================
WARNING: ThreadSanitizer: data race (pid=542)
  Read of size 8 at 0x56aac632b040 by thread T2:
    #0 operator() .../counter.cpp:26 (counter_tsan+0x2566)
    ...
  Previous write of size 8 at 0x56aac632b040 by thread T1:
    #0 operator() .../counter.cpp:26 (counter_tsan+0x2580)
    ...
  Location is global 'plain_counter' of size 8 at 0x56aac632b040 (counter_tsan+0x9040)

  Thread T2 (tid=546, running) created by main thread at:
    #0 pthread_create ...
    ...
    #6 main .../counter.cpp:23
```

How to read it:

<ul>
<li><b><code>WARNING: ThreadSanitizer: data race</code></b>: found one. This is the case that gave the right answer five times in Part 3. TSan doesn't need the collision to actually happen. It notices that two threads touch the same memory with nothing ordering them.</li>
<li><b><code>Read of size 8 … by thread T2</code> / <code>Previous write of size 8 … by thread T1</code></b>: the two accesses that conflict. Size 8 = a <code>long</code>, 8 bytes. Both at <b><code>counter.cpp:26</code></b>, the <code>plain_counter++</code> line.</li>
<li><b><code>Location is global 'plain_counter'</code></b>: <i>which</i> variable. Exactly the one we'd suspect.</li>
<li><b><code>Thread T2 … created by main thread at … counter.cpp:23</code></b>: where the thread came from (the <code>emplace_back</code> line).</li>
</ul>

The same program in <code>mutex</code> mode under TSan prints just its normal result line, with <b>no warnings</b>.
</div>

**Using TSan on Pulse** (this will be part of Week 3's testing): build a *separate* copy of Pulse with TSan turned on, so the normal `build/` stays untouched.

```bash
cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan
./build-tsan/pulse 8080        # then send it concurrent requests
```

| Piece | Meaning |
|---|---|
| `-S . -B build-tsan` | **S**ource is here (`.`), **b**uild into a new folder `build-tsan` |
| `-DCMAKE_CXX_FLAGS="..."` | Add these flags to every compile. `-fsanitize=thread` turns TSan on. `-g` keeps line numbers so reports say `tcp_server.cpp:NNN` |
| `cmake --build build-tsan` | Same as `cd build-tsan && make` |

A TSan build runs several times slower and uses much more memory, so it's for **finding** races, never for benchmarking. And it only checks code that actually *runs*: send it enough concurrent traffic to exercise every counter.

---

# Part 6 — `std::mutex`: one thread at a time

A **mutex** ("**mut**ual **ex**clusion") is a lock that only one thread can hold at a time. Code between locking and unlocking is the **critical section**. If thread A holds the lock, any other thread that tries to lock it **waits** until A unlocks.

```
lock          ← if someone else holds it, wait here
  read  total_requests
  add 1
  write total_requests        ← the whole read-modify-write, with no one else allowed in
unlock
```

The three-step increment from Part 2 can no longer be interrupted *by another thread that also uses the mutex*. That last part matters: **a mutex only protects data if every thread that touches the data locks it.** One code path that skips the lock brings the data race back.

<div class="example">
<b>Real run: the same counter, with a mutex.</b>

```
$ ./counter mutex 4 1000000           (3 runs)
mutex  threads=4  expected=4000000  got=4000000  lost=0  time=0.170 s
mutex  threads=4  expected=4000000  got=4000000  lost=0  time=0.175 s
mutex  threads=4  expected=4000000  got=4000000  lost=0  time=0.174 s
```

Exactly right, every run. And about <b>4× slower</b> than the broken version (0.17 s vs 0.044 s). Part 7 comes back to that.
</div>

### What a waiting thread looks like

<div class="example">
<b>Real run: one thread holds a mutex for 10 s (asleep while holding it). Three others try to lock it.</b>

```
    TID STAT WCHAN                  COMMAND
    559 Sl+  futex_wait_queue       holdlock     ← main thread, in join()
    560 Sl+  hrtimer_nanosleep      holdlock     ← the holder: asleep WHILE holding the lock
    561 Sl+  futex_wait_queue       holdlock     ← waiting for the mutex
    562 Sl+  futex_wait_queue       holdlock     ← waiting for the mutex
    563 Sl+  futex_wait_queue       holdlock     ← waiting for the mutex
```

<ul>
<li>A thread waiting for a <code>std::mutex</code> sleeps in <b><code>futex_wait_queue</code></b>, the <b>same wait channel as <code>join()</code></b> in Week 1 / Experiment 3. Both are built on the kernel's <b>futex</b>: "sleep until another thread signals this spot in memory". Unlocking the mutex is that signal.</li>
<li>Waiting threads use no CPU (state <code>S</code>), but they also <b>do no work</b>. Three threads stuck for 10 seconds because one thread held a lock while sleeping.</li>
</ul>
</div>

**That last point is the most important rule for Pulse.** If a worker held the stats mutex while doing something slow, like writing to a slow socket or (imagine) sleeping in `/slow`, **every other worker** that wants to update a counter would queue up behind it. That's Month 1's "one slow client blocks everyone" all over again, just moved from `accept()` to a mutex. **Hold a lock for as short a time as possible: just the few instructions that touch the shared data. Never do I/O or sleep while holding it.**

### `std::lock_guard`: RAII for locks

You *can* write:

```cpp
stats_mutex.lock();
total_requests++;
stats_mutex.unlock();
```

The spec says to avoid this, and here's the problem: anything that leaves the function between `lock()` and `unlock()`, an early `return` or an **exception**, skips the `unlock()`. The mutex stays locked forever, and **every** thread that tries to lock it later waits forever. The server hangs with no error, every worker asleep in `futex_wait_queue`.

The fix is the same idea as `Socket` closing its fd (Month 1 / Week 1): **RAII**. Tie the unlock to an object's destructor, which C++ guarantees to run however the scope is left.

```cpp
{
    std::lock_guard<std::mutex> lock(stats_mutex);   // constructor: stats_mutex.lock()
    total_requests++;
}                                                     // destructor:  stats_mutex.unlock()
```

| Piece | Meaning |
|---|---|
| `std::lock_guard<std::mutex>` | A class template (`<std::mutex>` says *what kind* of lock it manages). `#include <mutex>` |
| `lock` | The variable's name. **It must have a name.** `std::lock_guard<std::mutex>{stats_mutex};` with no name creates a temporary that's destroyed at the end of that same line, so it locks and **immediately unlocks**, and protects nothing. It compiles without complaint |
| `(stats_mutex)` | The mutex it locks in its constructor and unlocks in its destructor |
| the extra `{ }` | A scope just for the lock, so it's released right after the increment instead of at the end of the whole function. This is how you keep the critical section short |

`lock_guard` is the **`Socket` of mutexes**: you can't forget to unlock, and you can't copy it.

### `std::unique_lock`: a more flexible lock_guard

Same RAII idea, more control. You can `unlock()` it early and `lock()` it again, and it can be moved. You need it for `condition_variable` (Part 10). For a simple "lock, change a counter, unlock", `lock_guard` is the right tool. Use `unique_lock` when you need its extras.

### C++ detail Pulse will hit: `mutable`

`handle_client` is a **`const`** member function: it promises not to modify `TcpServer`'s members (Month 1 / Week 1). Locking a mutex *modifies* the mutex. So if the stats mutex is a member of `TcpServer`, this won't compile inside `handle_client`:

```cpp
std::mutex stats_mutex_;           // locking it in a const method → compile error
mutable std::mutex stats_mutex_;   // OK
```

**`mutable`** means "this member may change even inside `const` methods". It's the standard thing to do for a mutex: locking is bookkeeping, not a change to the object's real, visible state. (The counters themselves have the same issue, if they live inside `TcpServer`. Part 8 discusses where they should live.)

---

# Part 7 — `std::atomic`: one indivisible operation

For a **single** counter, there's a lighter tool: make the variable itself atomic.

```cpp
#include <atomic>
std::atomic<long> total_requests{0};   // spec: std::atomic<uint64_t>
total_requests++;                      // ONE indivisible read-modify-write
```

<div class="example">
<b>Real run: what <code>atomic++</code> compiles to</b> (<code>-O2</code>):

```
lock add  QWORD PTR counter[rip], 1
```

Compare the plain <code>-O2</code> version from Part 2: <code>add QWORD PTR counter[rip], 1</code>. Same instruction, plus the <b><code>lock</code></b> prefix, which tells the CPU to make the whole read-modify-write <b>indivisible across all cores</b>. No other core can touch that memory in the middle. That one word is the difference between Part 3's lost updates and a correct count.

<b>Real run: the counter, atomic.</b>

```
$ ./counter atomic 4 1000000          (3 runs)
atomic threads=4  expected=4000000  got=4000000  lost=0  time=0.054 s
atomic threads=4  expected=4000000  got=4000000  lost=0  time=0.054 s
atomic threads=4  expected=4000000  got=4000000  lost=0  time=0.053 s
```

Correct every run, and about 3× faster than the mutex (0.054 s vs 0.17 s) in this test.
</div>

### The "atomics are faster" trap

The spec warns: *do NOT conclude atomics are always faster than mutexes.* Here's all the timing data together, and what it actually measures:

| | 1 thread (no contention) | 4 threads (heavy contention) | Correct? |
|---|---|---|---|
| unprotected | 0.015–0.019 s | 0.044 s | 4 threads: **no**, lost ~2/3 |
| `std::atomic` | 0.032 s | 0.054 s | yes |
| `std::mutex` | 0.087 s | 0.17 s | yes |

(4 million increments in total in every case.)

- **This is a microbenchmark.** Each thread does *nothing* except increment the same counter: the worst possible contention, and the best case for showing differences. A Pulse request does ~0.2 ms of other work per increment. Whether the increment takes 10 ns or 40 ns will be invisible next to that. **Measure in Pulse, under Pulse's load,** before concluding anything (Week 4).
- **Even uncontended, protection costs something** (1-thread column): the `lock` prefix and the mutex's internal atomic operations aren't free. Correct code is slower than broken code. The broken version's speed doesn't count, because its answer is wrong.
- **An atomic only protects one operation on one variable.** It can't keep *several* variables consistent with each other (Part 8). A mutex can protect any amount of code and data. They're different tools, not a faster and slower version of the same one.

### Atomic pitfall: `x = x + 1` is NOT atomic

```cpp
std::atomic<long> n{0};
n++;            // ✅ one atomic read-modify-write   (same as n.fetch_add(1))
n += 1;         // ✅ also one atomic operation
n = n + 1;      // ❌ an atomic READ, then a separate atomic WRITE. Another thread can get in between
```

`n = n + 1` reads `n` atomically, adds 1 in a register, then stores atomically. That's two atomic steps with a gap between them, which is Part 2's lost update again. **Only the operators that are one operation (`++`, `--`, `+=`, `-=`, `fetch_add`, …) are atomic as a whole.**

The other basic operations: `n.load()` reads the current value, `n.store(v)` writes one, and `n.fetch_add(k)` adds `k` and returns the value from *before* the add. Atomics also take a "memory order" argument. The default (`std::memory_order_seq_cst`) is the strictest and the right choice here, so nothing else needs specifying.

---

# Part 8 — Designing Pulse's statistics

### Connection-local vs server-global (the spec's "fundamental distinction")

| Belongs to **one connection** → lives on that worker's stack, needs no protection | Belongs to **the whole server** → shared by all threads |
|---|---|
| the client's `Socket` (fd) | `router_` (read-only, so safe as is) |
| `raw_request` (request buffer) | the statistics counters (**written**, so need protection) |
| `response` (response buffer) | configuration (port, limits). Set once before threads start, then read-only |
| parser state (`ParseResult`) | shutdown state (not in Pulse yet. Week 2 doc, Part 5) |
| | the log (`std::cout`, written. Part 9) |

**The design rule:** keep as much as possible in the left column. Every item in the right column that's *written* needs a deliberate protection decision, and the spec's Week 3 deliverable is to **document that decision** for each one.

### The counters, one by one

| Statistic | Where in Pulse's code it changes | Something to work out |
|---|---|---|
| **total connections** | after `accept()` (main thread) or at the start of a worker | If only the main thread ever changes it, is it still shared? (Who *reads* it?) |
| **active connections** | +1 when a worker starts, −1 when it ends | The −1 must happen on **every** way a worker can end: a normal response, client disconnect, 400, an exception caught in `serve_connection`. Sound familiar? It's the same "must happen on every exit path" problem `Socket` solves with RAII. A tiny guard object whose constructor does +1 and destructor does −1 makes it impossible to miss |
| **total requests** | when a request is parsed as Complete | Does a Malformed request (400) count as a request? Decide, and write the decision down |
| **2xx / 4xx / 5xx** | once the response is decided, before writing it | `HttpResponse` currently has **no way to read its status code back** (`status_code_` is private, with no getter). It needs one |

**About 5xx:** Pulse **never sends a 5xx today**. If a route handler throws, the exception goes up to `serve_connection`, which logs it and drops the connection without sending anything. A real server would answer **`500 Internal Server Error`** ("the server broke, not your request"). Catching a handler's exception in `handle_client` and turning it into a 500 response would make the 5xx counter mean something, and give the client a real answer instead of a dropped connection.

### Where the stats live, and how `/stats` reads them

To see the numbers, a `GET /stats` route is the natural fit: it returns the counters as text. That raises two design questions:

1. **Lifetime and access.** Route handlers are lambdas built in `main.cpp`'s `build_router()`, *before* the server exists. A `/stats` handler needs to reach the stats object. The same lifetime argument as `server` in Week 2 (doc Part 5) applies: whatever holds the stats must outlive every worker thread. An object created in `main()` and captured by reference, `[&stats]`, is safe for the same reason `server` is: `main()` never returns while workers exist.
2. **A consistent snapshot.** `/stats` reads several counters. With six separate atomics, it reads them one at a time, and other workers keep updating them in between. So a reader can see, for a moment, `total requests = 10` but `2xx + 4xx = 9`, because one request was counted in one variable and not yet in the other. Each number is correct on its own, but they don't add up *to each other*.

| | Six `std::atomic` counters | One `std::mutex` around a stats struct |
|---|---|---|
| Each increment | one `lock add`, no waiting | lock → increment → unlock |
| `/stats` snapshot | each value correct, **but they may not add up to each other** | consistent: copy everything under the lock |
| Counters that must change together (e.g. total + 2xx) | can't be made to change "at the same time" | updated together under one lock |

Neither is wrong. **Atomics are right for independent counters. A mutex is right when several values must agree.** The spec asks for both, compared, and correctness comes first: unsafe → mutex → atomic, *then* measure.

---

# Part 9 — Fixing Week 2's scrambled log

Week 2 / Test 6 caught a real scrambled line:

```
[TID [TID 549548] Parsed (] Parsed (87 bytes total): 87 bytes total): GET /helloGET /hello HTTP/1.0
```

It's the same shape of problem as the counter: one log line is about ten separate `std::cout <<` operations, and other threads can slip in between them. (Technically `std::cout` itself isn't a *data race*. The standard makes each `<<` safe on its own, so nothing is corrupted and only the *order* mixes. It's a race condition, Part 4.)

The fix has two parts:

1. **Build the whole line first, privately.** `std::ostringstream` (from `<sstream>`) is a string you can `<<` into, exactly like `std::cout`. Build the line in a *local* `ostringstream`, on the worker's own stack, so there's no sharing and no lock needed. Then `.str()` gives the finished line.
2. **Write the finished line under a mutex**, a log mutex, in **one** `<<`. The lock is held only for that single write, not while building the line.

Building first keeps the critical section tiny (Part 6's rule). Every log call in `run()` and `handle_client()` would go through one small helper that does exactly this.

---

# Part 10 — Two more things that come with locks

### Deadlock

If thread A holds mutex 1 and waits for mutex 2, while thread B holds mutex 2 and waits for mutex 1, **both wait forever**. That's a **deadlock**. In `ps -L` it looks like threads asleep in `futex_wait_queue` that never wake up, with zero CPU and zero progress.

For Pulse this week there will be at most two mutexes (stats and log). The simple rule that makes deadlock impossible: **never hold one while locking the other**. Lock stats, update, unlock, *then* log. Never "lock stats, then log while still holding it". (When code genuinely needs two locks at once, it must always take them in the same order. `std::scoped_lock` can lock several mutexes at once safely. Pulse doesn't need it.)

### `condition_variable` (in the Month 2 learning goals, not built this week)

A mutex answers "only one thread *at a time*". A **`std::condition_variable`** answers a different question: **"sleep until something becomes true"**. For example, a thread waiting until a queue has work in it, without checking in a busy loop and burning CPU (Week 1 / Experiment 2).

```
consumer: lock → while (queue empty) cv.wait(lock) → take item → unlock
producer: lock → push item → unlock → cv.notify_one()
```

- `cv.wait(lock)` needs a **`unique_lock`** (that's one reason `unique_lock` exists). It unlocks the mutex *while* sleeping, so the producer can get in, and re-locks it before returning.
- The `while`, not an `if`: a thread can wake up without the condition being true, so it re-checks every time.

Pulse doesn't need it yet: every connection gets a brand-new thread, so nobody waits for work. **Month 5's controlled worker pool** is built on exactly this: a fixed set of threads sleeping on a condition variable until the accept loop puts a connection in a queue. You'll also recognize it in `ps`: a thread in `cv.wait` sleeps on a futex too.

---

# Part 11 — How Week 3 will be tested

Detailed commands, predictions and results go in `Week3/notes.html` once the code exists, as before. The plan:

1. **Unsafe counter first.** Add `total_requests++` with no protection. Hit Pulse with many concurrent requests (`ab -n 10000 -c 50`) and compare the counter (via `/stats`) to the number of requests sent. **Predict first.** Part 3 says it may show the right number. Either result is informative.
2. **TSan on the unsafe version.** It should report the data race even if step 1 counted correctly.
3. **Mutex version.** Exact count, and TSan clean.
4. **Atomic version.** Exact count, and TSan clean.
5. **The full statistics.** Total and active connections, total requests, 2xx/4xx/5xx, checked against known traffic. `active` should return to 0 when everything finishes. Check it during and after a batch of `/slow` requests.
6. **The log.** `ab -c 10` with the log on `/tmp` (Week 2's lesson): zero scrambled lines.
7. **Mutex vs atomic, measured in Pulse.** `ab` throughput for each version, log on `/tmp`, same conditions. Only *after* both are proven correct.

---

# Part 12 — Concept check

**Why isn't `total_requests++` safe from two threads, when it's one line of C++?**
It compiles to three steps, read → add → write (`mov`/`add`/`mov`). Between the read and the write, another thread can read the same old value, and one update overwrites the other.

**At `-O2`, `counter++` is one `add` instruction. Why is that still not safe?**
Without the `lock` prefix, the CPU still does read-modify-write internally, and another core can access the memory in between. Only `lock add` (what `std::atomic` produces) is indivisible across cores.

**In Part 3, 2 threads × 1,000 increments counted correctly five times. Why doesn't that prove anything?**
The threads barely overlapped: each finished in microseconds, often before the other started. The race needs an overlap at the exact moment of the increment. A bug that didn't happen in a test is still a bug. TSan proved the race was there.

**Why might Pulse's unprotected counter look correct even under `ab`?**
Each increment is a few nanoseconds inside ~0.2 ms of other work per request, so two workers rarely hit it at the same instant. Rare is not never.

**What's the difference between a data race and a race condition?**
Data race: unsynchronized concurrent access to the same memory with at least one write. That's undefined behavior. Race condition: the result depends on timing. It can happen even without a data race, like an atomic "check, then increment".

**Why is a data race undefined behavior and not just "maybe the wrong number"?**
The compiler may assume no other thread changes unsynchronized data, so it can cache values in registers or merge and reorder accesses. The behavior can change with optimization level or unrelated edits.

**What does a mutex protect, exactly?**
The critical section between lock and unlock, and only against other threads that lock the *same* mutex. Any access that skips the lock is unprotected.

**Why use `std::lock_guard` instead of `lock()` / `unlock()`?**
RAII: the destructor unlocks on every exit path, including early returns and exceptions. A missed `unlock()` leaves the mutex locked forever and hangs every thread that wants it.

**What's wrong with `std::lock_guard<std::mutex>{m};`?**
It has no name, so it's a temporary destroyed at the end of that line. It locks and immediately unlocks, protecting nothing.

**Why must you never sleep or do socket I/O while holding the stats mutex in Pulse?**
Every other worker that needs the mutex waits in `futex_wait_queue` until it's released, so one slow worker would block all the others. That's Month 1's problem moved into a lock.

**Why does a mutex member of `TcpServer` need `mutable`?**
`handle_client` is a `const` member function, and locking modifies the mutex. `mutable` allows that member to change inside `const` methods.

**What makes `std::atomic<long>::operator++` indivisible?**
It compiles to `lock add`. The `lock` prefix makes the read-modify-write atomic across all cores.

**Is `n = n + 1` atomic if `n` is a `std::atomic<long>`?**
No. It's an atomic load followed by a separate atomic store, with a gap where another thread can get in. Use `n++`, `n += 1` or `n.fetch_add(1)`.

**The atomic was ~3× faster than the mutex in Part 7. Why not conclude atomics are faster?**
It was a microbenchmark of pure contention. In Pulse, the increment is tiny next to each request's real work. Atomics also only cover single operations on single variables. Measure in the real program, under real load.

**When is a mutex the better choice than atomics for Pulse's stats?**
When several counters must be consistent with each other: updated together, and read as a matching snapshot by `/stats`.

**How do you make sure `active_connections` is decremented on every way a worker can end?**
RAII: a small guard object that increments in its constructor and decrements in its destructor, created at the start of the worker. The destructor runs on normal return and during exception unwinding.

**Why build a log line in an `ostringstream` before taking the log mutex?**
Building happens on the worker's own stack with no sharing, so the lock is held only for one short write. That keeps the critical section tiny.

**How do you avoid deadlock with a stats mutex and a log mutex?**
Never hold one while taking the other: finish and unlock stats, then log. If two locks are ever needed together, always take them in the same order.

**What does a `condition_variable` do that a mutex doesn't, and when will Pulse need one?**
It lets a thread sleep until a condition becomes true, without busy-waiting. Pulse needs one for Month 5's worker pool, where threads wait for connections in a queue.
