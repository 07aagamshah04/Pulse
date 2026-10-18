---
title: "Pulse — Month 2, Week 2: Concepts"
subtitle: "Thread-per-Connection: handing each client to its own thread"
---

# How to use this document

Week 1 was threads in isolation: small programs, no Pulse changes. **This week, Pulse itself changes.** The spec's version of the change, in `TcpServer::run()`:

```cpp
client_fd = accept();
std::thread(handle_client, client_fd).detach();
```

It looks like one line. This doc is about everything that line drags in with it: who owns the connection once it's handed over, how a move-only `Socket` gets into a thread, what happens to errors on a thread, and how to prove it all works with a deliberately slow endpoint.

Same format as before: every part starts from Pulse's code or a real run, and every output shown is real (Month 1 Pulse or a small demo, built and run in your WSL). No final Pulse code here. That comes after you've read this, as usual.

| Part | The Pulse question it answers |
|---|---|
| 1 | Where are we starting from? |
| 2 | What exactly changes in `run()`, and what will Pulse look like afterwards? |
| 3 | Who owns `client_fd`, today and after the change? |
| 4 | How do you hand a move-only `Socket` to a thread, and which thread closes it? |
| 5 | The thread calls `handle_client`, a member function. How, and is `this` safe? |
| 6 | What happens to an error on a worker thread? (Today, one bad write can kill the whole server.) |
| 7 | The `/slow` endpoint: how do we prove one slow request no longer blocks others? |
| 8 | What else changes: CMake, logs, open fds, and what deliberately doesn't |
| 9 | How Week 2 will be tested |
| 10 | Concept check |

---

# Part 1 — Where we're starting from

**Month 1 Pulse, idle, waiting for a client:**

<div class="example">
<b>Real run: Month 1 Pulse with no clients connected.</b>

```
$ grep Threads /proc/$P/status
Threads:	1

$ ps -L -o tid,stat,wchan:22,comm -p $P
    TID STAT WCHAN                  COMMAND
    584 S+   inet_csk_accept        pulse
```

One thread, asleep (<code>S</code>) in <code>inet_csk_accept</code>: the kernel side of <code>accept()</code> ("<b>inet c</b>onnection <b>s</b>oc<b>k</b>et accept"). It's the fourth wait channel you've seen. The other three were <code>wait_woken</code> (Pulse stuck in <code>read()</code>, Experiment 0), <code>hrtimer_nanosleep</code> (<code>sleep_for</code>) and <code>futex_wait_queue</code> (<code>join()</code>), from Experiment 3.
</div>

**What Week 1 established, in Pulse terms:**

| Week 1 finding | What it means for Week 2 |
|---|---|
| **Experiment 0:** a silent client froze Pulse. curl's 83-byte request sat unread for 30 s, `Threads: 1` | The "before" picture. Week 2 must make the same commands show `/hello` answering in ~0.02 s |
| **Experiment 1:** creation order ≠ execution order. Output from two threads mixes | Connections may be handled in any order. Log lines from different connections will interleave |
| **Experiment 2:** busy threads beyond the CPU count wait in line and get force-switched | Pulse threads must not be busy for long. They aren't: Pulse isn't CPU-bound |
| **Experiment 3:** sleeping threads cost ~0 CPU. Each reserves 8 MiB but uses ~4.6 KiB of real RAM | A thread per slow client is cheap at moderate scale. Week 4 finds where it stops being cheap |

---

# Part 2 — What changes in `run()`, as a picture

Today's loop (the real code, comments removed):

```cpp
for (;;)
{
    int client_fd = ::accept(listen_socket_.fd(), ...);
    ...
    handle_client(Socket(client_fd));    // ← the main thread does ALL the work, then loops
}
```

The main thread does everything: accept, read, parse, route, write, close. Only then does it go back to `accept()`.

After Week 2, the main thread does one job: **accept, hand over, go straight back to `accept()`.** Everything else happens on a worker thread, one per connection.

