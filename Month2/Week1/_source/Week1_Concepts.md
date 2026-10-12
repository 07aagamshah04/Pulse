---
title: "Pulse — Month 2, Week 1: Concepts"
subtitle: "Threads, explained through Pulse's own code"
---

# How to use this document

Week 2 will add essentially one line to `TcpServer::run()`. The spec's version:

```cpp
std::thread(handle_client, client_fd).detach();
```

**This entire document exists to explain what that one line creates, why it fixes Pulse, and what new problems it brings.** Every part starts from Pulse: your `tcp_server.cpp`, your benchmark numbers, your server running. The general OS/C++ concept comes *after*, as the explanation of what you just saw.

Every output in this doc is real. It was produced by building and running **your actual Month 1 Pulse** (or a tiny demo where Pulse can't show it yet) in your WSL2 Ubuntu. Your PIDs and addresses will differ; the patterns won't.

| Part | The Pulse question it answers |
|---|---|
| 1 | What exactly goes wrong in Month 1 Pulse? (run it yourself) |
| 2 | *Where* in the code is Pulse stuck, and what is a "thread" in those terms? |
| 3 | What will Pulse look like with threads? |
| 4 | Which of Pulse's variables will threads share, and which does each get its own copy of? |
| 5 | Why doesn't a stuck thread freeze the rest of Pulse? |
| 6 | How will you *see* Pulse's threads from the terminal? |
| 7 | What does `std::thread(handle_client, client_fd)` actually mean, word by word? |
| 8 | Why `.detach()` and not `.join()`? |
| 9 | What will Pulse's log output look like with threads? |
| 10 | This week's experiments, and which Pulse question each one answers |
| 11 | Concept check |

---

# Part 1 — The problem, on your own server

Your Month 1 benchmark said: *CPU near 0–12% of one core, memory flat, "not CPU-bound."* Here's what that means in practice. Run this yourself; it's the "before" picture for all of Month 2.

<div class="example">
<b>Real run: one silent client freezes Pulse for everyone.</b>

```bash
# terminal 1
./build/pulse 8080

# terminal 2: a client that connects and then sends NOTHING (holds the connection open)
nc localhost 8080

# terminal 3: a normal, perfectly valid request, given 5 seconds to finish
curl -s -m 5 -w "http_code=%{http_code}\n" localhost:8080/hello
```

What actually happened (port 8090 in this run):

```
curl result: http_code=000
curl exit code: 28
curl took: 5.040167710 s
```

<code>/hello</code> normally answers in about <b>0.010 s</b>. Here curl waited the full 5 seconds and gave up (<code>exit code 28</code> = curl's "timeout"; <code>http_code=000</code> = no response at all). As soon as the silent <code>nc</code> was closed, the same <code>curl</code> got <code>Hello from Pulse!</code> in 0.010 s.
</div>

Now, *while* curl was stuck, here's what Pulse and the kernel looked like:

<div class="example">
<b>Real run: looking inside, while curl waits.</b>

```
$ grep Threads /proc/485/status
Threads:	1
```
Pulse has exactly <b>one</b> thread. That's the root of everything below.

```
$ ps -L -o tid,stat,wchan:22,comm -p 485
    TID STAT WCHAN                  COMMAND
    485 S+   wait_woken             pulse
```
That one thread is in state <code>S</code> = <b>sleeping</b>, and <code>WCHAN</code> ("wait channel": the kernel function it's parked in) is <code>wait_woken</code>, i.e. waiting for an event, here "data to arrive on a socket". It isn't using any CPU. It's waiting for the silent client to send something, which it never will.

```
$ ss -ltn "sport = :8090"
State  Recv-Q Send-Q Local Address:Port Peer Address:Port
LISTEN 1      128          0.0.0.0:8090      0.0.0.0:*

$ ss -tn state established "sport = :8090"
Recv-Q Send-Q Local Address:Port Peer Address:Port
0      0          127.0.0.1:8090    127.0.0.1:44922     ← the silent nc client
83     0          127.0.0.1:8090    127.0.0.1:44936     ← curl
```
This is the most revealing output in this whole doc:
<ul>
<li><b>LISTEN <code>Recv-Q 1</code></b>: on a listening socket, Recv-Q is the number of connections the kernel has <i>fully finished</i> the TCP handshake for, sitting in the <code>listen()</code> backlog queue waiting for Pulse to call <code>accept()</code>. That <code>1</code> is curl. (The <code>128</code> is your backlog size from <code>TcpServer</code>'s constructor.)</li>
<li><b>curl's connection, <code>Recv-Q 83</code></b>: curl has already sent its whole request. All <b>83 bytes</b> of <code>GET /hello HTTP/1.1 ...</code> are sitting in the kernel's receive buffer for that connection. Pulse just never read them.</li>
<li><b>nc's connection, <code>Recv-Q 0</code></b>: nothing sent, so nothing to read. This is the connection Pulse's one thread is stuck <code>read()</code>ing.</li>
</ul>
</div>

**So the kernel did all its work for curl.** The handshake was done, the request bytes had arrived, and everything was ready. Pulse never got to it, because its one and only thread was asleep inside `read()` for a *different* client. That's what "not CPU-bound" looks like in practice: no shortage of CPU, just a shortage of *places to wait from*.

<div class="analogy">
<b>Analogy:</b> a restaurant with one waiter. The waiter has gone to table 1, and table 1 says "hmm, give me a minute to decide…" and then just sits there. The waiter, by the rules of this restaurant, stands at table 1 until they order. Meanwhile table 2 has already written their order on a slip and left it on the counter (curl's 83 bytes in Recv-Q). The kitchen is idle, the order is ready, and nobody picks it up. The fix isn't a faster waiter. It's a second waiter.
</div>

---

# Part 2 — *Where* is Pulse stuck? (stack and program counter, in your code)

"Pulse's thread is stuck in `read()`" can be made exact. A debugger can pause a running program and print the chain of function calls it's currently inside. Here's Pulse, paused while stuck on the silent client:

<div class="example">
<b>Real run: <code>gdb</code> backtrace of Month 1 Pulse, stuck on a silent client.</b>

```
#0  __internal_syscall_cancel (a1=4, a2=140737488343104, a3=4096, ...) at ./nptl/cancellation.c:40
#1  __syscall_cancel (a1=4, a2=140737488343104, a3=4096, ...)          at ./nptl/cancellation.c:75
#2  __GI___libc_read (fd=..., buf=..., nbytes=...)                      at ../sysdeps/unix/sysv/linux/read.c:26
#3  TcpServer::handle_client (this=0x7fffffffe130, client=...)          at src/tcp_server.cpp:232
#4  TcpServer::run (this=0x7fffffffe130)                                 at src/tcp_server.cpp:167
#5  main (argc=2, argv=0x7fffffffe2c8)                                   at src/main.cpp:126

  Id   Target Id                               Frame
* 1    Thread 0x7ffff7f7f780 (LWP 456) "pulse" __internal_syscall_cancel (...)
```

Read it bottom-up. That's the order the calls happened in:

<ul>
<li><b>#5 <code>main</code></b>, at <code>main.cpp:126</code>: that's the line where <code>main</code> calls <code>server.run()</code>.</li>
<li><b>#4 <code>TcpServer::run</code></b>, at <code>tcp_server.cpp:167</code>: the <code>handle_client(Socket(client_fd));</code> line inside the <code>for(;;)</code> accept loop.</li>
<li><b>#3 <code>TcpServer::handle_client</code></b>, at <code>tcp_server.cpp:232</code>: the <code>::read(client.fd(), chunk, sizeof(chunk))</code> line.</li>
<li><b>#2–#0</b>: inside glibc's <code>read()</code>, which is making the actual system call. <code>a1=4</code> is the fd (the client socket; fd 3 is the listening socket), <code>a3=4096</code> is <code>sizeof(chunk)</code>. Those are your arguments.</li>
<li>The last line, <code>info threads</code>: <b>one</b> thread in the whole program.</li>
</ul>
</div>

That printout *is* the two things a "thread" is made of:

**1. The stack.** Each line (`#0`…`#5`) is a **stack frame**: the memory for one function call that hasn't returned yet. Frame #3 holds `handle_client`'s local variables (`raw_request`, `chunk[4096]`, `response`), plus a note saying "when you return, go back to `run()` at line 167." Frame #4 holds `run()`'s locals (`client_addr`, `client_fd`). When a function returns, its frame is thrown away. That's why locals disappear when a function ends. This chain of frames is called **the stack**.

**2. The program counter.** Frame #0 is "the exact instruction being executed right now." The CPU keeps that address in a register called the **program counter** (on x86-64, `rip`). Together with a few dozen other **registers** (the CPU's own tiny, fastest storage, holding values it's working on right now), that's the CPU's entire notion of "where am I and what am I holding."

**So a thread is: one position in the code (program counter + registers) and one stack of "how did I get here" frames.** That's all it is.

Now the problem from Part 1 is easy to state precisely:

> Pulse has **one** program counter and **one** stack. Right now they say "inside `read()`, inside `handle_client` for the silent client." For Pulse to *also* be inside `accept()` for curl, or inside `handle_client` for curl, it needs a **second** program counter and a **second** stack, at the same time. That is a second thread.

<div class="analogy">
<b>Analogy:</b> you're following a recipe with one finger on the current step (program counter), and a pile of sticky notes like "after making the sauce, return to step 6" (the stack). One finger can only be on one step. If the step says "wait for the dough to rise" (<code>read()</code>), you're stuck there. A second cook with their own finger and their own sticky notes can be on a completely different recipe at the same time, in the same kitchen.
</div>

---

# Part 3 — What Pulse will look like with threads

Today (Month 1), one thread does everything, in order:

```
main thread:  main → run → accept → handle_client(A) → read ... (stuck while A is slow)
                                  → handle_client(B) only AFTER A is completely finished
```

After Week 2: the main thread only accepts, and every connection gets its own thread, with its own program counter and its own stack:

```
main thread:   main → run → accept()                       ← always back here, ready for the next client
thread A:      handle_client(silent nc) → read()           ← stuck, asleep. Only affects A.
thread B:      handle_client(curl /hello) → write()        ← finishes in ~0.01 s, independently of A
```

If you ran the same gdb command on Week 2 Pulse during the same experiment, `info threads` would list **three** threads, and each would have its own backtrace like the one in Part 2. Thread A's backtrace would end in `read()`. The main thread's would end in `accept()`.

The key fact that makes this work: **`handle_client` barely has to change.** Each call to it, in each thread, gets its own stack frame on its own stack, so each connection automatically gets its own `raw_request`, its own `chunk[4096]`, its own `response`. Two threads running `handle_client` at the same moment don't collide on those, any more than two recursive calls of the same function would.

But not *everything* `handle_client` touches lives on its own stack. That's Part 4.

---

# Part 4 — What Pulse's threads will share, and what each owns

## Start here: the simple version

Forget Pulse for a minute. This whole part comes down to **one rule**, and you can see it with two plain `int`s.

### Step 1: notebook vs whiteboard

Picture a classroom:

- Every student has their **own notebook**. What you write in your notebook, nobody else sees or changes.
- There is **one whiteboard** at the front. Everybody sees it. If one student writes on it, every other student sees the change.

In a program with threads:

- Each **thread** is a student.
- A thread's **stack** is its notebook. That's where the **local variables** of the functions it's running go: anything declared *inside* a function.
- **Global variables** (declared *outside* any function), the **heap** (anything made with `new`), and the **open files/sockets** are the whiteboard. There is only one of each, for all threads.

### Step 2: see it with two ints

<div class="example">
<b>Real run: a local <code>int</code> vs a global <code>int</code>, with two threads.</b>

```cpp
int total = 0;   // global: declared OUTSIDE any function   → whiteboard

void count_three(const char* who) {
    int mine = 0;   // local: declared INSIDE the function → notebook
    for (int i = 0; i < 3; ++i) {
        mine++;
        total++;
        std::printf("%s: mine = %d   total = %d\n", who, mine, total);
    }
}

int main() {
    std::thread t1(count_three, "thread1");
    t1.join();                               // let thread1 finish first, so the output is easy to read
    std::thread t2(count_three, "thread2");
    t2.join();
}
```

Actual output:

```
thread1: mine = 1   total = 1
thread1: mine = 2   total = 2
thread1: mine = 3   total = 3
thread2: mine = 1   total = 4      ← mine starts again from 1. total does NOT.
thread2: mine = 2   total = 5
thread2: mine = 3   total = 6
```

Both threads ran the <b>same function</b> with the <b>same variable names</b>. But:

<ul>
<li><b><code>mine</code></b> started back at <code>1</code> for thread2. thread2 got its <b>own, fresh</b> <code>mine</code> in its own notebook (stack). thread1's counting never touched it.</li>
<li><b><code>total</code></b> carried on from <code>3</code> to <code>4</code>. There is only <b>one</b> <code>total</code>, on the whiteboard, and both threads wrote to that same one.</li>
</ul>

(The <code>join()</code> between the threads is only there to keep the output tidy, one thread after the other. If both threads ran at the same time and both did <code>total++</code>, they could get in each other's way. That's exactly the Week 3 problem. For now we're only asking <i>where</i> things live.)
</div>

### Step 3: the same rule, applied to Pulse

Now replace `count_three` with `handle_client`, and two students with two clients. In Week 2, two clients connected at the same time means two threads, each running `handle_client`:

```
   Thread A's notebook (stack)              Thread B's notebook (stack)
   handling the silent nc client            handling curl /hello
  +------------------------------+         +------------------------------+
  | handle_client's locals:      |         | handle_client's locals:      |
  |   raw_request = ""           |         |   raw_request = "GET /hello…"|
  |   chunk[4096]                |         |   chunk[4096]                |
  |   response                   |         |   response                   |
  |   client  → fd 4             |         |   client  → fd 5             |
  +------------------------------+         +------------------------------+
                 \                                      /
                  \                                    /
                   v                                  v
  +----------------------------------------------------------------------+
  |                    THE WHITEBOARD: one copy, shared                   |
  |                                                                      |
  |   the TcpServer object  →  router_ (the route table), port_,          |
  |                            listen_socket_                             |
  |   std::cout  (the terminal everyone's log lines go to)               |
  |   the fd table:  3 = listening socket, 4 = nc's connection,           |
  |                  5 = curl's connection                               |
  |   (Week 3) total_requests counter                                    |
  +----------------------------------------------------------------------+
```

Read it the same way as the `int` demo:

- **`raw_request`, `chunk`, `response`** are like `mine`. They're declared *inside* `handle_client`, so each thread gets its own. Thread A's `raw_request` being empty (nc sent nothing) has **no effect** on thread B's `raw_request`, which holds curl's request. That's why `handle_client` barely needs to change for threads: its working data is already private.
- **`router_`** is like `total`. There's only one `TcpServer` object (created once in `main()`), so there's one `router_`, and both threads use that same one to look up routes. That's fine, because both only *read* it. Nobody changes the route table while the server is running.
- **`std::cout`** is like `total`. One terminal, shared. That's why log lines from two connections can mix together (Part 9).
- **The fd table** is like `total` too. fd `4` means "nc's connection" for *every* thread, not just thread A. If any thread closes fd 4, it's closed for all of them. That's the ownership question at the end of this part.

### Step 4: the rule in one sentence

> **Declared inside the function the thread is running → that thread's own private copy. Everything else → one shared copy that every thread sees.**

There's one catch to keep in mind. A private variable can still *point at* something shared. Thread A's `client` is its own `Socket` object, private to it, but the fd number inside it (`4`) refers to an entry in the shared fd table. A private notebook can still have "see the whiteboard, line 4" written in it.

---

## The full list, for reference

Same rule, now covering every variable Pulse touches:

| In Pulse | Where it lives | Week 2 consequence |
|---|---|---|
| `raw_request`, `chunk`, `response`, `result` in `handle_client` | That call's stack frame → **own copy per thread** | Safe. Every connection has its own. |
| `client_addr`, `client_fd` in `run()` | The **main** thread's stack | Overwritten by every new `accept()`. A worker must not depend on the main thread's variable staying the same (Part 7 shows the bug). |
| `TcpServer server` object (`listen_socket_`, `port_`, `router_`) | Constructed in `main()`, so in the **main thread's stack frame**, reached by every thread through `this` (`this=0x7fffffffe130` in the gdb output) | **Shared.** Every thread's `handle_client` reads the *same* `router_`. Fine as long as nobody modifies it (Week 3 asks why). |
| Anything `new`'d / heap (the contents of a `std::string`, `std::vector`) | The heap: one heap for the whole process | Shared, if two threads have a pointer to the same object. |
| Globals and `static` variables | One copy for the whole process | **Shared.** Week 3's `total_requests` counter will be one, and it's where race conditions come from. |
| `std::cout` | A global object | **Shared.** Log lines from different connections will mix (Part 9). |
| The **file descriptor table** (fd 3 = listen socket, fd 4, 5… = clients) | Kernel, one table per process | **Shared.** fd 4 means the same connection in every thread. This is where the Week 2 "who owns `client_fd`?" question comes from (below). |

<div class="example">
<b>Deeper proof (optional): the actual memory addresses.</b> Step 2 showed sharing through <i>values</i>. This shows it through <i>addresses</i>: where each variable physically sits in memory.

```cpp
int global_value = 42;

void show(const char* who, int* heap_ptr) {
    int local = 0;   // lives on THIS thread's stack
    std::printf("%-8s pid=%d tid=%ld  &local=%p  &global=%p  heap=%p\n",
                who, getpid(), (long)syscall(SYS_gettid),
                (void*)&local, (void*)&global_value, (void*)heap_ptr);
}

int main() {
    int* heap_ptr = new int(7);
    show("main", heap_ptr);
    std::thread t1(show, "thread1", heap_ptr);
    std::thread t2(show, "thread2", heap_ptr);
    t1.join();
    t2.join();
    delete heap_ptr;
}
```

Actual output:

```
main     pid=542 tid=542  &local=0x7ffeaa90fe64  &global=0x5664aa093010  heap=0x5664ab985020
thread1  pid=542 tid=543  &local=0x73a1d8bfece4  &global=0x5664aa093010  heap=0x5664ab985020
thread2  pid=542 tid=544  &local=0x73a1d83fdce4  &global=0x5664aa093010  heap=0x5664ab985020
```

<ul>
<li><b>Same <code>pid</code></b>, different <b><code>tid</code></b>: one process, three threads.</li>
<li><b><code>&global</code> and <code>heap</code> identical</b>: shared. All three see the exact same bytes.</li>
<li><b><code>&local</code> different in every thread</b>: same function, same variable name, three separate stacks. In Pulse terms: three connections, three separate <code>raw_request</code>s.</li>
<li><code>0x73a1d8bfece4 − 0x73a1d83fdce4 = 0x801000</code> = <b>8 MiB + 4 KiB</b>. Each thread gets an 8 MiB stack (the Linux default; <code>ulimit -s</code> prints <code>8192</code> KiB), plus one 4 KiB "guard page" with no access allowed, so that a stack overflow crashes immediately instead of overwriting the next thread's stack.</li>
</ul>
</div>

**Why the 8 MiB matters for Pulse:** thread-per-connection means **one 8 MiB stack reserved per open connection**. 100 connections ≈ 800 MiB of reserved address space; your WSL2 VM has 3.5 GiB of RAM. It isn't as bad as it sounds, because the kernel only gives a stack real RAM for the parts that are actually touched (`handle_client` uses a few KiB). But it's exactly the "why can't threads scale forever" question that **Week 4** measures. Your Month 1 baseline RSS of **3840 KB** is the number to compare against.

### Why threads, and not a separate process per connection?

Pulse *could* call `fork()` per connection: a whole new process, which is what early Apache did. The difference is exactly the shared/private split above:

<div class="example">
<b>Real run: a child process vs a thread changing the same global.</b>

```cpp
int counter = 0;
int main() {
    pid_t pid = fork();                          // new PROCESS (gets its own copy of all memory)
    if (pid == 0) { counter = 100; return 0; }
    waitpid(pid, nullptr, 0);
    std::printf("after child process set it: counter = %d\n", counter);

    std::thread t([] { counter = 100; });        // new THREAD (same memory)
    t.join();
    std::printf("after thread set it:        counter = %d\n", counter);
}
```

```
after child process set it: counter = 0
after thread set it:        counter = 100
```

A child process changes only <i>its own copy</i>. With fork-per-connection, Week 3's shared request counter couldn't simply be a variable. Every connection would be counting in its own private copy. Threads share by default: cheaper, simpler, and also the source of every Week 3 bug.
</div>

### The fd table being shared: the Week 2 ownership question

Today, in `run()`:

```cpp
int client_fd = ::accept(...);        // say it returns 4
handle_client(Socket(client_fd));     // Socket's destructor close()s fd 4 when handle_client returns
```

With threads, fd `4` is handed to a worker thread, and the fd table is shared, so `4` means *the same connection* in both threads. Now imagine the **main** thread accidentally closes it (for example, a `Socket` object in the accept loop goes out of scope and its destructor runs):

1. The worker's next `read(4, ...)` fails, because its connection is closed out from under it.
2. Worse: the next `accept()` reuses the lowest free number, so the *next* client also gets fd **4**. The worker is now reading **someone else's request** and may write its response to the wrong client.

That's why the spec asks, before any Week 2 code: **"Who owns `client_fd`?"** Your `Socket` class (RAII, move-only, closes in its destructor) is precisely the tool for making ownership explicit. *How* to hand it to a thread is Week 2's job. Part 7 gives you the pieces.

---

# Part 5 — Why a stuck thread won't freeze the rest of Pulse

Back to Part 1's output: Pulse's thread was `S` (sleeping) in `wait_woken`. What does sleeping mean to the kernel?

Your WSL2 VM has **12 CPUs** (`nproc` → 12). Each CPU runs one thread at a time. The kernel's **scheduler** decides which thread runs on which CPU. Every thread is, roughly, in one of two states:

| State | `ps` letter | Meaning | Pulse example |
|---|---|---|---|
| **Runnable / running** | `R` | Has work to do; on a CPU or waiting for one | Parsing a request, building a response |
| **Sleeping (blocked)** | `S` | Waiting for an event. **The scheduler ignores it entirely and it uses 0 CPU** until the event happens. | Inside `read()` waiting for bytes, inside `accept()` waiting for a client |

When the silent client's thread is in `read()`, it's `S`. It isn't spinning, it isn't taking a turn, it's simply parked. The scheduler runs whatever threads *are* runnable: the main thread returning from `accept()`, curl's thread parsing and writing. When bytes finally arrive for the silent client's socket, the kernel wakes that thread (`S` → `R`), and it continues from exactly where frame #0 left off.

> **Blocking I/O + multiple threads = concurrent connections.** Blocking was never the problem in Month 1. Blocking the *only* thread was.

### Concurrent vs parallel, and which one Pulse actually needs

- **Concurrent**: several things *in progress* at overlapping times (they might take turns on one CPU).
- **Parallel**: several things *executing at the same instant* on different CPUs.

Your benchmark showed Pulse uses ~0–12% of **one** core. Pulse doesn't need more computing happening at once; it needs **more waiting happening at once**. That's concurrency. Pulse would get the fix from Part 1 even on a 1-CPU machine, because the slow client's thread is asleep, not using the CPU.

<div class="analogy">
<b>Analogy:</b> a chess master playing 20 opponents simultaneously walks board to board. 20 games are <b>concurrent</b>, but only one move happens at a time. It works because most of each game is the <i>opponent thinking</i>, and the master doesn't stand there waiting for it. Pulse's connections are mostly "the client (network) thinking."
</div>

### Context switches: the cost you'll measure in Week 4

When a CPU stops running thread A and starts thread B, the kernel saves A's registers/program counter and loads B's. That's a **context switch** (the same "save the finger position, resume later" idea from Part 2). It's cheap, but not free, and with hundreds of Pulse threads there are lots of them. You can count them per process:

```bash
grep ctxt /proc/<pid>/status
voluntary_ctxt_switches:     ...   # thread blocked (read/accept/sleep) and gave the CPU up itself
nonvoluntary_ctxt_switches:  ...   # scheduler took the CPU away because its time slice was up
```

For Pulse, nearly all will be voluntary: threads blocking in `read()`/`accept()`.

---

# Part 6 — Seeing Pulse's threads from the terminal

To Linux, a thread is a **task**, the same kind of thing as a process. The only difference is that it's created (via the `clone()` system call) *sharing* its creator's memory and fd table instead of getting its own. That's why the tools show threads almost like processes:

| ID | Meaning | Where you see it |
|---|---|---|
| **TID** | One per thread, unique | `SPID` column in `ps -T`, `TID`/`LWP` in `ps -L`, `/proc/<pid>/task/<tid>` |
| **PID** (as `ps`/`getpid()` show it) | One per process; the same for all its threads; equals the **main** thread's TID | `PID` column. In Part 4's demo, main had `pid=542 tid=542`. |

(`LWP` = "light-weight process," an old name for a thread that some `ps` columns still use. Same thing as TID.)

<div class="example">
<b>Real run: what Week 2 Pulse will look like with 3 connections open.</b> Here faked with a tiny program whose 3 extra threads just sleep, since Pulse can't do it yet:

```
$ ps -T -p 454
    PID    SPID TTY          TIME CMD
    454     454 pts/0    00:00:00 sleepers      ← main thread  (in Pulse: the accept loop)
    454     456 pts/0    00:00:00 sleepers      ← thread 1     (in Pulse: connection 1)
    454     457 pts/0    00:00:00 sleepers      ← thread 2     (in Pulse: connection 2)
    454     458 pts/0    00:00:00 sleepers      ← thread 3     (in Pulse: connection 3)

$ grep -E "^(Threads|VmSize|VmRSS)" /proc/454/status
VmSize:	   31484 kB          ← reserved address space: includes 3 × 8 MiB thread stacks
VmRSS:	    3712 kB          ← physical RAM actually used: the stacks are mostly untouched
Threads:	4
```

Compare Part 1: Month 1 Pulse showed <code>Threads: 1</code>, even with 2 clients connected. In Week 2, that number going up and down as clients connect and disconnect is how you'll know thread-per-connection is working.
</div>

**Tools you'll use** (first install the missing two: `sudo apt install htop sysstat`):

| Command | What it shows | Use it for |
|---|---|---|
| `grep Threads /proc/<PID>/status` | Thread count, right now | Quickest check |
| `ps -T -p <PID>` | One row per thread | Snapshot |
| `ps -L -o tid,psr,stat,wchan:20,pcpu,comm -p <PID>` | Per thread: which CPU (`PSR`), state (`R`/`S`), what it's waiting in (`WCHAN`), CPU% | "Which threads are stuck, and where" |
| `top -H -p <PID>` | Live, per thread; press `1` for per-CPU bars | Watching CPU use move around |
| `htop` | Same, nicer; `H` toggles threads, `F4` filters | Interactive |
| `pidstat -t -p <PID> 1` | Per-thread CPU, printed every second | Pasting into `notes.txt` |

**Reading `%CPU` with threads:** plain `top` sums all of a process's threads, so it can exceed 100%. Four threads, each fully busy on its own CPU = ~400%. That's "four CPUs' worth," not a bug. In `top -H`, each thread row maxes at ~100%.

---

# Part 7 — `std::thread(handle_client, client_fd)`, word by word

Now the C++ side. Take the spec's Week 2 line apart.

### `#include <thread>` and `-pthread`

`std::thread` is in the `<thread>` header (C++11 and later). On Linux it's built on top of the POSIX threads library ("pthreads"), so compiling needs the `-pthread` flag: `g++ -std=c++17 -Wall -pthread x.cpp`. In CMake, Pulse's `CMakeLists.txt` will need `find_package(Threads REQUIRED)` and `target_link_libraries(pulse PRIVATE Threads::Threads)` in Week 2. Under the hood, constructing a `std::thread` calls `pthread_create`, which calls the `clone()` system call from Part 6.

### `std::thread( WHAT_TO_RUN, ARGUMENTS... )`

- **First argument:** the thing to run: a function, or a lambda (like the route handlers in your `main.cpp`).
- **Remaining arguments:** passed to it. `std::thread(handle_client, client_fd)` means "on a new thread, call `handle_client(client_fd)`."
- **It starts running immediately**, inside the constructor. There's no `.start()`. By the time the *next line* of `run()` executes, the new thread may already be reading, may have finished, or may not have been scheduled yet. You can't know which, and correct code must not care.

(In real Pulse, `handle_client` is a *member function* of `TcpServer`, so it needs a `TcpServer` object to be called on. Passing a member function to `std::thread` has a specific syntax. That's a Week 2 detail; the spec's line is the concept.)

### Arguments are COPIED into the new thread, and why that's good for Pulse

`std::thread` makes its own copy of every argument before the new thread starts. Look at `run()` and you'll see why that matters: `client_fd` is a local of the accept loop. The moment the main thread loops back around and calls `accept()` again, `client_fd` gets **overwritten** with the next client's number. If the worker were still looking at the main thread's variable, it would suddenly see a different connection. Because `std::thread` copied the value, the worker has its own `4`, and the main thread can do whatever it likes with `client_fd` afterwards.

<div class="example">
<b>Real run: copy vs <code>std::ref</code>.</b>

```cpp
void add_ten(int& x)     { x += 10; }   // takes a REFERENCE: changes the caller's variable
void add_ten_copy(int x) { x += 10; }   // takes a COPY: changes only its own copy

int main() {
    int a = 1, b = 1;
    std::thread t1(add_ten_copy, a);      t1.join();
    std::thread t2(add_ten, std::ref(b)); t2.join();
    std::printf("a = %d (copy given to thread)\nb = %d (std::ref given to thread)\n", a, b);
}
```

```
a = 1 (copy given to thread)
b = 11 (std::ref given to thread)
```

<code>std::ref(b)</code> (from <code>&lt;functional&gt;</code>) is how you deliberately say "pass a reference, not a copy." Writing <code>std::thread t2(add_ten, b);</code> without it <b>won't compile</b>: <code>std::thread</code> passes its internal copy as a temporary (an rvalue, in your Month 1 &amp;/&amp;&amp; terms), and a non-const <code>int&amp;</code> can't bind to an rvalue. The library refuses to silently let you modify a copy you'd never see again.
</div>

### The classic Pulse bug this protects you from: lambdas capturing by reference

You might be tempted to write the Week 2 line with a lambda:

```cpp
std::thread([&] { /* use client_fd */ }).detach();   // [&] = capture everything BY REFERENCE
```

`[&]` means the lambda refers to the *main thread's* `client_fd` variable, not a copy. That's the exact bug from above: the next `accept()` overwrites it, and connection 1's thread may end up serving connection 2's socket. It works fine when you test with one curl at a time and breaks under load. The safe options are `[client_fd]` / `[=]` (capture **by copy**) or passing it as an argument (copied automatically).

| Capture | Meaning | For a Pulse worker thread |
|---|---|---|
| `[]` | Nothing captured | Safe |
| `[client_fd]` or `[=]` | Captured **by copy**, at the moment the lambda is created | Safe: the thread has its own value |
| `[&client_fd]` or `[&]` | Captured **by reference** to the main thread's variable | **Bug**: it changes on the next `accept()`, and dies when `run()`'s scope does |

### `std::thread` is move-only, exactly like your `Socket`

A `std::thread` object *owns* one running thread, the same way your `Socket` owns one fd. Two copies would both claim it. So, exactly as `Socket(const Socket&) = delete` in `socket.hpp`:

```cpp
std::thread a(work);
std::thread b = a;              // ERROR: copy constructor deleted
std::thread c = std::move(a);   // OK: c owns the thread now; a owns nothing
```

For the experiments, where you'll keep several threads in a `std::vector`: use `threads.emplace_back(work)` (builds it directly inside the vector, so there's no copy) and loop with `for (auto& t : threads)`. Note the `&`: `for (auto t : threads)` would try to copy each one and fails to compile.

The same move-only idea is what you'll use in Week 2 to hand a `Socket` *into* a thread so the worker becomes its single owner. That's the answer to "who owns `client_fd`?" taking shape.

### Small helpers you'll use in experiments

```cpp
std::this_thread::sleep_for(std::chrono::seconds(2));  // put THIS thread to sleep (state S); others keep running
std::this_thread::get_id();                            // a C++-level ID for the calling thread (printable with <<)
std::thread::hardware_concurrency();                   // CPUs available for true parallelism: 12 on your WSL2
```

`std::this_thread` is a namespace of functions that act on "whichever thread calls me." `std::chrono::seconds(2)` is a typed duration from `<chrono>`. `sleep_for(2)` won't compile, because C++ forces you to say the unit. Note that `get_id()` is *not* the TID you see in `ps -T`. To print that number, use `gettid()` (from `<unistd.h>`), as the Part 4 demo did.

---

# Part 8 — Why `.detach()`, not `.join()`?

Every `std::thread` object must have exactly one of these called on it before it's destroyed:

- **`t.join()`** = "the calling thread **waits here** until thread `t` finishes."
- **`t.detach()`** = "let `t` run on its own. I won't wait for it, and when it finishes the system cleans it up."
- **`t.joinable()`** = "does this object still own a thread I haven't joined/detached?" (`false` after either one, and after being moved from.)

### Try `join()` in Pulse's accept loop, and you get Month 1 back

```cpp
for (;;) {
    int client_fd = ::accept(...);
    std::thread t(handle_client, client_fd);
    t.join();                       // main thread waits for this connection to finish...
}                                   // ...before going back to accept() the next one
```

Timeline with the Part 1 experiment:

```
main:      [accept nc][ join: waiting for nc's thread ....................... forever ]
thread nc:            [ read() ... asleep, nc never sends ...................          ]
curl:      sitting in the listen backlog, Recv-Q 1, never accepted. Same as Month 1.
```

You'd have created a thread and then stood still waiting for it, which is exactly Month 1 with extra steps. That's why the spec's line ends in `.detach()`: the accept loop must **not** wait for any connection. It hands the connection off and goes straight back to `accept()`.

### The spec's question: "Why doesn't `t1.join(); t2.join();` make it single-threaded?"

The example above might make you think "join = sequential." It isn't. What made it sequential was **joining *before creating* the next thread**. Compare:

```cpp
std::thread t1(work_a);   // t1 STARTS here
std::thread t2(work_b);   // t2 STARTS here: both are running now
t1.join();                // main waits for t1 (t2 keeps running during this wait)
t2.join();                // main waits for t2 (often it's already done, so this returns immediately)
```

```
main: [create t1][create t2][ join(t1): waiting ............ ][join(t2)]
t1:         [============ work_a ============]
t2:                    [====== work_b ======]
                        ↑ both running at the same time
```

`join()` doesn't *start* or *run* anything. It only makes the **calling** thread wait. Both threads were already started by their constructors. Total time ≈ the longer job, not the sum. Experiment 2 lets you measure this.

So:

| Who uses it | Why |
|---|---|
| **Experiments this week: `join()`** | `main()` creates N threads, then must wait for all of them before exiting. Otherwise `main` returns, the process exits, and every thread is killed mid-work. |
| **Pulse, Week 2: `detach()`** | The accept loop runs forever and must never wait on a connection. |

### Forgetting both: the whole program crashes

<div class="example">
<b>Real run: a <code>std::thread</code> destroyed while still joinable.</b>

```cpp
int main() {
    std::thread t([] { std::puts("hello from the thread"); });
    // no join(), no detach()
}   // ← t's destructor runs here
```

```
$ ./forgot_join
terminate called without an active exception
Aborted
$ echo $?
134
```

The destructor of a still-joinable <code>std::thread</code> calls <code>std::terminate()</code>, which aborts the <b>whole process</b> (134 = 128 + signal 6, <code>SIGABRT</code>). "hello from the thread" never even printed. <b>In Pulse terms:</b> forget <code>.detach()</code> in Week 2 and the <i>first</i> client that connects crashes the entire server, because the temporary <code>std::thread</code> is destroyed at the end of that line.

Why so harsh? Auto-join in the destructor could hang forever (as in the Pulse example above). Auto-detach could leave a thread using freed memory. C++ refuses to guess, and crashes loudly so you can't miss it. (C++20's <code>std::jthread</code> auto-joins; Pulse stays on C++17.)
</div>

### Two things to know about `detach()` before Week 2

1. **A detached thread must not depend on anything that can die before it does.** If it holds a reference/pointer to a local in `run()` (the `[&]` bug from Part 7), that local can change or vanish. Give it copies, or move ownership in. (It *will* use `router_` through `this`, which lives in `main()`'s stack frame. That's safe only because `main()` never returns while `run()` is looping. Worth noticing.)
2. **When the process exits, every thread dies instantly**, mid-request, with no destructors run on those threads. Ctrl+C on Week 2 Pulse kills in-flight connections immediately. That's acceptable for now; "graceful shutdown" is the kind of problem the spec lists under server-global "shutdown state."

---

# Part 9 — What Pulse's log output will look like

`handle_client` prints lines like:

```cpp
std::cout << "Parsed (" << raw_request.size() << " bytes total): "
          << result.request.method << " " << result.request.path << ...;
```

That's not one print. It's **about ten separate `<<` calls**. `std::cout` is a single global object, shared by every thread (Part 4). The C++ standard guarantees two threads using it at once won't corrupt the stream itself, but it does **not** guarantee one thread's ten pieces stay together. With two connections being handled at once, you can get output like:

```
Parsed (Parsed (83 bytes total): GET 83 bytes total): GET /hello /health HTTP/1.1
HTTP/1.1
```

And more generally: **which thread runs first is not guaranteed.** Connection A's thread being *created* before B's doesn't mean A's code runs before B's. After creation, both are independent tasks and the scheduler picks. That's what Experiment 1 demonstrates in its simplest form.

Scrambled logs are the first, harmless, visible symptom of *threads sharing something*. The fix is coordinating access with a mutex, which is **Week 3**. For now, just learn to recognize it.

---

# Part 10 — This week's experiments, and the Pulse question each answers

Each is a separate tiny `.cpp` in `Month2/Week1/`, compiled with `g++ -std=c++17 -Wall -pthread`. They're not Pulse changes; they're small, isolated versions of things Pulse will do in Week 2.

| Experiment | What you build | Pulse question it answers |
|---|---|---|
| **0: Month 1 baseline** | Nothing new. Rerun Part 1 on your Month 1 Pulse, and record `Threads:`, `ps -L` state/WCHAN, `ss` Recv-Q, and curl's time. | The "before" for Week 2. Next week the same commands should show `/hello` answering in ~0.01 s with the silent client still connected. |
| **1: A and B** | Two threads, one printing `A`, one printing `B`. Run it many times in a loop, then make each print its letter many times. | If connection A's thread is created before B's, does A's code run first? (Part 9) |
| **2: 2 / 4 / 8 busy threads** | Each thread does a few seconds of pure CPU work (a loop, *not* `sleep_for`). Time it. Watch with `top -H`, `ps -L` (the `PSR` column), `pidstat -t`. | What happens when threads actually compete for CPUs, and what does `join()` after creating all of them do to total time? (Part 8) |
| **3: sleeping threads** | Same as 2, but each thread `sleep_for`s instead of computing. | What do Pulse's threads, mostly waiting in `read()`, look like to the scheduler? (Part 5) |

**Write predictions in `notes.txt` *before* running each one.** For example:

- Exp 1: will `A` always come first, since its thread was created first?
- Exp 2: if one thread's work takes T seconds alone, how long for 2 threads? 4? 8? What would change past 12 threads, and why?
- Exp 2: what will plain `top` show as the process's `%CPU` with 2 threads? With 8?
- Exp 3: what will `%CPU` be? What will `STAT` and `WCHAN` say?

Afterwards we go field by field through what you got, right and wrong, the same as Month 1 / Week 4.

---

# Part 11 — Concept check

**In Part 1, the kernel had already completed curl's handshake and received its 83-byte request. Why didn't curl get a response?**
Pulse's only thread was asleep inside `read()` for the silent client (`handle_client`, `tcp_server.cpp:232`), so it never went back to `accept()` curl's connection (LISTEN `Recv-Q 1`) or read its bytes (connection `Recv-Q 83`).

**In the gdb backtrace, what is "the stack" and what is "the program counter"?**
The stack is the chain of frames `main → run → handle_client → read`, each holding that call's locals and where to return. The program counter is the address of the instruction executing right now (frame #0). One program counter + one stack = one thread.

**Why does each Week 2 connection automatically get its own `raw_request` and `chunk` without any extra code?**
They're locals of `handle_client`, so they live in that call's stack frame, and each thread has its own stack, so each thread's call has its own frame.

**Which of these will Week 2's threads share: `raw_request`, `router_`, fd 4, `std::cout`, a global counter?**
Everything except `raw_request`. `router_` (through `this`), the fd table, `std::cout`, and globals are one copy for the whole process.

**Why is a thread blocked in `read()` not a problem in Week 2 when it was the whole problem in Month 1?**
A blocked thread is sleeping (`S`): it uses no CPU and the scheduler just runs other threads. In Month 1 the blocked thread was the *only* thread, so nothing else could run.

**Pulse uses ~0–12% of one core. Does it need parallelism or concurrency? Why?**
Concurrency. Its connections are mostly waiting on the network, not computing, so the win is waiting on many connections at once, not doing more computation at once.

**Why is `std::thread` copying its arguments good for `client_fd`?**
The main thread's `client_fd` is overwritten by the next `accept()`. The worker has its own copy, so it keeps the right connection.

**What's wrong with `std::thread([&] { handle(client_fd); }).detach();` in the accept loop?**
`[&]` captures the main thread's `client_fd` by reference. When the main thread accepts the next client, the value changes, so the worker can end up serving the wrong connection, or reading a variable that's gone out of scope.

**What happens if you `join()` each connection's thread right after creating it in `run()`?**
The accept loop waits for that connection to finish before accepting the next: Month 1's behavior again, with extra steps.

**Then why doesn't `t1.join(); t2.join();` make a program single-threaded?**
Both threads were started by their constructors before either join. `join()` only makes the caller wait; it doesn't start or run anything. Sequential behavior comes from joining one thread before *creating* the next.

**What happens if Week 2 Pulse forgets `.detach()`?**
The temporary `std::thread` is destroyed while still joinable, its destructor calls `std::terminate()`, and the whole server aborts on the first connection.

**Why does each thread's stack matter for Week 4?**
Each thread reserves an 8 MiB stack (+4 KiB guard page). With one thread per connection, the number of connections directly sets how many stacks, how many threads the scheduler juggles, and how many context switches happen.

**If the main thread closes `client_fd` after handing it to a worker, what can go wrong?**
The fd table is shared, so the worker's connection is closed under it. And the next `accept()` can reuse the same fd number, so the worker could read or write a *different* client's connection. Hence "who owns `client_fd`?"

**Why might two threads' `std::cout << ... << ...` lines come out scrambled?**
Each `<<` is a separate operation on the one shared `std::cout`. The standard prevents corrupting the stream, not interleaving between pieces. The fix (a mutex) is Week 3.