```
                       Month 1                                     Week 2

main thread:  accept ─► read ─► parse ─► route ─►       main thread:  accept ─► hand over ─► accept ─► hand over ─► ...
              write ─► close ─► accept ─► ...                                      │                    │
                                                                                   ▼                    ▼
                                                                        worker A:  read ─► parse    worker B: read ─► ...
                                                                                   ─► route ─►
                                                                                   write ─► close ─► thread ends
```

**Experiment 0 again, after the change.** Same steps: silent `nc` connects, then `curl /hello`.

```
main:      [accept nc][hand to A][accept curl][hand to B][accept ... waiting for next client]
worker A:             [read() ... asleep, nc never sends .......................................]
worker B:                                    [read][parse][route][write][close] done in ~0.02 s
```

Worker A is stuck exactly where Month 1 Pulse was stuck. But it's stuck **alone**. The main thread and worker B never wait for it.

**What `ps -L` should show during that run** (predict these now, check them in testing):

| Thread | `STAT` | `WCHAN` | Doing what |
|---|---|---|---|
| main | `S` | `inet_csk_accept` | Waiting for the next client |
| worker for `nc` | `S` | `wait_woken` | Waiting in `read()` for bytes that never come |
| worker for `/slow` (Part 7) | `S` | `hrtimer_nanosleep` | The handler is sleeping |
| worker for `/hello` | (gone) | | Finished in milliseconds. Its thread ended |

The thread count goes **up and down** as clients connect and finish: 1 + one per connection currently being handled.

---

# Part 3 — Who owns `client_fd`?

The spec's most important question before writing code. Start with how ownership works **today**, because Week 2 keeps exactly the same rule.

### Today: ownership is already handed over, by a move

```cpp
int client_fd = ::accept(...);          // 1. the kernel creates the connection, gives us the number 4
handle_client(Socket(client_fd));       // 2. Socket(4) takes ownership of fd 4 (it will close() it)
                                        //    and is MOVED into handle_client's by-value parameter
void TcpServer::handle_client(Socket client) const
{
    ...                                 // 3. `client` is now the one and only owner of fd 4
}                                       // 4. `client` goes out of scope → ~Socket() → close(4)
```

At every moment there is **exactly one owner** of fd 4: first the bare `int` for a single line, then the temporary `Socket`, then the parameter `client`. It ends with exactly one `close()`, when `handle_client` returns, however it returns (RAII, Month 1 / Week 1).

### Week 2: the same rule, across a thread boundary

The connection still needs exactly one owner at every moment. The only difference is that the final owner, `handle_client`'s `client` parameter, now lives **on the worker thread's stack** (Week 1 doc, Part 4: each thread has its own notebook). So `close()` happens on the worker thread, when it finishes the request.

### The three ways to get it wrong

| Mistake | What happens | Why it's bad |
|---|---|---|
| **Two owners**: the main thread *also* closes fd 4 (say, a `Socket` left behind in the accept loop goes out of scope) | The worker's `read(4)` fails, because its connection was closed under it. Worse: the next `accept()` reuses the lowest free number, **4**, so the worker may read or write **a different client's** connection | The fd table is shared by all threads (Week 1 doc, Part 4). "4" means the same thing everywhere |
| **No owner**: nobody closes fd 4 | Every connection leaks an fd. `ls /proc/<PID>/fd` keeps growing. At about 1024 (`ulimit -n`), `accept()` starts failing with `EMFILE` ("too many open files") and Pulse stops accepting anyone | fds are a limited resource, per process |
| **Owner by reference**: the worker refers to the *main thread's* `client_fd` variable instead of its own copy | The main thread's variable changes on the next `accept()`, or dies at the end of the loop iteration. The worker ends up using the wrong fd | Shown for real below |

<div class="example">
<b>Real run: the "owner by reference" mistake.</b> A fake accept loop that hands out fds 4, 5, 6, 7, 8. Each worker captures <code>client_fd</code> with <code>[&amp;]</code>, by reference:

```cpp
for (int i = 0; i < 5; ++i) {
    int client_fd = fake_accept();                       // 4, 5, 6, 7, 8
    std::thread([&] {                                    // [&] = capture client_fd BY REFERENCE
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::printf("worker meant for one client is serving fd %d\n", client_fd);
    }).detach();
}
```

Actual output, the same in all 3 runs:

```
worker meant for one client is serving fd 8
worker meant for one client is serving fd 8
worker meant for one client is serving fd 8
worker meant for one client is serving fd 8
worker meant for one client is serving fd 8
```

<b>Five clients, and every worker served the last one.</b> Each worker held a <i>reference</i> to the main thread's <code>client_fd</code>. By the time they woke up 50 ms later, the loop had finished and that stack slot last held 8. (Strictly, it's worse than "wrong value": each iteration's <code>client_fd</code> is <i>dead</i> once its iteration ends, so the workers are reading a dead variable. That's undefined behavior, and it could print anything, or crash.)

The fix is one character, <code>[client_fd]</code> (capture by copy):

```
worker is serving fd 4
worker is serving fd 5
worker is serving fd 6
worker is serving fd 7
worker is serving fd 8
```

In real Pulse, the <code>[&amp;]</code> version would pass every test with one <code>curl</code> at a time, and send responses to the wrong clients under load.
</div>

> **The rule for Week 2:** at every moment, exactly one thing owns the connection, and ownership moves from the main thread to the worker. It is never shared, and never borrowed by reference.

<div class="analogy">
<b>Analogy:</b> a coat check. The attendant (main thread) takes your coat (the connection) and hands it, with its numbered tag, to a runner (worker thread). From then on the runner is responsible for that coat: only the runner returns it (close). If the attendant <i>also</i> thinks they still have it, two people may hand out tag 4, and someone gets the wrong coat. If nobody returns it, the rack fills up and the coat check stops taking coats. If the runner just writes down "whatever coat is on the counter right now" instead of taking <i>this</i> coat, they end up with whoever came last.
</div>

---

# Part 4 — Handing a move-only `Socket` to a thread

`Socket` can't be copied (`Socket(const Socket&) = delete`), only moved. Week 1 showed that `std::thread` **copies** its arguments into the new thread (Week 1 doc, Part 7). So how does a move-only object get in?

**Answer:** `std::thread` doesn't strictly copy. It *copies or moves*, depending on what you give it. Give it an **rvalue** (a temporary, or something wrapped in `std::move`, Month 1's `&&` discussion) and it **moves** it. That's exactly what a move-only type needs.

To see it happen, here's a tiny class that behaves like `Socket`: move-only, holds a number, and "closes" it in its destructor. Every line prints which thread (TID) it runs on.

<div class="example">
<b>Real run: a <code>Socket</code>-like object handed to a thread.</b>

```cpp
class Ticket {                         // move-only, like Socket
public:
    explicit Ticket(int n) : n_(n) { /* print "created" */ }
    Ticket(Ticket&& other) noexcept : n_(std::exchange(other.n_, -1)) { /* print "moved" */ }
    Ticket(const Ticket&) = delete;
    ~Ticket() { /* print "CLOSING n" if n_ != -1, else "empty, nothing to close" */ }
    ...
};

class Server {
public:
    void handle(Ticket t) const { /* print "working on ticket" */ }   // like handle_client(Socket client)

    void start_one(int n) {
        Ticket t(n);
        std::thread worker(&Server::handle, this, std::move(t));     // ← the hand-over
        /* print what t holds now */
        worker.join();                                                // (join only so the demo can exit cleanly)
    }
};
```

(The move constructor uses <code>std::exchange</code>, exactly like <code>Socket</code>'s in <code>socket.cpp</code>.)

Actual output:

```
[TID 477] main thread starting
[TID 477] Ticket(7) created
[TID 477] main thread hands ticket 7 to a new thread
[TID 477] Ticket moved: new owner holds 7, old object now holds -1      ← move #1
[TID 477] main thread: after the hand-off, my t holds -1
[TID 478] Ticket moved: new owner holds 7, old object now holds -1      ← move #2
[TID 478] handle() working on ticket 7
[TID 478] ~Ticket: CLOSING 7                                            ← the real close
[TID 478] ~Ticket: empty, nothing to close
[TID 477] ~Ticket: empty, nothing to close
```
</div>

Line by line:

1. **`Ticket(7) created` on TID 477, the main thread.** Like `Socket(client_fd)` after `accept()`.
2. **Move #1 on TID 477, the main thread.** Inside `std::thread`'s constructor, *before* the new thread exists. `std::thread` moves the argument into its own private storage, which the new thread will own. That's why it has to happen on the main thread: it's the hand-over itself.
3. **`after the hand-off, my t holds -1`.** The main thread's `t` is now **empty**. When it's destroyed later it closes nothing. **This is what stops the main thread from accidentally closing the connection** (the spec's warning): after the move, it simply doesn't have it anymore.
4. **Move #2 on TID 478, the new thread.** The new thread moves the ticket out of that storage into `handle()`'s by-value parameter `t`, just as `Socket(client_fd)` is moved into `handle_client(Socket client)` today.
5. **`~Ticket: CLOSING 7` on TID 478.** `handle()` returned, its parameter was destroyed, and the "close" ran **on the worker thread**. In Pulse, this is `~Socket()` → `::close(fd)`, run by the worker when it's done with its client.
6. **Two `empty, nothing to close` lines.** The two moved-from shells (`std::thread`'s storage and the main thread's `t`) are destroyed, holding -1, so they do nothing.

**Exactly one `CLOSING 7`, on the thread that finished the work.** That's the Part 3 rule, enforced by the type system instead of by being careful.

### The C++ pieces in that hand-over line

```cpp
std::thread worker(&Server::handle, this, std::move(t));
```

| Piece | Meaning |
|---|---|
| `&Server::handle` | A **pointer to a member function**. `handle` isn't a free-standing function like `busy_work` in Experiment 2. It belongs to the class, so it must be called *on an object*. `&Server::handle` names "the `handle` function of class `Server`", without saying which `Server` yet |
| `this` | **Which object** to call it on. Inside a member function, `this` is a pointer to the current object. `std::thread` calls `this->handle(...)` on the new thread. Part 5 asks whether that's safe |
| `std::move(t)` | Marks `t` as "OK to move from" (it's a cast to an rvalue, nothing more, Month 1). This is what makes `std::thread` choose move #1 instead of a copy, which wouldn't compile for a move-only type |

**What if you forget the `std::move`?** `std::thread worker(&Server::handle, this, t);` tries to *copy* `t`, the copy constructor is deleted, and **it doesn't compile**. Annoying, but good: forgetting the hand-over is a compile error, not a runtime bug.

### Other ways to write the hand-over

You'll see these in real code. They give the same ownership result.

| Way | Looks like | Notes |
|---|---|---|
| **Member function + `this` + move** (above) | `std::thread(&C::f, this, std::move(s))` | Most direct. Ownership is visible in the call |
| **Lambda with a move-capture** (C++14 "init capture") | `std::thread([this, s = std::move(s)]() mutable { f(std::move(s)); })` | `s = std::move(s)` creates a new variable *inside* the lambda, moved from the outer one. `mutable` is needed because a lambda's captured variables are `const` by default, and you can't move out of a `const` |
| **Pass the raw `int`, wrap it in the thread** | `std::thread(&C::f_int, this, client_fd)` and `Socket` created inside | Works, but for a moment the fd is a bare `int` that nobody owns. If creating the thread fails (Part 6), it leaks. With the move version, a failed thread creation destroys the moved-in `Socket`, which closes the fd |

### `.detach()` on the end

Pulse won't `join()` its workers. The accept loop must never wait for a connection to finish (Week 1 doc, Part 8). So the thread is detached right after it's created. And it **has** to be detached (or joined) before the `std::thread` object is destroyed, otherwise `std::terminate()` (Week 1 doc, Part 8). In the accept loop the `std::thread` is a temporary, destroyed at the end of that same line, so forgetting `.detach()` crashes Pulse on the very first connection.

---

# Part 5 — `this`, `router_`, and how long things live

The worker thread runs `handle_client`, which uses `router_` (`response = router_.route(result.request);`). It reaches `router_` through `this`, a pointer to the `TcpServer` object. So the question every detached thread raises (Week 1 doc, Part 8): **is the thing it points to guaranteed to outlive the thread?**

Where does the `TcpServer` object live?

```cpp
int main(...) {
    ...
    TcpServer server(static_cast<uint16_t>(port), build_router());   // lives in main()'s stack frame
    server.run();                                                     // never returns
}
```

`server` lives in `main()`'s stack frame, on the **main thread's** stack. Detached workers read it through `this`, a pointer into another thread's stack (Week 1 doc, Part 4: stacks are private by convention, not by enforcement). That's safe **only because `run()` never returns**, so `server` is never destroyed while workers exist.

If Pulse ever gets a way to stop `run()` cleanly (a "shutdown" flag, which the spec lists under server-global state), this becomes a real bug: `main()` would return and destroy `server` while detached workers might still be using `router_`. Worth writing down now. It's a deliberate Month 2 simplification, not something to solve this week.

**Sharing `router_` between threads is fine, for now.** Every worker reads the same `router_` at the same time. Reading shared data from many threads is safe **as long as nobody writes to it**. `route()` is `const` and the routes are all registered in `build_router()` *before* `run()` starts. Week 3 adds a request counter that every thread *writes*, and that's where it stops being safe.

**The handlers themselves are now called concurrently.** Two `/echo` requests mean two threads inside the same lambda at once. That's safe because each call only uses its own `request` parameter and local variables (each on its own stack). A handler that touched a global would not be safe. That's Week 3.

**Ctrl+C now cuts off requests in progress.** Detached threads are killed instantly when the process exits (Week 1 doc, Part 8). Month 1 had at most one request in flight. Week 2 can have many. Acceptable for now. It's the same "graceful shutdown" problem as above.

---

# Part 6 — Errors on a worker thread

### Month 1 already has a hidden "one client kills the server" bug

Follow an error through today's code:

```cpp
// tcp_server.cpp — inside write_all(), called at the end of handle_client
ssize_t n = ::write(fd, data + sent, len - sent);
if (n < 0) { throw_errno("write"); }          // throws std::runtime_error

// handle_client doesn't catch it → run() doesn't catch it → it reaches main():
catch (const std::exception &e) {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;                                  // ← the WHOLE SERVER exits
}
```

So today, **one client causing a `write()` error** (for example, by disconnecting at the wrong moment) **shuts down Pulse for everyone**. It has never shown up because the tests never triggered a write error.

### With threads it gets worse: `main`'s `catch` can't even see it

An exception unwinds **its own thread's stack** looking for a `catch`. The `try/catch` in `main()` is on the **main thread's** stack. A worker thread's stack starts at the worker's own function. There's nothing above that, and nowhere further to look.

<div class="example">
<b>Real run: an exception on a worker thread, with a <code>try/catch</code> in <code>main</code>.</b>

```cpp
void work() { throw std::runtime_error("write: Broken pipe"); }

int main() {
    try {
        std::thread t(work);
        t.join();
    } catch (const std::exception& e) {
        std::printf("caught in main: %s\n", e.what());   // never reached
    }
    std::printf("main finished normally\n");            // never reached
}
```

Actual output:

```
terminate called after throwing an instance of 'std::runtime_error'
  what():  write: Broken pipe
Aborted
exit code: 134
```

The <code>catch</code> in <code>main</code> never ran. <b>An exception that escapes a thread's function calls <code>std::terminate()</code></b>, which aborts the whole process (exit code 134 = SIGABRT, the same as Week 1's missing-<code>join()</code> crash). In Week 2 Pulse, that means one client's write error kills every connection being served at that moment, not just Pulse's ability to accept new ones.
</div>

**The fix is a rule, not a trick:** the function a worker thread runs must not let any exception escape. Catch at the top of the worker, log it, and let that one connection end (its `Socket` destructor still closes the fd, because unwinding runs destructors):

```cpp
// the SHAPE of it, not Pulse's code:
void worker_entry(...) {
    try {
        // all the real work
    } catch (const std::exception& e) {
        // log e.what(); this connection is over, the server carries on
    }
}
```

`catch (const std::exception& e)` catches anything derived from `std::exception`, which includes `std::runtime_error` from `throw_errno`. `catch (...)` (three literal dots) catches *anything at all*. Some code adds it as a last resort after the `std::exception` one.

### One more process-wide killer: `SIGPIPE`

Experiment 0's "Noticed for later" note: writing to a socket whose other end has fully closed (sent a TCP reset) raises the **`SIGPIPE`** signal, and the default action for `SIGPIPE` is to **kill the process**. It's not an exception, so no `catch` stops it. Signals are delivered to the *process*, and the default action ends the whole process, all threads included.

With one thread, that's one crashed server. With thread-per-connection, Pulse is serving many clients at once, and any one of them disconnecting at the wrong moment could take all the others down. Two standard fixes, either one is enough:

| Fix | How | Effect |
|---|---|---|
| Ignore `SIGPIPE` for the whole process | `signal(SIGPIPE, SIG_IGN);` once at the start of `main()` (`#include <csignal>`) | `write()` returns `-1` with `errno == EPIPE` instead of killing the process, so Pulse's existing `throw_errno("write")` path handles it, now caught per worker |
| Ask for no signal on each send | `send(fd, data, len, MSG_NOSIGNAL)` instead of `write()` | Same effect, per call. Linux-specific flag |

So Week 2's error handling has two layers: **no exception escapes a worker**, and **no `SIGPIPE` kills the process**. Together, a bad client can only ever end its own connection.

---

# Part 7 — The `/slow` endpoint: proving it works

Experiment 0 used a *silent client* to block Pulse. The spec adds a second, more controllable way: a **slow handler**, an endpoint that deliberately takes several seconds.

```
GET /slow   → the handler sleeps ~5 s, then responds
GET /hello  → responds immediately
```

In `main.cpp`'s `build_router()`, a handler is a lambda (Month 1 / Week 2). A slow one just calls `std::this_thread::sleep_for(std::chrono::seconds(5))` before returning its response, the same call as Experiment 3.

**Predicted behavior:**

```
Month 1 (one thread):
  curl /slow   [======== handler sleeping 5 s ========][response]
  curl /hello                                            [waits in the backlog ......][response]   ← ~5 s

Week 2 (thread per connection):
  curl /slow   [======== worker A sleeping 5 s ========][response]
  curl /hello  [worker B: response]                                                                ← ~0.02 s
```

**Why the two ways of being slow are worth comparing:**

| | Silent client (Experiment 0) | `/slow` handler |
|---|---|---|
| Where the thread waits | Inside `read()`, waiting for the **client** | Inside the **handler**, after the request was fully read |
| `WCHAN` you'll see | `wait_woken` | `hrtimer_nanosleep`, exactly Experiment 3's sleeping thread, now inside Pulse |
| Who's "at fault" | The client (slow network, slow sender) | The server (slow database query, slow computation, in a real system) |
| Month 1 effect | Blocks everyone | Blocks everyone |
| Week 2 effect | Blocks only its own thread | Blocks only its own thread |

A real server sees both kinds. Thread-per-connection handles both the same way: whoever is slow only blocks their own thread.

**One thing `/slow` does NOT test:** CPU-heavy handlers. A handler that *computes* for 5 seconds (Experiment 2's busy loop) instead of sleeping would still not block other connections, but many of them at once would fill the CPUs, exactly like Experiment 2 Part C. Sleeping is the realistic case for Pulse. Busy is Week 4's stress question.

---

# Part 8 — What else changes, and what deliberately doesn't

### CMake: link the threads library

`std::thread` needs the POSIX threads library (Week 1 doc, Part 7). With raw `g++` that was the `-pthread` flag. In CMake it's two lines in `CMakeLists.txt`:

```cmake
find_package(Threads REQUIRED)                         # "find the platform's threads library"
target_link_libraries(pulse PRIVATE Threads::Threads)  # "link pulse against it"
```

`find_package` locates it (on Linux, pthreads). `Threads::Threads` is the name CMake gives it. `PRIVATE` means "pulse uses it internally", which is the same keyword as `target_include_directories(pulse PRIVATE include)` already in the file.

### Logs will interleave (expected, not fixed this week)

`handle_client` prints `"Client connected: ..."` and `"Parsed (...)"` with several `<<` pieces each. Experiment 1b showed pieces from different threads mixing. Under load, Pulse's log will get scrambled lines. The fix (a mutex) is Week 3. This week, scrambled logs are expected and harmless, and they're evidence that connections really are being handled at the same time.

### Several client fds open at once (new)

Month 1 Pulse only ever had fd 3 (the listening socket) and **one** client fd (always 4), because it closed each client before accepting the next. Week 2 Pulse will have several client fds open at once. You'll see them in `ls -l /proc/<PID>/fd` during the slow tests: 4, 5, 6… one per connection in progress. When a worker finishes, its fd closes and the number becomes free again. If the list ever only grows, ownership is broken somewhere (Part 3's "no owner" mistake).

### What deliberately does NOT change

| Stays the same | Why |
|---|---|
| `HttpParser`, `Router`, `HttpResponse`, the route handlers | They only work on their parameters and locals, so they're already safe to run on many threads at once (Part 5) |
| `handle_client`'s body: read loop, parse, route, write | Its locals already live on whichever thread runs it (Week 1 doc, Part 4). Only *which thread runs it* changes. (Part 6's error handling wraps around it) |
| `Connection: close`, one request per connection | Keep-alive is a separate topic |
| `TcpServer`'s constructor (socket/bind/listen) | Still runs once, on the main thread, before any worker exists |
| No limit on the number of threads | Deliberate. Week 4 measures what happens without one, and a controlled worker pool is Month 5 |

---

# Part 9 — How Week 2 will be tested

The detailed plan, every command explained, and predictions go in `Week2/notes.html` once the code is written, same as Week 1. The tests will be:

1. **Experiment 0, again.** The exact same commands as Week 1: silent `nc`, then `curl /hello`. Expected: `/hello` in ~0.02 s, `Threads:` ≥ 2, main thread in `inet_csk_accept`, nc's worker in `wait_woken`, LISTEN `Recv-Q 0`. Compared side by side with Week 1's numbers.
2. **`/slow` vs `/hello`.** Start `curl /slow`, then immediately `curl /hello`. Expected: `/hello` answers while `/slow` is still sleeping. `ps -L` shows a worker in `hrtimer_nanosleep`.
3. **Many slow requests at once.** Several `/slow` requests in parallel. Watch `Threads:` rise and fall, and `ls /proc/<PID>/fd` grow and shrink.
4. **Nothing broke.** Every Month 1 behavior still works: `/`, `/hello`, `/health`, `POST /echo`, 404, 405, 400 for malformed input, the slow-client partial-read test.
5. **A quick look with `ab -c 10`.** Just to see it still works under concurrency. The full Month 1 vs Month 2 benchmark is Week 4.

---

# Part 10 — Concept check

**In Month 1 Pulse, which line makes the main thread do all the work, and what does it become in Week 2?**
`handle_client(Socket(client_fd));` in `run()`. In Week 2, the main thread instead creates a detached thread that runs `handle_client` with the connection, and loops straight back to `accept()`.

**What should `ps -L` show for the main thread in Week 2, whatever clients are doing?**
`S` in `inet_csk_accept`: asleep in `accept()`, waiting for the next client. It never waits on a client's `read()` or a slow handler again.

**Who owns `client_fd` today, and who owns it after the Week 2 change?**
Today: the `Socket` moved into `handle_client`'s parameter, so the closing happens when `handle_client` returns, on the main thread. Week 2: the same `Socket` parameter, but on the worker thread's stack, so the worker closes it when it's done. The main thread owns nothing after the hand-over.

**Name the three ownership mistakes and their symptoms.**
Two owners (the main thread also closes it): the worker's fd closes under it, and fd reuse can send data to the wrong client. No owner (nobody closes): an fd leak, ending in `EMFILE` and `accept()` failing. Owner by reference (`[&]` capture): the worker uses whatever the main thread's variable holds later, the wrong client, or a dead variable.

**In the Ticket demo, why were there two moves, on two different threads?**
Move #1 happened on the main thread inside `std::thread`'s constructor, into the thread's private storage (the hand-over). Move #2 happened on the new thread, from that storage into `handle()`'s by-value parameter.

**Why does the main thread's `Socket` not close the connection after the hand-over?**
It was moved from. Its fd is now -1, so its destructor does nothing. The type makes "the main thread accidentally closes it" impossible.

**What happens if you write `std::thread(&TcpServer::handle_client, this, sock)` without `std::move`?**
It doesn't compile: `std::thread` would need to copy a move-only `Socket`, and the copy constructor is deleted.

**What are `&TcpServer::handle_client` and `this` doing in that call?**
`&TcpServer::handle_client` is a pointer to a member function: which function. `this` is the object to call it on. The new thread runs `this->handle_client(...)`.

**Why is it safe for detached workers to use `router_` through `this`?**
The `TcpServer` lives in `main()`'s stack frame, and `run()` never returns, so it's never destroyed while workers exist. And `router_` is only read, never modified, after `run()` starts.

**In Month 1, what happens if `write()` fails for one client? And in Week 2 without changes?**
Month 1: `throw_errno` throws, nothing catches it until `main()`, and the whole server exits with "Fatal error". Week 2: the exception escapes the worker thread's function, `std::terminate()` runs, and the whole process aborts, killing every connection.

**Why can't the `try/catch` in `main()` catch an exception from a worker thread?**
An exception only unwinds its own thread's stack. `main()`'s `try` is on the main thread's stack, and the worker's stack doesn't contain it.

**Why is `SIGPIPE` more dangerous with thread-per-connection, and what are the fixes?**
Its default action kills the whole process, and with many clients served at once, any one of them disconnecting at the wrong moment can take down all the others. Fix with `signal(SIGPIPE, SIG_IGN)` at startup, or `send(..., MSG_NOSIGNAL)`, so `write` returns `EPIPE` instead.

**How do the silent client and `/slow` differ in where the thread waits, and what does `WCHAN` show?**
Silent client: in `read()`, waiting for the client's bytes (`wait_woken`). `/slow`: in the handler, after reading the full request, sleeping (`hrtimer_nanosleep`). Both block only their own thread in Week 2.

**Is it safe for many threads to run the route handlers at the same time? When would it stop being safe?**
Yes, today: each handler only touches its own parameter and locals. It stops being safe when a handler (or `handle_client`) writes to shared state, like Week 3's request counter.

**What in `/proc/<PID>/fd` would tell you ownership is broken?**
The list of client fds only ever growing, never shrinking after connections finish: a leak, because nobody is closing.
